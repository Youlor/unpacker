/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rosalloc.h"

#include "base/memory_tool.h"
#include "base/mutex-inl.h"
#include "gc/space/memory_tool_settings.h"
#include "mem_map.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "thread-inl.h"
#include "thread_list.h"

#include <map>
#include <list>
#include <sstream>
#include <vector>

namespace art {
namespace gc {
namespace allocator {

static constexpr bool kUsePrefetchDuringAllocRun = false;
static constexpr bool kPrefetchNewRunDataByZeroing = false;
static constexpr size_t kPrefetchStride = 64;

size_t RosAlloc::bracketSizes[kNumOfSizeBrackets];
size_t RosAlloc::numOfPages[kNumOfSizeBrackets];
size_t RosAlloc::numOfSlots[kNumOfSizeBrackets];
size_t RosAlloc::headerSizes[kNumOfSizeBrackets];
bool RosAlloc::initialized_ = false;
size_t RosAlloc::dedicated_full_run_storage_[kPageSize / sizeof(size_t)] = { 0 };
RosAlloc::Run* RosAlloc::dedicated_full_run_ =
    reinterpret_cast<RosAlloc::Run*>(dedicated_full_run_storage_);

RosAlloc::RosAlloc(void* base, size_t capacity, size_t max_capacity,
                   PageReleaseMode page_release_mode, bool running_on_memory_tool,
                   size_t page_release_size_threshold)
    : base_(reinterpret_cast<uint8_t*>(base)), footprint_(capacity),
      capacity_(capacity), max_capacity_(max_capacity),
      lock_("rosalloc global lock", kRosAllocGlobalLock),
      bulk_free_lock_("rosalloc bulk free lock", kRosAllocBulkFreeLock),
      page_release_mode_(page_release_mode),
      page_release_size_threshold_(page_release_size_threshold),
      is_running_on_memory_tool_(running_on_memory_tool) {
  DCHECK_ALIGNED(base, kPageSize);
  DCHECK_EQ(RoundUp(capacity, kPageSize), capacity);
  DCHECK_EQ(RoundUp(max_capacity, kPageSize), max_capacity);
  CHECK_LE(capacity, max_capacity);
  CHECK_ALIGNED(page_release_size_threshold_, kPageSize);
  // Zero the memory explicitly (don't rely on that the mem map is zero-initialized).
  if (!kMadviseZeroes) {
    memset(base_, 0, max_capacity);
  }
  CHECK_EQ(madvise(base_, max_capacity, MADV_DONTNEED), 0);
  if (!initialized_) {
    Initialize();
  }
  VLOG(heap) << "RosAlloc base="
             << std::hex << (intptr_t)base_ << ", end="
             << std::hex << (intptr_t)(base_ + capacity_)
             << ", capacity=" << std::dec << capacity_
             << ", max_capacity=" << std::dec << max_capacity_;
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    size_bracket_lock_names_[i] =
        StringPrintf("an rosalloc size bracket %d lock", static_cast<int>(i));
    size_bracket_locks_[i] = new Mutex(size_bracket_lock_names_[i].c_str(), kRosAllocBracketLock);
    current_runs_[i] = dedicated_full_run_;
  }
  DCHECK_EQ(footprint_, capacity_);
  size_t num_of_pages = footprint_ / kPageSize;
  size_t max_num_of_pages = max_capacity_ / kPageSize;
  std::string error_msg;
  page_map_mem_map_.reset(MemMap::MapAnonymous("rosalloc page map", nullptr,
                                               RoundUp(max_num_of_pages, kPageSize),
                                               PROT_READ | PROT_WRITE, false, false, &error_msg));
  CHECK(page_map_mem_map_.get() != nullptr) << "Couldn't allocate the page map : " << error_msg;
  page_map_ = page_map_mem_map_->Begin();
  page_map_size_ = num_of_pages;
  max_page_map_size_ = max_num_of_pages;
  free_page_run_size_map_.resize(num_of_pages);
  FreePageRun* free_pages = reinterpret_cast<FreePageRun*>(base_);
  if (kIsDebugBuild) {
    free_pages->magic_num_ = kMagicNumFree;
  }
  free_pages->SetByteSize(this, capacity_);
  DCHECK_EQ(capacity_ % kPageSize, static_cast<size_t>(0));
  DCHECK(free_pages->IsFree());
  free_pages->ReleasePages(this);
  DCHECK(free_pages->IsFree());
  free_page_runs_.insert(free_pages);
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::RosAlloc() : Inserted run 0x" << std::hex
              << reinterpret_cast<intptr_t>(free_pages)
              << " into free_page_runs_";
  }
}

RosAlloc::~RosAlloc() {
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    delete size_bracket_locks_[i];
  }
  if (is_running_on_memory_tool_) {
    MEMORY_TOOL_MAKE_DEFINED(base_, capacity_);
  }
}

void* RosAlloc::AllocPages(Thread* self, size_t num_pages, uint8_t page_map_type) {
  lock_.AssertHeld(self);
  DCHECK(page_map_type == kPageMapRun || page_map_type == kPageMapLargeObject);
  FreePageRun* res = nullptr;
  const size_t req_byte_size = num_pages * kPageSize;
  // Find the lowest address free page run that's large enough.
  for (auto it = free_page_runs_.begin(); it != free_page_runs_.end(); ) {
    FreePageRun* fpr = *it;
    DCHECK(fpr->IsFree());
    size_t fpr_byte_size = fpr->ByteSize(this);
    DCHECK_EQ(fpr_byte_size % kPageSize, static_cast<size_t>(0));
    if (req_byte_size <= fpr_byte_size) {
      // Found one.
      free_page_runs_.erase(it++);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::AllocPages() : Erased run 0x"
                  << std::hex << reinterpret_cast<intptr_t>(fpr)
                  << " from free_page_runs_";
      }
      if (req_byte_size < fpr_byte_size) {
        // Split.
        FreePageRun* remainder = reinterpret_cast<FreePageRun*>(reinterpret_cast<uint8_t*>(fpr) + req_byte_size);
        if (kIsDebugBuild) {
          remainder->magic_num_ = kMagicNumFree;
        }
        remainder->SetByteSize(this, fpr_byte_size - req_byte_size);
        DCHECK_EQ(remainder->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        // Don't need to call madvise on remainder here.
        free_page_runs_.insert(remainder);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AllocPages() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(remainder)
                    << " into free_page_runs_";
        }
        fpr->SetByteSize(this, req_byte_size);
        DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
      }
      res = fpr;
      break;
    } else {
      ++it;
    }
  }

  // Failed to allocate pages. Grow the footprint, if possible.
  if (UNLIKELY(res == nullptr && capacity_ > footprint_)) {
    FreePageRun* last_free_page_run = nullptr;
    size_t last_free_page_run_size;
    auto it = free_page_runs_.rbegin();
    if (it != free_page_runs_.rend() && (last_free_page_run = *it)->End(this) == base_ + footprint_) {
      // There is a free page run at the end.
      DCHECK(last_free_page_run->IsFree());
      DCHECK(IsFreePage(ToPageMapIndex(last_free_page_run)));
      last_free_page_run_size = last_free_page_run->ByteSize(this);
    } else {
      // There is no free page run at the end.
      last_free_page_run_size = 0;
    }
    DCHECK_LT(last_free_page_run_size, req_byte_size);
    if (capacity_ - footprint_ + last_free_page_run_size >= req_byte_size) {
      // If we grow the heap, we can allocate it.
      size_t increment = std::min(std::max(2 * MB, req_byte_size - last_free_page_run_size),
                                  capacity_ - footprint_);
      DCHECK_EQ(increment % kPageSize, static_cast<size_t>(0));
      size_t new_footprint = footprint_ + increment;
      size_t new_num_of_pages = new_footprint / kPageSize;
      DCHECK_LT(page_map_size_, new_num_of_pages);
      DCHECK_LT(free_page_run_size_map_.size(), new_num_of_pages);
      page_map_size_ = new_num_of_pages;
      DCHECK_LE(page_map_size_, max_page_map_size_);
      free_page_run_size_map_.resize(new_num_of_pages);
      ArtRosAllocMoreCore(this, increment);
      if (last_free_page_run_size > 0) {
        // There was a free page run at the end. Expand its size.
        DCHECK_EQ(last_free_page_run_size, last_free_page_run->ByteSize(this));
        last_free_page_run->SetByteSize(this, last_free_page_run_size + increment);
        DCHECK_EQ(last_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        DCHECK_EQ(last_free_page_run->End(this), base_ + new_footprint);
      } else {
        // Otherwise, insert a new free page run at the end.
        FreePageRun* new_free_page_run = reinterpret_cast<FreePageRun*>(base_ + footprint_);
        if (kIsDebugBuild) {
          new_free_page_run->magic_num_ = kMagicNumFree;
        }
        new_free_page_run->SetByteSize(this, increment);
        DCHECK_EQ(new_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        free_page_runs_.insert(new_free_page_run);
        DCHECK_EQ(*free_page_runs_.rbegin(), new_free_page_run);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AlloPages() : Grew the heap by inserting run 0x"
                    << std::hex << reinterpret_cast<intptr_t>(new_free_page_run)
                    << " into free_page_runs_";
        }
      }
      DCHECK_LE(footprint_ + increment, capacity_);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::AllocPages() : increased the footprint from "
                  << footprint_ << " to " << new_footprint;
      }
      footprint_ = new_footprint;

      // And retry the last free page run.
      it = free_page_runs_.rbegin();
      DCHECK(it != free_page_runs_.rend());
      FreePageRun* fpr = *it;
      if (kIsDebugBuild && last_free_page_run_size > 0) {
        DCHECK(last_free_page_run != nullptr);
        DCHECK_EQ(last_free_page_run, fpr);
      }
      size_t fpr_byte_size = fpr->ByteSize(this);
      DCHECK_EQ(fpr_byte_size % kPageSize, static_cast<size_t>(0));
      DCHECK_LE(req_byte_size, fpr_byte_size);
      free_page_runs_.erase(fpr);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::AllocPages() : Erased run 0x" << std::hex << reinterpret_cast<intptr_t>(fpr)
                  << " from free_page_runs_";
      }
      if (req_byte_size < fpr_byte_size) {
        // Split if there's a remainder.
        FreePageRun* remainder = reinterpret_cast<FreePageRun*>(reinterpret_cast<uint8_t*>(fpr) + req_byte_size);
        if (kIsDebugBuild) {
          remainder->magic_num_ = kMagicNumFree;
        }
        remainder->SetByteSize(this, fpr_byte_size - req_byte_size);
        DCHECK_EQ(remainder->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        free_page_runs_.insert(remainder);
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::AllocPages() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(remainder)
                    << " into free_page_runs_";
        }
        fpr->SetByteSize(this, req_byte_size);
        DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
      }
      res = fpr;
    }
  }
  if (LIKELY(res != nullptr)) {
    // Update the page map.
    size_t page_map_idx = ToPageMapIndex(res);
    for (size_t i = 0; i < num_pages; i++) {
      DCHECK(IsFreePage(page_map_idx + i));
    }
    switch (page_map_type) {
    case kPageMapRun:
      page_map_[page_map_idx] = kPageMapRun;
      for (size_t i = 1; i < num_pages; i++) {
        page_map_[page_map_idx + i] = kPageMapRunPart;
      }
      break;
    case kPageMapLargeObject:
      page_map_[page_map_idx] = kPageMapLargeObject;
      for (size_t i = 1; i < num_pages; i++) {
        page_map_[page_map_idx + i] = kPageMapLargeObjectPart;
      }
      break;
    default:
      LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_type);
      break;
    }
    if (kIsDebugBuild) {
      // Clear the first page since it is not madvised due to the magic number.
      memset(res, 0, kPageSize);
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocPages() : 0x" << std::hex << reinterpret_cast<intptr_t>(res)
                << "-0x" << (reinterpret_cast<intptr_t>(res) + num_pages * kPageSize)
                << "(" << std::dec << (num_pages * kPageSize) << ")";
    }
    return res;
  }

  // Fail.
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::AllocPages() : nullptr";
  }
  return nullptr;
}

size_t RosAlloc::FreePages(Thread* self, void* ptr, bool already_zero) {
  lock_.AssertHeld(self);
  size_t pm_idx = ToPageMapIndex(ptr);
  DCHECK_LT(pm_idx, page_map_size_);
  uint8_t pm_type = page_map_[pm_idx];
  DCHECK(pm_type == kPageMapRun || pm_type == kPageMapLargeObject);
  uint8_t pm_part_type;
  switch (pm_type) {
  case kPageMapRun:
    pm_part_type = kPageMapRunPart;
    break;
  case kPageMapLargeObject:
    pm_part_type = kPageMapLargeObjectPart;
    break;
  default:
    LOG(FATAL) << "Unreachable - " << __PRETTY_FUNCTION__ << " : " << "pm_idx=" << pm_idx << ", pm_type="
               << static_cast<int>(pm_type) << ", ptr=" << std::hex
               << reinterpret_cast<intptr_t>(ptr);
    return 0;
  }
  // Update the page map and count the number of pages.
  size_t num_pages = 1;
  page_map_[pm_idx] = kPageMapEmpty;
  size_t idx = pm_idx + 1;
  size_t end = page_map_size_;
  while (idx < end && page_map_[idx] == pm_part_type) {
    page_map_[idx] = kPageMapEmpty;
    num_pages++;
    idx++;
  }
  const size_t byte_size = num_pages * kPageSize;
  if (already_zero) {
    if (ShouldCheckZeroMemory()) {
      const uintptr_t* word_ptr = reinterpret_cast<uintptr_t*>(ptr);
      for (size_t i = 0; i < byte_size / sizeof(uintptr_t); ++i) {
        CHECK_EQ(word_ptr[i], 0U) << "words don't match at index " << i;
      }
    }
  } else if (!DoesReleaseAllPages()) {
    memset(ptr, 0, byte_size);
  }

  if (kTraceRosAlloc) {
    LOG(INFO) << __PRETTY_FUNCTION__ << " : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr)
              << "-0x" << (reinterpret_cast<intptr_t>(ptr) + byte_size)
              << "(" << std::dec << (num_pages * kPageSize) << ")";
  }

  // Turn it into a free run.
  FreePageRun* fpr = reinterpret_cast<FreePageRun*>(ptr);
  if (kIsDebugBuild) {
    fpr->magic_num_ = kMagicNumFree;
  }
  fpr->SetByteSize(this, byte_size);
  DCHECK_ALIGNED(fpr->ByteSize(this), kPageSize);

  DCHECK(free_page_runs_.find(fpr) == free_page_runs_.end());
  if (!free_page_runs_.empty()) {
    // Try to coalesce in the higher address direction.
    if (kTraceRosAlloc) {
      LOG(INFO) << __PRETTY_FUNCTION__ << "RosAlloc::FreePages() : trying to coalesce a free page run 0x"
                << std::hex << reinterpret_cast<uintptr_t>(fpr) << " [" << std::dec << pm_idx << "] -0x"
                << std::hex << reinterpret_cast<uintptr_t>(fpr->End(this)) << " [" << std::dec
                << (fpr->End(this) == End() ? page_map_size_ : ToPageMapIndex(fpr->End(this))) << "]";
    }
    auto higher_it = free_page_runs_.upper_bound(fpr);
    if (higher_it != free_page_runs_.end()) {
      for (auto it = higher_it; it != free_page_runs_.end(); ) {
        FreePageRun* h = *it;
        DCHECK_EQ(h->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::FreePages() : trying to coalesce with a higher free page run 0x"
                    << std::hex << reinterpret_cast<uintptr_t>(h) << " [" << std::dec << ToPageMapIndex(h) << "] -0x"
                    << std::hex << reinterpret_cast<uintptr_t>(h->End(this)) << " [" << std::dec
                    << (h->End(this) == End() ? page_map_size_ : ToPageMapIndex(h->End(this))) << "]";
        }
        if (fpr->End(this) == h->Begin()) {
          if (kTraceRosAlloc) {
            LOG(INFO) << "Success";
          }
          // Clear magic num since this is no longer the start of a free page run.
          if (kIsDebugBuild) {
            h->magic_num_ = 0;
          }
          free_page_runs_.erase(it++);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::FreePages() : (coalesce) Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(h)
                      << " from free_page_runs_";
          }
          fpr->SetByteSize(this, fpr->ByteSize(this) + h->ByteSize(this));
          DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        } else {
          // Not adjacent. Stop.
          if (kTraceRosAlloc) {
            LOG(INFO) << "Fail";
          }
          break;
        }
      }
    }
    // Try to coalesce in the lower address direction.
    auto lower_it = free_page_runs_.upper_bound(fpr);
    if (lower_it != free_page_runs_.begin()) {
      --lower_it;
      for (auto it = lower_it; ; ) {
        // We want to try to coalesce with the first element but
        // there's no "<=" operator for the iterator.
        bool to_exit_loop = it == free_page_runs_.begin();

        FreePageRun* l = *it;
        DCHECK_EQ(l->ByteSize(this) % kPageSize, static_cast<size_t>(0));
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::FreePages() : trying to coalesce with a lower free page run 0x"
                    << std::hex << reinterpret_cast<uintptr_t>(l) << " [" << std::dec << ToPageMapIndex(l) << "] -0x"
                    << std::hex << reinterpret_cast<uintptr_t>(l->End(this)) << " [" << std::dec
                    << (l->End(this) == End() ? page_map_size_ : ToPageMapIndex(l->End(this))) << "]";
        }
        if (l->End(this) == fpr->Begin()) {
          if (kTraceRosAlloc) {
            LOG(INFO) << "Success";
          }
          free_page_runs_.erase(it--);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::FreePages() : (coalesce) Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(l)
                      << " from free_page_runs_";
          }
          l->SetByteSize(this, l->ByteSize(this) + fpr->ByteSize(this));
          DCHECK_EQ(l->ByteSize(this) % kPageSize, static_cast<size_t>(0));
          // Clear magic num since this is no longer the start of a free page run.
          if (kIsDebugBuild) {
            fpr->magic_num_ = 0;
          }
          fpr = l;
        } else {
          // Not adjacent. Stop.
          if (kTraceRosAlloc) {
            LOG(INFO) << "Fail";
          }
          break;
        }
        if (to_exit_loop) {
          break;
        }
      }
    }
  }

  // Insert it.
  DCHECK_EQ(fpr->ByteSize(this) % kPageSize, static_cast<size_t>(0));
  DCHECK(free_page_runs_.find(fpr) == free_page_runs_.end());
  DCHECK(fpr->IsFree());
  fpr->ReleasePages(this);
  DCHECK(fpr->IsFree());
  free_page_runs_.insert(fpr);
  DCHECK(free_page_runs_.find(fpr) != free_page_runs_.end());
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreePages() : Inserted run 0x" << std::hex << reinterpret_cast<intptr_t>(fpr)
              << " into free_page_runs_";
  }
  return byte_size;
}

void* RosAlloc::AllocLargeObject(Thread* self, size_t size, size_t* bytes_allocated,
                                 size_t* usable_size, size_t* bytes_tl_bulk_allocated) {
  DCHECK(bytes_allocated != nullptr);
  DCHECK(usable_size != nullptr);
  DCHECK_GT(size, kLargeSizeThreshold);
  size_t num_pages = RoundUp(size, kPageSize) / kPageSize;
  void* r;
  {
    MutexLock mu(self, lock_);
    r = AllocPages(self, num_pages, kPageMapLargeObject);
  }
  if (UNLIKELY(r == nullptr)) {
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocLargeObject() : nullptr";
    }
    return nullptr;
  }
  const size_t total_bytes = num_pages * kPageSize;
  *bytes_allocated = total_bytes;
  *usable_size = total_bytes;
  *bytes_tl_bulk_allocated = total_bytes;
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::AllocLargeObject() : 0x" << std::hex << reinterpret_cast<intptr_t>(r)
              << "-0x" << (reinterpret_cast<intptr_t>(r) + num_pages * kPageSize)
              << "(" << std::dec << (num_pages * kPageSize) << ")";
  }
  // Check if the returned memory is really all zero.
  if (ShouldCheckZeroMemory()) {
    CHECK_EQ(total_bytes % sizeof(uintptr_t), 0U);
    const uintptr_t* words = reinterpret_cast<uintptr_t*>(r);
    for (size_t i = 0; i < total_bytes / sizeof(uintptr_t); ++i) {
      CHECK_EQ(words[i], 0U);
    }
  }
  return r;
}

size_t RosAlloc::FreeInternal(Thread* self, void* ptr) {
  DCHECK_LE(base_, ptr);
  DCHECK_LT(ptr, base_ + footprint_);
  size_t pm_idx = RoundDownToPageMapIndex(ptr);
  Run* run = nullptr;
  {
    MutexLock mu(self, lock_);
    DCHECK_LT(pm_idx, page_map_size_);
    uint8_t page_map_entry = page_map_[pm_idx];
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreeInternal() : " << std::hex << ptr << ", pm_idx=" << std::dec << pm_idx
                << ", page_map_entry=" << static_cast<int>(page_map_entry);
    }
    switch (page_map_[pm_idx]) {
      case kPageMapLargeObject:
        return FreePages(self, ptr, false);
      case kPageMapLargeObjectPart:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_[pm_idx]);
        return 0;
      case kPageMapRunPart: {
        // Find the beginning of the run.
        do {
          --pm_idx;
          DCHECK_LT(pm_idx, capacity_ / kPageSize);
        } while (page_map_[pm_idx] != kPageMapRun);
        FALLTHROUGH_INTENDED;
      case kPageMapRun:
        run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
        DCHECK_EQ(run->magic_num_, kMagicNum);
        break;
      case kPageMapReleased:
      case kPageMapEmpty:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_[pm_idx]);
        return 0;
      }
      default:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_[pm_idx]);
        return 0;
    }
  }
  DCHECK(run != nullptr);
  return FreeFromRun(self, ptr, run);
}

size_t RosAlloc::Free(Thread* self, void* ptr) {
  ReaderMutexLock rmu(self, bulk_free_lock_);
  return FreeInternal(self, ptr);
}

RosAlloc::Run* RosAlloc::AllocRun(Thread* self, size_t idx) {
  RosAlloc::Run* new_run = nullptr;
  {
    MutexLock mu(self, lock_);
    new_run = reinterpret_cast<Run*>(AllocPages(self, numOfPages[idx], kPageMapRun));
  }
  if (LIKELY(new_run != nullptr)) {
    if (kIsDebugBuild) {
      new_run->magic_num_ = kMagicNum;
    }
    new_run->size_bracket_idx_ = idx;
    DCHECK(!new_run->IsThreadLocal());
    DCHECK(!new_run->to_be_bulk_freed_);
    if (kUsePrefetchDuringAllocRun && idx < kNumThreadLocalSizeBrackets) {
      // Take ownership of the cache lines if we are likely to be thread local run.
      if (kPrefetchNewRunDataByZeroing) {
        // Zeroing the data is sometimes faster than prefetching but it increases memory usage
        // since we end up dirtying zero pages which may have been madvised.
        new_run->ZeroData();
      } else {
        const size_t num_of_slots = numOfSlots[idx];
        const size_t bracket_size = bracketSizes[idx];
        const size_t num_of_bytes = num_of_slots * bracket_size;
        uint8_t* begin = reinterpret_cast<uint8_t*>(new_run) + headerSizes[idx];
        for (size_t i = 0; i < num_of_bytes; i += kPrefetchStride) {
          __builtin_prefetch(begin + i);
        }
      }
    }
    new_run->InitFreeList();
  }
  return new_run;
}

RosAlloc::Run* RosAlloc::RefillRun(Thread* self, size_t idx) {
  // Get the lowest address non-full run from the binary tree.
  auto* const bt = &non_full_runs_[idx];
  if (!bt->empty()) {
    // If there's one, use it as the current run.
    auto it = bt->begin();
    Run* non_full_run = *it;
    DCHECK(non_full_run != nullptr);
    DCHECK(!non_full_run->IsThreadLocal());
    bt->erase(it);
    return non_full_run;
  }
  // If there's none, allocate a new run and use it as the current run.
  return AllocRun(self, idx);
}

inline void* RosAlloc::AllocFromCurrentRunUnlocked(Thread* self, size_t idx) {
  Run* current_run = current_runs_[idx];
  DCHECK(current_run != nullptr);
  void* slot_addr = current_run->AllocSlot();
  if (UNLIKELY(slot_addr == nullptr)) {
    // The current run got full. Try to refill it.
    DCHECK(current_run->IsFull());
    if (kIsDebugBuild && current_run != dedicated_full_run_) {
      full_runs_[idx].insert(current_run);
      if (kTraceRosAlloc) {
        LOG(INFO) << __PRETTY_FUNCTION__ << " : Inserted run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(current_run)
                  << " into full_runs_[" << std::dec << idx << "]";
      }
      DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(current_run) != full_runs_[idx].end());
    }
    current_run = RefillRun(self, idx);
    if (UNLIKELY(current_run == nullptr)) {
      // Failed to allocate a new run, make sure that it is the dedicated full run.
      current_runs_[idx] = dedicated_full_run_;
      return nullptr;
    }
    DCHECK(current_run != nullptr);
    DCHECK(non_full_runs_[idx].find(current_run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(current_run) == full_runs_[idx].end());
    current_run->SetIsThreadLocal(false);
    current_runs_[idx] = current_run;
    DCHECK(!current_run->IsFull());
    slot_addr = current_run->AllocSlot();
    // Must succeed now with a new run.
    DCHECK(slot_addr != nullptr);
  }
  return slot_addr;
}

void* RosAlloc::AllocFromRunThreadUnsafe(Thread* self, size_t size, size_t* bytes_allocated,
                                         size_t* usable_size,
                                         size_t* bytes_tl_bulk_allocated) {
  DCHECK(bytes_allocated != nullptr);
  DCHECK(usable_size != nullptr);
  DCHECK(bytes_tl_bulk_allocated != nullptr);
  DCHECK_LE(size, kLargeSizeThreshold);
  size_t bracket_size;
  size_t idx = SizeToIndexAndBracketSize(size, &bracket_size);
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  void* slot_addr = AllocFromCurrentRunUnlocked(self, idx);
  if (LIKELY(slot_addr != nullptr)) {
    *bytes_allocated = bracket_size;
    *usable_size = bracket_size;
    *bytes_tl_bulk_allocated = bracket_size;
  }
  // Caller verifies that it is all 0.
  return slot_addr;
}

void* RosAlloc::AllocFromRun(Thread* self, size_t size, size_t* bytes_allocated,
                             size_t* usable_size, size_t* bytes_tl_bulk_allocated) {
  DCHECK(bytes_allocated != nullptr);
  DCHECK(usable_size != nullptr);
  DCHECK(bytes_tl_bulk_allocated != nullptr);
  DCHECK_LE(size, kLargeSizeThreshold);
  size_t bracket_size;
  size_t idx = SizeToIndexAndBracketSize(size, &bracket_size);
  void* slot_addr;
  if (LIKELY(idx < kNumThreadLocalSizeBrackets)) {
    // Use a thread-local run.
    Run* thread_local_run = reinterpret_cast<Run*>(self->GetRosAllocRun(idx));
    // Allow invalid since this will always fail the allocation.
    if (kIsDebugBuild) {
      // Need the lock to prevent race conditions.
      MutexLock mu(self, *size_bracket_locks_[idx]);
      CHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
      CHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
    }
    DCHECK(thread_local_run != nullptr);
    DCHECK(thread_local_run->IsThreadLocal() || thread_local_run == dedicated_full_run_);
    slot_addr = thread_local_run->AllocSlot();
    // The allocation must fail if the run is invalid.
    DCHECK(thread_local_run != dedicated_full_run_ || slot_addr == nullptr)
        << "allocated from an invalid run";
    if (UNLIKELY(slot_addr == nullptr)) {
      // The run got full. Try to free slots.
      DCHECK(thread_local_run->IsFull());
      MutexLock mu(self, *size_bracket_locks_[idx]);
      bool is_all_free_after_merge;
      // This is safe to do for the dedicated_full_run_ since the bitmaps are empty.
      if (thread_local_run->MergeThreadLocalFreeListToFreeList(&is_all_free_after_merge)) {
        DCHECK_NE(thread_local_run, dedicated_full_run_);
        // Some slot got freed. Keep it.
        DCHECK(!thread_local_run->IsFull());
        DCHECK_EQ(is_all_free_after_merge, thread_local_run->IsAllFree());
      } else {
        // No slots got freed. Try to refill the thread-local run.
        DCHECK(thread_local_run->IsFull());
        if (thread_local_run != dedicated_full_run_) {
          thread_local_run->SetIsThreadLocal(false);
          if (kIsDebugBuild) {
            full_runs_[idx].insert(thread_local_run);
            if (kTraceRosAlloc) {
              LOG(INFO) << "RosAlloc::AllocFromRun() : Inserted run 0x" << std::hex
                        << reinterpret_cast<intptr_t>(thread_local_run)
                        << " into full_runs_[" << std::dec << idx << "]";
            }
          }
          DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
          DCHECK(full_runs_[idx].find(thread_local_run) != full_runs_[idx].end());
        }

        thread_local_run = RefillRun(self, idx);
        if (UNLIKELY(thread_local_run == nullptr)) {
          self->SetRosAllocRun(idx, dedicated_full_run_);
          return nullptr;
        }
        DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
        DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
        thread_local_run->SetIsThreadLocal(true);
        self->SetRosAllocRun(idx, thread_local_run);
        DCHECK(!thread_local_run->IsFull());
      }
      DCHECK(thread_local_run != nullptr);
      DCHECK(!thread_local_run->IsFull());
      DCHECK(thread_local_run->IsThreadLocal());
      // Account for all the free slots in the new or refreshed thread local run.
      *bytes_tl_bulk_allocated = thread_local_run->NumberOfFreeSlots() * bracket_size;
      slot_addr = thread_local_run->AllocSlot();
      // Must succeed now with a new run.
      DCHECK(slot_addr != nullptr);
    } else {
      // The slot is already counted. Leave it as is.
      *bytes_tl_bulk_allocated = 0;
    }
    DCHECK(slot_addr != nullptr);
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocFromRun() thread-local : 0x" << std::hex
                << reinterpret_cast<intptr_t>(slot_addr)
                << "-0x" << (reinterpret_cast<intptr_t>(slot_addr) + bracket_size)
                << "(" << std::dec << (bracket_size) << ")";
    }
    *bytes_allocated = bracket_size;
    *usable_size = bracket_size;
  } else {
    // Use the (shared) current run.
    MutexLock mu(self, *size_bracket_locks_[idx]);
    slot_addr = AllocFromCurrentRunUnlocked(self, idx);
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::AllocFromRun() : 0x" << std::hex
                << reinterpret_cast<intptr_t>(slot_addr)
                << "-0x" << (reinterpret_cast<intptr_t>(slot_addr) + bracket_size)
                << "(" << std::dec << (bracket_size) << ")";
    }
    if (LIKELY(slot_addr != nullptr)) {
      *bytes_allocated = bracket_size;
      *usable_size = bracket_size;
      *bytes_tl_bulk_allocated = bracket_size;
    }
  }
  // Caller verifies that it is all 0.
  return slot_addr;
}

size_t RosAlloc::FreeFromRun(Thread* self, void* ptr, Run* run) {
  DCHECK_EQ(run->magic_num_, kMagicNum);
  DCHECK_LT(run, ptr);
  DCHECK_LT(ptr, run->End());
  const size_t idx = run->size_bracket_idx_;
  const size_t bracket_size = bracketSizes[idx];
  bool run_was_full = false;
  MutexLock brackets_mu(self, *size_bracket_locks_[idx]);
  if (kIsDebugBuild) {
    run_was_full = run->IsFull();
  }
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::FreeFromRun() : 0x" << std::hex << reinterpret_cast<intptr_t>(ptr);
  }
  if (LIKELY(run->IsThreadLocal())) {
    // It's a thread-local run. Just mark the thread-local free bit map and return.
    DCHECK_LT(run->size_bracket_idx_, kNumThreadLocalSizeBrackets);
    DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
    run->AddToThreadLocalFreeList(ptr);
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::FreeFromRun() : Freed a slot in a thread local run 0x" << std::hex
                << reinterpret_cast<intptr_t>(run);
    }
    // A thread local run will be kept as a thread local even if it's become all free.
    return bracket_size;
  }
  // Free the slot in the run.
  run->FreeSlot(ptr);
  auto* non_full_runs = &non_full_runs_[idx];
  if (run->IsAllFree()) {
    // It has just become completely free. Free the pages of this run.
    std::set<Run*>::iterator pos = non_full_runs->find(run);
    if (pos != non_full_runs->end()) {
      non_full_runs->erase(pos);
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::FreeFromRun() : Erased run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(run) << " from non_full_runs_";
      }
    }
    if (run == current_runs_[idx]) {
      current_runs_[idx] = dedicated_full_run_;
    }
    DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
    DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
    run->ZeroHeaderAndSlotHeaders();
    {
      MutexLock lock_mu(self, lock_);
      FreePages(self, run, true);
    }
  } else {
    // It is not completely free. If it wasn't the current run or
    // already in the non-full run set (i.e., it was full) insert it
    // into the non-full run set.
    if (run != current_runs_[idx]) {
      auto* full_runs = kIsDebugBuild ? &full_runs_[idx] : nullptr;
      auto pos = non_full_runs->find(run);
      if (pos == non_full_runs->end()) {
        DCHECK(run_was_full);
        DCHECK(full_runs->find(run) != full_runs->end());
        if (kIsDebugBuild) {
          full_runs->erase(run);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::FreeFromRun() : Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(run) << " from full_runs_";
          }
        }
        non_full_runs->insert(run);
        DCHECK(!run->IsFull());
        if (kTraceRosAlloc) {
          LOG(INFO) << "RosAlloc::FreeFromRun() : Inserted run 0x" << std::hex
                    << reinterpret_cast<intptr_t>(run)
                    << " into non_full_runs_[" << std::dec << idx << "]";
        }
      }
    }
  }
  return bracket_size;
}

template<bool kUseTail>
std::string RosAlloc::Run::FreeListToStr(SlotFreeList<kUseTail>* free_list) {
  std::string free_list_str;
  const uint8_t idx = size_bracket_idx_;
  const size_t bracket_size = bracketSizes[idx];
  for (Slot* slot = free_list->Head(); slot != nullptr; slot = slot->Next()) {
    bool is_last = slot->Next() == nullptr;
    uintptr_t slot_offset = reinterpret_cast<uintptr_t>(slot) -
        reinterpret_cast<uintptr_t>(FirstSlot());
    DCHECK_EQ(slot_offset % bracket_size, 0U);
    uintptr_t slot_idx = slot_offset / bracket_size;
    if (!is_last) {
      free_list_str.append(StringPrintf("%u-", static_cast<uint32_t>(slot_idx)));
    } else {
      free_list_str.append(StringPrintf("%u", static_cast<uint32_t>(slot_idx)));
    }
  }
  return free_list_str;
}

std::string RosAlloc::Run::Dump() {
  size_t idx = size_bracket_idx_;
  std::ostringstream stream;
  stream << "RosAlloc Run = " << reinterpret_cast<void*>(this)
         << "{ magic_num=" << static_cast<int>(magic_num_)
         << " size_bracket_idx=" << idx
         << " is_thread_local=" << static_cast<int>(is_thread_local_)
         << " to_be_bulk_freed=" << static_cast<int>(to_be_bulk_freed_)
         << " free_list=" << FreeListToStr(&free_list_)
         << " bulk_free_list=" << FreeListToStr(&bulk_free_list_)
         << " thread_local_list=" << FreeListToStr(&thread_local_free_list_)
         << " }" << std::endl;
  return stream.str();
}

void RosAlloc::Run::FreeSlot(void* ptr) {
  DCHECK(!IsThreadLocal());
  const uint8_t idx = size_bracket_idx_;
  const size_t bracket_size = bracketSizes[idx];
  Slot* slot = ToSlot(ptr);
  // Zero out the memory.
  // TODO: Investigate alternate memset since ptr is guaranteed to be aligned to 16.
  memset(slot, 0, bracket_size);
  free_list_.Add(slot);
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::Run::FreeSlot() : " << slot
              << ", bracket_size=" << std::dec << bracket_size << ", slot_idx=" << SlotIndex(slot);
  }
}

inline bool RosAlloc::Run::MergeThreadLocalFreeListToFreeList(bool* is_all_free_after_out) {
  DCHECK(IsThreadLocal());
  // Merge the thread local free list into the free list and clear the thread local free list.
  const uint8_t idx = size_bracket_idx_;
  bool thread_local_free_list_size = thread_local_free_list_.Size();
  const size_t size_before = free_list_.Size();
  free_list_.Merge(&thread_local_free_list_);
  const size_t size_after = free_list_.Size();
  DCHECK_EQ(size_before < size_after, thread_local_free_list_size > 0);
  DCHECK_LE(size_before, size_after);
  *is_all_free_after_out = free_list_.Size() == numOfSlots[idx];
  // Return true at least one slot was added to the free list.
  return size_before < size_after;
}

inline void RosAlloc::Run::MergeBulkFreeListToFreeList() {
  DCHECK(!IsThreadLocal());
  // Merge the bulk free list into the free list and clear the bulk free list.
  free_list_.Merge(&bulk_free_list_);
}

inline void RosAlloc::Run::MergeBulkFreeListToThreadLocalFreeList() {
  DCHECK(IsThreadLocal());
  // Merge the bulk free list into the thread local free list and clear the bulk free list.
  thread_local_free_list_.Merge(&bulk_free_list_);
}

inline void RosAlloc::Run::AddToThreadLocalFreeList(void* ptr) {
  DCHECK(IsThreadLocal());
  AddToFreeListShared(ptr, &thread_local_free_list_, __FUNCTION__);
}

inline size_t RosAlloc::Run::AddToBulkFreeList(void* ptr) {
  return AddToFreeListShared(ptr, &bulk_free_list_, __FUNCTION__);
}

inline size_t RosAlloc::Run::AddToFreeListShared(void* ptr,
                                                 SlotFreeList<true>* free_list,
                                                 const char* caller_name) {
  const uint8_t idx = size_bracket_idx_;
  const size_t bracket_size = bracketSizes[idx];
  Slot* slot = ToSlot(ptr);
  memset(slot, 0, bracket_size);
  free_list->Add(slot);
  if (kTraceRosAlloc) {
    LOG(INFO) << "RosAlloc::Run::" << caller_name << "() : " << ptr
              << ", bracket_size=" << std::dec << bracket_size << ", slot_idx=" << SlotIndex(slot);
  }
  return bracket_size;
}

inline void RosAlloc::Run::ZeroHeaderAndSlotHeaders() {
  DCHECK(IsAllFree());
  const uint8_t idx = size_bracket_idx_;
  // Zero the slot header (next pointers).
  for (Slot* slot = free_list_.Head(); slot != nullptr; ) {
    Slot* next_slot = slot->Next();
    slot->Clear();
    slot = next_slot;
  }
  // Zero the header.
  memset(this, 0, headerSizes[idx]);
  // Check that the entire run is all zero.
  if (kIsDebugBuild) {
    const size_t size = numOfPages[idx] * kPageSize;
    const uintptr_t* word_ptr = reinterpret_cast<uintptr_t*>(this);
    for (size_t i = 0; i < size / sizeof(uintptr_t); ++i) {
      CHECK_EQ(word_ptr[i], 0U) << "words don't match at index " << i;
    }
  }
}

inline void RosAlloc::Run::ZeroData() {
  const uint8_t idx = size_bracket_idx_;
  uint8_t* slot_begin = reinterpret_cast<uint8_t*>(FirstSlot());
  memset(slot_begin, 0, numOfSlots[idx] * bracketSizes[idx]);
}

void RosAlloc::Run::InspectAllSlots(void (*handler)(void* start, void* end, size_t used_bytes, void* callback_arg),
                                    void* arg) {
  size_t idx = size_bracket_idx_;
  uint8_t* slot_base = reinterpret_cast<uint8_t*>(this) + headerSizes[idx];
  size_t num_slots = numOfSlots[idx];
  size_t bracket_size = IndexToBracketSize(idx);
  DCHECK_EQ(slot_base + num_slots * bracket_size,
            reinterpret_cast<uint8_t*>(this) + numOfPages[idx] * kPageSize);
  // Free slots are on the free list and the allocated/used slots are not. We traverse the free list
  // to find out and record which slots are free in the is_free array.
  std::unique_ptr<bool[]> is_free(new bool[num_slots]());  // zero initialized
  for (Slot* slot = free_list_.Head(); slot != nullptr; slot = slot->Next()) {
    size_t slot_idx = SlotIndex(slot);
    DCHECK_LT(slot_idx, num_slots);
    is_free[slot_idx] = true;
  }
  if (IsThreadLocal()) {
    for (Slot* slot = thread_local_free_list_.Head(); slot != nullptr; slot = slot->Next()) {
      size_t slot_idx = SlotIndex(slot);
      DCHECK_LT(slot_idx, num_slots);
      is_free[slot_idx] = true;
    }
  }
  for (size_t slot_idx = 0; slot_idx < num_slots; ++slot_idx) {
    uint8_t* slot_addr = slot_base + slot_idx * bracket_size;
    if (!is_free[slot_idx]) {
      handler(slot_addr, slot_addr + bracket_size, bracket_size, arg);
    } else {
      handler(slot_addr, slot_addr + bracket_size, 0, arg);
    }
  }
}

// If true, read the page map entries in BulkFree() without using the
// lock for better performance, assuming that the existence of an
// allocated chunk/pointer being freed in BulkFree() guarantees that
// the page map entry won't change. Disabled for now.
static constexpr bool kReadPageMapEntryWithoutLockInBulkFree = true;

size_t RosAlloc::BulkFree(Thread* self, void** ptrs, size_t num_ptrs) {
  size_t freed_bytes = 0;
  if ((false)) {
    // Used only to test Free() as GC uses only BulkFree().
    for (size_t i = 0; i < num_ptrs; ++i) {
      freed_bytes += FreeInternal(self, ptrs[i]);
    }
    return freed_bytes;
  }

  WriterMutexLock wmu(self, bulk_free_lock_);

  // First mark slots to free in the bulk free bit map without locking the
  // size bracket locks. On host, unordered_set is faster than vector + flag.
#ifdef __ANDROID__
  std::vector<Run*> runs;
#else
  std::unordered_set<Run*, hash_run, eq_run> runs;
#endif
  for (size_t i = 0; i < num_ptrs; i++) {
    void* ptr = ptrs[i];
    DCHECK_LE(base_, ptr);
    DCHECK_LT(ptr, base_ + footprint_);
    size_t pm_idx = RoundDownToPageMapIndex(ptr);
    Run* run = nullptr;
    if (kReadPageMapEntryWithoutLockInBulkFree) {
      // Read the page map entries without locking the lock.
      uint8_t page_map_entry = page_map_[pm_idx];
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : " << std::hex << ptr << ", pm_idx="
                  << std::dec << pm_idx
                  << ", page_map_entry=" << static_cast<int>(page_map_entry);
      }
      if (LIKELY(page_map_entry == kPageMapRun)) {
        run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
      } else if (LIKELY(page_map_entry == kPageMapRunPart)) {
        size_t pi = pm_idx;
        // Find the beginning of the run.
        do {
          --pi;
          DCHECK_LT(pi, capacity_ / kPageSize);
        } while (page_map_[pi] != kPageMapRun);
        run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
      } else if (page_map_entry == kPageMapLargeObject) {
        MutexLock mu(self, lock_);
        freed_bytes += FreePages(self, ptr, false);
        continue;
      } else {
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_entry);
      }
    } else {
      // Read the page map entries with a lock.
      MutexLock mu(self, lock_);
      DCHECK_LT(pm_idx, page_map_size_);
      uint8_t page_map_entry = page_map_[pm_idx];
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : " << std::hex << ptr << ", pm_idx="
                  << std::dec << pm_idx
                  << ", page_map_entry=" << static_cast<int>(page_map_entry);
      }
      if (LIKELY(page_map_entry == kPageMapRun)) {
        run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
      } else if (LIKELY(page_map_entry == kPageMapRunPart)) {
        size_t pi = pm_idx;
        // Find the beginning of the run.
        do {
          --pi;
          DCHECK_LT(pi, capacity_ / kPageSize);
        } while (page_map_[pi] != kPageMapRun);
        run = reinterpret_cast<Run*>(base_ + pi * kPageSize);
      } else if (page_map_entry == kPageMapLargeObject) {
        freed_bytes += FreePages(self, ptr, false);
        continue;
      } else {
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_entry);
      }
    }
    DCHECK(run != nullptr);
    DCHECK_EQ(run->magic_num_, kMagicNum);
    // Set the bit in the bulk free bit map.
    freed_bytes += run->AddToBulkFreeList(ptr);
#ifdef __ANDROID__
    if (!run->to_be_bulk_freed_) {
      run->to_be_bulk_freed_ = true;
      runs.push_back(run);
    }
#else
    runs.insert(run);
#endif
  }

  // Now, iterate over the affected runs and update the alloc bit map
  // based on the bulk free bit map (for non-thread-local runs) and
  // union the bulk free bit map into the thread-local free bit map
  // (for thread-local runs.)
  for (Run* run : runs) {
#ifdef __ANDROID__
    DCHECK(run->to_be_bulk_freed_);
    run->to_be_bulk_freed_ = false;
#endif
    size_t idx = run->size_bracket_idx_;
    MutexLock brackets_mu(self, *size_bracket_locks_[idx]);
    if (run->IsThreadLocal()) {
      DCHECK_LT(run->size_bracket_idx_, kNumThreadLocalSizeBrackets);
      DCHECK(non_full_runs_[idx].find(run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(run) == full_runs_[idx].end());
      run->MergeBulkFreeListToThreadLocalFreeList();
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : Freed slot(s) in a thread local run 0x"
                  << std::hex << reinterpret_cast<intptr_t>(run);
      }
      DCHECK(run->IsThreadLocal());
      // A thread local run will be kept as a thread local even if
      // it's become all free.
    } else {
      bool run_was_full = run->IsFull();
      run->MergeBulkFreeListToFreeList();
      if (kTraceRosAlloc) {
        LOG(INFO) << "RosAlloc::BulkFree() : Freed slot(s) in a run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(run);
      }
      // Check if the run should be moved to non_full_runs_ or
      // free_page_runs_.
      auto* non_full_runs = &non_full_runs_[idx];
      auto* full_runs = kIsDebugBuild ? &full_runs_[idx] : nullptr;
      if (run->IsAllFree()) {
        // It has just become completely free. Free the pages of the
        // run.
        bool run_was_current = run == current_runs_[idx];
        if (run_was_current) {
          DCHECK(full_runs->find(run) == full_runs->end());
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
          // If it was a current run, reuse it.
        } else if (run_was_full) {
          // If it was full, remove it from the full run set (debug
          // only.)
          if (kIsDebugBuild) {
            std::unordered_set<Run*, hash_run, eq_run>::iterator pos = full_runs->find(run);
            DCHECK(pos != full_runs->end());
            full_runs->erase(pos);
            if (kTraceRosAlloc) {
              LOG(INFO) << "RosAlloc::BulkFree() : Erased run 0x" << std::hex
                        << reinterpret_cast<intptr_t>(run)
                        << " from full_runs_";
            }
            DCHECK(full_runs->find(run) == full_runs->end());
          }
        } else {
          // If it was in a non full run set, remove it from the set.
          DCHECK(full_runs->find(run) == full_runs->end());
          DCHECK(non_full_runs->find(run) != non_full_runs->end());
          non_full_runs->erase(run);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::BulkFree() : Erased run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(run)
                      << " from non_full_runs_";
          }
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
        }
        if (!run_was_current) {
          run->ZeroHeaderAndSlotHeaders();
          MutexLock lock_mu(self, lock_);
          FreePages(self, run, true);
        }
      } else {
        // It is not completely free. If it wasn't the current run or
        // already in the non-full run set (i.e., it was full) insert
        // it into the non-full run set.
        if (run == current_runs_[idx]) {
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
          DCHECK(full_runs->find(run) == full_runs->end());
          // If it was a current run, keep it.
        } else if (run_was_full) {
          // If it was full, remove it from the full run set (debug
          // only) and insert into the non-full run set.
          DCHECK(full_runs->find(run) != full_runs->end());
          DCHECK(non_full_runs->find(run) == non_full_runs->end());
          if (kIsDebugBuild) {
            full_runs->erase(run);
            if (kTraceRosAlloc) {
              LOG(INFO) << "RosAlloc::BulkFree() : Erased run 0x" << std::hex
                        << reinterpret_cast<intptr_t>(run)
                        << " from full_runs_";
            }
          }
          non_full_runs->insert(run);
          if (kTraceRosAlloc) {
            LOG(INFO) << "RosAlloc::BulkFree() : Inserted run 0x" << std::hex
                      << reinterpret_cast<intptr_t>(run)
                      << " into non_full_runs_[" << std::dec << idx;
          }
        } else {
          // If it was not full, so leave it in the non full run set.
          DCHECK(full_runs->find(run) == full_runs->end());
          DCHECK(non_full_runs->find(run) != non_full_runs->end());
        }
      }
    }
  }
  return freed_bytes;
}

std::string RosAlloc::DumpPageMap() {
  std::ostringstream stream;
  stream << "RosAlloc PageMap: " << std::endl;
  lock_.AssertHeld(Thread::Current());
  size_t end = page_map_size_;
  FreePageRun* curr_fpr = nullptr;
  size_t curr_fpr_size = 0;
  size_t remaining_curr_fpr_size = 0;
  size_t num_running_empty_pages = 0;
  for (size_t i = 0; i < end; ++i) {
    uint8_t pm = page_map_[i];
    switch (pm) {
      case kPageMapReleased:
        // Fall-through.
      case kPageMapEmpty: {
        FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
        if (free_page_runs_.find(fpr) != free_page_runs_.end()) {
          // Encountered a fresh free page run.
          DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
          DCHECK(fpr->IsFree());
          DCHECK(curr_fpr == nullptr);
          DCHECK_EQ(curr_fpr_size, static_cast<size_t>(0));
          curr_fpr = fpr;
          curr_fpr_size = fpr->ByteSize(this);
          DCHECK_EQ(curr_fpr_size % kPageSize, static_cast<size_t>(0));
          remaining_curr_fpr_size = curr_fpr_size - kPageSize;
          stream << "[" << i << "]=" << (pm == kPageMapReleased ? "Released" : "Empty")
                 << " (FPR start) fpr_size=" << curr_fpr_size
                 << " remaining_fpr_size=" << remaining_curr_fpr_size << std::endl;
          if (remaining_curr_fpr_size == 0) {
            // Reset at the end of the current free page run.
            curr_fpr = nullptr;
            curr_fpr_size = 0;
          }
          stream << "curr_fpr=0x" << std::hex << reinterpret_cast<intptr_t>(curr_fpr) << std::endl;
          DCHECK_EQ(num_running_empty_pages, static_cast<size_t>(0));
        } else {
          // Still part of the current free page run.
          DCHECK_NE(num_running_empty_pages, static_cast<size_t>(0));
          DCHECK(curr_fpr != nullptr && curr_fpr_size > 0 && remaining_curr_fpr_size > 0);
          DCHECK_EQ(remaining_curr_fpr_size % kPageSize, static_cast<size_t>(0));
          DCHECK_GE(remaining_curr_fpr_size, static_cast<size_t>(kPageSize));
          remaining_curr_fpr_size -= kPageSize;
          stream << "[" << i << "]=Empty (FPR part)"
                 << " remaining_fpr_size=" << remaining_curr_fpr_size << std::endl;
          if (remaining_curr_fpr_size == 0) {
            // Reset at the end of the current free page run.
            curr_fpr = nullptr;
            curr_fpr_size = 0;
          }
        }
        num_running_empty_pages++;
        break;
      }
      case kPageMapLargeObject: {
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        stream << "[" << i << "]=Large (start)" << std::endl;
        break;
      }
      case kPageMapLargeObjectPart:
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        stream << "[" << i << "]=Large (part)" << std::endl;
        break;
      case kPageMapRun: {
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
        size_t idx = run->size_bracket_idx_;
        stream << "[" << i << "]=Run (start)"
               << " idx=" << idx
               << " numOfPages=" << numOfPages[idx]
               << " is_thread_local=" << run->is_thread_local_
               << " is_all_free=" << (run->IsAllFree() ? 1 : 0)
               << std::endl;
        break;
      }
      case kPageMapRunPart:
        DCHECK_EQ(remaining_curr_fpr_size, static_cast<size_t>(0));
        num_running_empty_pages = 0;
        stream << "[" << i << "]=Run (part)" << std::endl;
        break;
      default:
        stream << "[" << i << "]=Unrecognizable page map type: " << pm;
        break;
    }
  }
  return stream.str();
}

size_t RosAlloc::UsableSize(const void* ptr) {
  DCHECK_LE(base_, ptr);
  DCHECK_LT(ptr, base_ + footprint_);
  size_t pm_idx = RoundDownToPageMapIndex(ptr);
  MutexLock mu(Thread::Current(), lock_);
  switch (page_map_[pm_idx]) {
    case kPageMapReleased:
      // Fall-through.
    case kPageMapEmpty:
      LOG(FATAL) << "Unreachable - " << __PRETTY_FUNCTION__ << ": pm_idx=" << pm_idx << ", ptr="
                 << std::hex << reinterpret_cast<intptr_t>(ptr);
      break;
    case kPageMapLargeObject: {
      size_t num_pages = 1;
      size_t idx = pm_idx + 1;
      size_t end = page_map_size_;
      while (idx < end && page_map_[idx] == kPageMapLargeObjectPart) {
        num_pages++;
        idx++;
      }
      return num_pages * kPageSize;
    }
    case kPageMapLargeObjectPart:
      LOG(FATAL) << "Unreachable - " << __PRETTY_FUNCTION__ << ": pm_idx=" << pm_idx << ", ptr="
                 << std::hex << reinterpret_cast<intptr_t>(ptr);
      break;
    case kPageMapRun:
    case kPageMapRunPart: {
      // Find the beginning of the run.
      while (page_map_[pm_idx] != kPageMapRun) {
        pm_idx--;
        DCHECK_LT(pm_idx, capacity_ / kPageSize);
      }
      DCHECK_EQ(page_map_[pm_idx], kPageMapRun);
      Run* run = reinterpret_cast<Run*>(base_ + pm_idx * kPageSize);
      DCHECK_EQ(run->magic_num_, kMagicNum);
      size_t idx = run->size_bracket_idx_;
      size_t offset_from_slot_base = reinterpret_cast<const uint8_t*>(ptr)
          - (reinterpret_cast<uint8_t*>(run) + headerSizes[idx]);
      DCHECK_EQ(offset_from_slot_base % bracketSizes[idx], static_cast<size_t>(0));
      return IndexToBracketSize(idx);
    }
    default: {
      LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(page_map_[pm_idx]);
      break;
    }
  }
  return 0;
}

bool RosAlloc::Trim() {
  MutexLock mu(Thread::Current(), lock_);
  FreePageRun* last_free_page_run;
  DCHECK_EQ(footprint_ % kPageSize, static_cast<size_t>(0));
  auto it = free_page_runs_.rbegin();
  if (it != free_page_runs_.rend() && (last_free_page_run = *it)->End(this) == base_ + footprint_) {
    // Remove the last free page run, if any.
    DCHECK(last_free_page_run->IsFree());
    DCHECK(IsFreePage(ToPageMapIndex(last_free_page_run)));
    DCHECK_EQ(last_free_page_run->ByteSize(this) % kPageSize, static_cast<size_t>(0));
    DCHECK_EQ(last_free_page_run->End(this), base_ + footprint_);
    free_page_runs_.erase(last_free_page_run);
    size_t decrement = last_free_page_run->ByteSize(this);
    size_t new_footprint = footprint_ - decrement;
    DCHECK_EQ(new_footprint % kPageSize, static_cast<size_t>(0));
    size_t new_num_of_pages = new_footprint / kPageSize;
    DCHECK_GE(page_map_size_, new_num_of_pages);
    // Zero out the tail of the page map.
    uint8_t* zero_begin = const_cast<uint8_t*>(page_map_) + new_num_of_pages;
    uint8_t* madvise_begin = AlignUp(zero_begin, kPageSize);
    DCHECK_LE(madvise_begin, page_map_mem_map_->End());
    size_t madvise_size = page_map_mem_map_->End() - madvise_begin;
    if (madvise_size > 0) {
      DCHECK_ALIGNED(madvise_begin, kPageSize);
      DCHECK_EQ(RoundUp(madvise_size, kPageSize), madvise_size);
      if (!kMadviseZeroes) {
        memset(madvise_begin, 0, madvise_size);
      }
      CHECK_EQ(madvise(madvise_begin, madvise_size, MADV_DONTNEED), 0);
    }
    if (madvise_begin - zero_begin) {
      memset(zero_begin, 0, madvise_begin - zero_begin);
    }
    page_map_size_ = new_num_of_pages;
    free_page_run_size_map_.resize(new_num_of_pages);
    DCHECK_EQ(free_page_run_size_map_.size(), new_num_of_pages);
    ArtRosAllocMoreCore(this, -(static_cast<intptr_t>(decrement)));
    if (kTraceRosAlloc) {
      LOG(INFO) << "RosAlloc::Trim() : decreased the footprint from "
                << footprint_ << " to " << new_footprint;
    }
    DCHECK_LT(new_footprint, footprint_);
    DCHECK_LT(new_footprint, capacity_);
    footprint_ = new_footprint;
    return true;
  }
  return false;
}

void RosAlloc::InspectAll(void (*handler)(void* start, void* end, size_t used_bytes, void* callback_arg),
                          void* arg) {
  // Note: no need to use this to release pages as we already do so in FreePages().
  if (handler == nullptr) {
    return;
  }
  MutexLock mu(Thread::Current(), lock_);
  size_t pm_end = page_map_size_;
  size_t i = 0;
  while (i < pm_end) {
    uint8_t pm = page_map_[i];
    switch (pm) {
      case kPageMapReleased:
        // Fall-through.
      case kPageMapEmpty: {
        // The start of a free page run.
        FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
        DCHECK(free_page_runs_.find(fpr) != free_page_runs_.end());
        size_t fpr_size = fpr->ByteSize(this);
        DCHECK_ALIGNED(fpr_size, kPageSize);
        void* start = fpr;
        if (kIsDebugBuild) {
          // In the debug build, the first page of a free page run
          // contains a magic number for debugging. Exclude it.
          start = reinterpret_cast<uint8_t*>(fpr) + kPageSize;
        }
        void* end = reinterpret_cast<uint8_t*>(fpr) + fpr_size;
        handler(start, end, 0, arg);
        size_t num_pages = fpr_size / kPageSize;
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK(IsFreePage(j));
          }
        }
        i += fpr_size / kPageSize;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapLargeObject: {
        // The start of a large object.
        size_t num_pages = 1;
        size_t idx = i + 1;
        while (idx < pm_end && page_map_[idx] == kPageMapLargeObjectPart) {
          num_pages++;
          idx++;
        }
        void* start = base_ + i * kPageSize;
        void* end = base_ + (i + num_pages) * kPageSize;
        size_t used_bytes = num_pages * kPageSize;
        handler(start, end, used_bytes, arg);
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK_EQ(page_map_[j], kPageMapLargeObjectPart);
          }
        }
        i += num_pages;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapLargeObjectPart:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm);
        break;
      case kPageMapRun: {
        // The start of a run.
        Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
        DCHECK_EQ(run->magic_num_, kMagicNum);
        // The dedicated full run doesn't contain any real allocations, don't visit the slots in
        // there.
        run->InspectAllSlots(handler, arg);
        size_t num_pages = numOfPages[run->size_bracket_idx_];
        if (kIsDebugBuild) {
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            DCHECK_EQ(page_map_[j], kPageMapRunPart);
          }
        }
        i += num_pages;
        DCHECK_LE(i, pm_end);
        break;
      }
      case kPageMapRunPart:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm);
        break;
      default:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm);
        break;
    }
  }
}

size_t RosAlloc::Footprint() {
  MutexLock mu(Thread::Current(), lock_);
  return footprint_;
}

size_t RosAlloc::FootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return capacity_;
}

void RosAlloc::SetFootprintLimit(size_t new_capacity) {
  MutexLock mu(Thread::Current(), lock_);
  DCHECK_EQ(RoundUp(new_capacity, kPageSize), new_capacity);
  // Only growing is supported here. But Trim() is supported.
  if (capacity_ < new_capacity) {
    CHECK_LE(new_capacity, max_capacity_);
    capacity_ = new_capacity;
    VLOG(heap) << "new capacity=" << capacity_;
  }
}

// Below may be called by mutator itself just before thread termination.
size_t RosAlloc::RevokeThreadLocalRuns(Thread* thread) {
  Thread* self = Thread::Current();
  size_t free_bytes = 0U;
  for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; idx++) {
    MutexLock mu(self, *size_bracket_locks_[idx]);
    Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(idx));
    CHECK(thread_local_run != nullptr);
    // Invalid means already revoked.
    DCHECK(thread_local_run->IsThreadLocal());
    if (thread_local_run != dedicated_full_run_) {
      // Note the thread local run may not be full here.
      thread->SetRosAllocRun(idx, dedicated_full_run_);
      DCHECK_EQ(thread_local_run->magic_num_, kMagicNum);
      // Count the number of free slots left.
      size_t num_free_slots = thread_local_run->NumberOfFreeSlots();
      free_bytes += num_free_slots * bracketSizes[idx];
      // The above bracket index lock guards thread local free list to avoid race condition
      // with unioning bulk free list to thread local free list by GC thread in BulkFree.
      // If thread local run is true, GC thread will help update thread local free list
      // in BulkFree. And the latest thread local free list will be merged to free list
      // either when this thread local run is full or when revoking this run here. In this
      // case the free list wll be updated. If thread local run is false, GC thread will help
      // merge bulk free list in next BulkFree.
      // Thus no need to merge bulk free list to free list again here.
      bool dont_care;
      thread_local_run->MergeThreadLocalFreeListToFreeList(&dont_care);
      thread_local_run->SetIsThreadLocal(false);
      DCHECK(non_full_runs_[idx].find(thread_local_run) == non_full_runs_[idx].end());
      DCHECK(full_runs_[idx].find(thread_local_run) == full_runs_[idx].end());
      RevokeRun(self, idx, thread_local_run);
    }
  }
  return free_bytes;
}

void RosAlloc::RevokeRun(Thread* self, size_t idx, Run* run) {
  size_bracket_locks_[idx]->AssertHeld(self);
  DCHECK(run != dedicated_full_run_);
  if (run->IsFull()) {
    if (kIsDebugBuild) {
      full_runs_[idx].insert(run);
      DCHECK(full_runs_[idx].find(run) != full_runs_[idx].end());
      if (kTraceRosAlloc) {
        LOG(INFO) << __PRETTY_FUNCTION__  << " : Inserted run 0x" << std::hex
                  << reinterpret_cast<intptr_t>(run)
                  << " into full_runs_[" << std::dec << idx << "]";
      }
    }
  } else if (run->IsAllFree()) {
    run->ZeroHeaderAndSlotHeaders();
    MutexLock mu(self, lock_);
    FreePages(self, run, true);
  } else {
    non_full_runs_[idx].insert(run);
    DCHECK(non_full_runs_[idx].find(run) != non_full_runs_[idx].end());
    if (kTraceRosAlloc) {
      LOG(INFO) << __PRETTY_FUNCTION__ << " : Inserted run 0x" << std::hex
                << reinterpret_cast<intptr_t>(run)
                << " into non_full_runs_[" << std::dec << idx << "]";
    }
  }
}

void RosAlloc::RevokeThreadUnsafeCurrentRuns() {
  // Revoke the current runs which share the same idx as thread local runs.
  Thread* self = Thread::Current();
  for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; ++idx) {
    MutexLock mu(self, *size_bracket_locks_[idx]);
    if (current_runs_[idx] != dedicated_full_run_) {
      RevokeRun(self, idx, current_runs_[idx]);
      current_runs_[idx] = dedicated_full_run_;
    }
  }
}

size_t RosAlloc::RevokeAllThreadLocalRuns() {
  // This is called when a mutator thread won't allocate such as at
  // the Zygote creation time or during the GC pause.
  MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
  MutexLock mu2(Thread::Current(), *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  size_t free_bytes = 0U;
  for (Thread* thread : thread_list) {
    free_bytes += RevokeThreadLocalRuns(thread);
  }
  RevokeThreadUnsafeCurrentRuns();
  return free_bytes;
}

void RosAlloc::AssertThreadLocalRunsAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    Thread* self = Thread::Current();
    // Avoid race conditions on the bulk free bit maps with BulkFree() (GC).
    ReaderMutexLock wmu(self, bulk_free_lock_);
    for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; idx++) {
      MutexLock mu(self, *size_bracket_locks_[idx]);
      Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(idx));
      DCHECK(thread_local_run == nullptr || thread_local_run == dedicated_full_run_);
    }
  }
}

void RosAlloc::AssertAllThreadLocalRunsAreRevoked() {
  if (kIsDebugBuild) {
    Thread* self = Thread::Current();
    MutexLock shutdown_mu(self, *Locks::runtime_shutdown_lock_);
    MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (Thread* t : thread_list) {
      AssertThreadLocalRunsAreRevoked(t);
    }
    for (size_t idx = 0; idx < kNumThreadLocalSizeBrackets; ++idx) {
      MutexLock brackets_mu(self, *size_bracket_locks_[idx]);
      CHECK_EQ(current_runs_[idx], dedicated_full_run_);
    }
  }
}

void RosAlloc::Initialize() {
  // bracketSizes.
  static_assert(kNumRegularSizeBrackets == kNumOfSizeBrackets - 2,
                "There should be two non-regular brackets");
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    if (i < kNumThreadLocalSizeBrackets) {
      bracketSizes[i] = kThreadLocalBracketQuantumSize * (i + 1);
    } else if (i < kNumRegularSizeBrackets) {
      bracketSizes[i] = kBracketQuantumSize * (i - kNumThreadLocalSizeBrackets + 1) +
          (kThreadLocalBracketQuantumSize *  kNumThreadLocalSizeBrackets);
    } else if (i == kNumOfSizeBrackets - 2) {
      bracketSizes[i] = 1 * KB;
    } else {
      DCHECK_EQ(i, kNumOfSizeBrackets - 1);
      bracketSizes[i] = 2 * KB;
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "bracketSizes[" << i << "]=" << bracketSizes[i];
    }
  }
  // numOfPages.
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    if (i < kNumThreadLocalSizeBrackets) {
      numOfPages[i] = 1;
    } else if (i < (kNumThreadLocalSizeBrackets + kNumRegularSizeBrackets) / 2) {
      numOfPages[i] = 1;
    } else if (i < kNumRegularSizeBrackets) {
      numOfPages[i] = 1;
    } else if (i == kNumOfSizeBrackets - 2) {
      numOfPages[i] = 2;
    } else {
      DCHECK_EQ(i, kNumOfSizeBrackets - 1);
      numOfPages[i] = 4;
    }
    if (kTraceRosAlloc) {
      LOG(INFO) << "numOfPages[" << i << "]=" << numOfPages[i];
    }
  }
  // Compute numOfSlots and slotOffsets.
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    size_t bracket_size = bracketSizes[i];
    size_t run_size = kPageSize * numOfPages[i];
    size_t max_num_of_slots = run_size / bracket_size;
    // Compute the actual number of slots by taking the header and
    // alignment into account.
    size_t fixed_header_size = RoundUp(Run::fixed_header_size(), sizeof(uint64_t));
    DCHECK_EQ(fixed_header_size, 80U);
    size_t header_size = 0;
    size_t num_of_slots = 0;
    // Search for the maximum number of slots that allows enough space
    // for the header.
    for (int s = max_num_of_slots; s >= 0; s--) {
      size_t tmp_slots_size = bracket_size * s;
      size_t tmp_unaligned_header_size = fixed_header_size;
      // Align up the unaligned header size. bracket_size may not be a power of two.
      size_t tmp_header_size = (tmp_unaligned_header_size % bracket_size == 0) ?
          tmp_unaligned_header_size :
          tmp_unaligned_header_size + (bracket_size - tmp_unaligned_header_size % bracket_size);
      DCHECK_EQ(tmp_header_size % bracket_size, 0U);
      DCHECK_EQ(tmp_header_size % sizeof(uint64_t), 0U);
      if (tmp_slots_size + tmp_header_size <= run_size) {
        // Found the right number of slots, that is, there was enough
        // space for the header (including the bit maps.)
        num_of_slots = s;
        header_size = tmp_header_size;
        break;
      }
    }
    DCHECK_GT(num_of_slots, 0U) << i;
    DCHECK_GT(header_size, 0U) << i;
    // Add the padding for the alignment remainder.
    header_size += run_size % bracket_size;
    DCHECK_EQ(header_size + num_of_slots * bracket_size, run_size);
    numOfSlots[i] = num_of_slots;
    headerSizes[i] = header_size;
    if (kTraceRosAlloc) {
      LOG(INFO) << "numOfSlots[" << i << "]=" << numOfSlots[i]
                << ", headerSizes[" << i << "]=" << headerSizes[i];
    }
  }
  // Set up the dedicated full run so that nobody can successfully allocate from it.
  if (kIsDebugBuild) {
    dedicated_full_run_->magic_num_ = kMagicNum;
  }
  // It doesn't matter which size bracket we use since the main goal is to have the allocation
  // fail 100% of the time you attempt to allocate into the dedicated full run.
  dedicated_full_run_->size_bracket_idx_ = 0;
  DCHECK_EQ(dedicated_full_run_->FreeList()->Size(), 0U);  // It looks full.
  dedicated_full_run_->SetIsThreadLocal(true);

  // The smallest bracket size must be at least as large as the sizeof(Slot).
  DCHECK_LE(sizeof(Slot), bracketSizes[0]) << "sizeof(Slot) <= the smallest bracket size";
  // Check the invariants between the max bracket sizes and the number of brackets.
  DCHECK_EQ(kMaxThreadLocalBracketSize, bracketSizes[kNumThreadLocalSizeBrackets - 1]);
  DCHECK_EQ(kMaxRegularBracketSize, bracketSizes[kNumRegularSizeBrackets - 1]);
}

void RosAlloc::BytesAllocatedCallback(void* start ATTRIBUTE_UNUSED, void* end ATTRIBUTE_UNUSED,
                                      size_t used_bytes, void* arg) {
  if (used_bytes == 0) {
    return;
  }
  size_t* bytes_allocated = reinterpret_cast<size_t*>(arg);
  *bytes_allocated += used_bytes;
}

void RosAlloc::ObjectsAllocatedCallback(void* start ATTRIBUTE_UNUSED, void* end ATTRIBUTE_UNUSED,
                                        size_t used_bytes, void* arg) {
  if (used_bytes == 0) {
    return;
  }
  size_t* objects_allocated = reinterpret_cast<size_t*>(arg);
  ++(*objects_allocated);
}

void RosAlloc::Verify() {
  Thread* self = Thread::Current();
  CHECK(Locks::mutator_lock_->IsExclusiveHeld(self))
      << "The mutator locks isn't exclusively locked at " << __PRETTY_FUNCTION__;
  MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
  ReaderMutexLock wmu(self, bulk_free_lock_);
  std::vector<Run*> runs;
  {
    MutexLock lock_mu(self, lock_);
    size_t pm_end = page_map_size_;
    size_t i = 0;
    size_t memory_tool_modifier =  is_running_on_memory_tool_ ?
        2 * ::art::gc::space::kDefaultMemoryToolRedZoneBytes :  // Redzones before and after.
        0;
    while (i < pm_end) {
      uint8_t pm = page_map_[i];
      switch (pm) {
        case kPageMapReleased:
          // Fall-through.
        case kPageMapEmpty: {
          // The start of a free page run.
          FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
          DCHECK_EQ(fpr->magic_num_, kMagicNumFree);
          CHECK(free_page_runs_.find(fpr) != free_page_runs_.end())
              << "An empty page must belong to the free page run set";
          size_t fpr_size = fpr->ByteSize(this);
          CHECK_ALIGNED(fpr_size, kPageSize)
              << "A free page run size isn't page-aligned : " << fpr_size;
          size_t num_pages = fpr_size / kPageSize;
          CHECK_GT(num_pages, static_cast<uintptr_t>(0))
              << "A free page run size must be > 0 : " << fpr_size;
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            CHECK(IsFreePage(j))
                << "A mismatch between the page map table for kPageMapEmpty "
                << " at page index " << j
                << " and the free page run size : page index range : "
                << i << " to " << (i + num_pages) << std::endl << DumpPageMap();
          }
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapLargeObject: {
          // The start of a large object.
          size_t num_pages = 1;
          size_t idx = i + 1;
          while (idx < pm_end && page_map_[idx] == kPageMapLargeObjectPart) {
            num_pages++;
            idx++;
          }
          uint8_t* start = base_ + i * kPageSize;
          if (is_running_on_memory_tool_) {
            start += ::art::gc::space::kDefaultMemoryToolRedZoneBytes;
          }
          mirror::Object* obj = reinterpret_cast<mirror::Object*>(start);
          size_t obj_size = obj->SizeOf();
          CHECK_GT(obj_size + memory_tool_modifier, kLargeSizeThreshold)
              << "A rosalloc large object size must be > " << kLargeSizeThreshold;
          CHECK_EQ(num_pages, RoundUp(obj_size + memory_tool_modifier, kPageSize) / kPageSize)
              << "A rosalloc large object size " << obj_size + memory_tool_modifier
              << " does not match the page map table " << (num_pages * kPageSize)
              << std::endl << DumpPageMap();
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapLargeObjectPart:
          LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm) << std::endl << DumpPageMap();
          break;
        case kPageMapRun: {
          // The start of a run.
          Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
          DCHECK_EQ(run->magic_num_, kMagicNum);
          size_t idx = run->size_bracket_idx_;
          CHECK_LT(idx, kNumOfSizeBrackets) << "Out of range size bracket index : " << idx;
          size_t num_pages = numOfPages[idx];
          CHECK_GT(num_pages, static_cast<uintptr_t>(0))
              << "Run size must be > 0 : " << num_pages;
          for (size_t j = i + 1; j < i + num_pages; ++j) {
            CHECK_EQ(page_map_[j], kPageMapRunPart)
                << "A mismatch between the page map table for kPageMapRunPart "
                << " at page index " << j
                << " and the run size : page index range " << i << " to " << (i + num_pages)
                << std::endl << DumpPageMap();
          }
          // Don't verify the dedicated_full_run_ since it doesn't have any real allocations.
          runs.push_back(run);
          i += num_pages;
          CHECK_LE(i, pm_end) << "Page map index " << i << " out of range < " << pm_end
                              << std::endl << DumpPageMap();
          break;
        }
        case kPageMapRunPart:
          // Fall-through.
        default:
          LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm) << std::endl << DumpPageMap();
          break;
      }
    }
  }
  std::list<Thread*> threads = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* thread : threads) {
    for (size_t i = 0; i < kNumThreadLocalSizeBrackets; ++i) {
      MutexLock brackets_mu(self, *size_bracket_locks_[i]);
      Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(i));
      CHECK(thread_local_run != nullptr);
      CHECK(thread_local_run->IsThreadLocal());
      CHECK(thread_local_run == dedicated_full_run_ ||
            thread_local_run->size_bracket_idx_ == i);
    }
  }
  for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
    MutexLock brackets_mu(self, *size_bracket_locks_[i]);
    Run* current_run = current_runs_[i];
    CHECK(current_run != nullptr);
    if (current_run != dedicated_full_run_) {
      // The dedicated full run is currently marked as thread local.
      CHECK(!current_run->IsThreadLocal());
      CHECK_EQ(current_run->size_bracket_idx_, i);
    }
  }
  // Call Verify() here for the lock order.
  for (auto& run : runs) {
    run->Verify(self, this, is_running_on_memory_tool_);
  }
}

void RosAlloc::Run::Verify(Thread* self, RosAlloc* rosalloc, bool running_on_memory_tool) {
  DCHECK_EQ(magic_num_, kMagicNum) << "Bad magic number : " << Dump();
  const size_t idx = size_bracket_idx_;
  CHECK_LT(idx, kNumOfSizeBrackets) << "Out of range size bracket index : " << Dump();
  uint8_t* slot_base = reinterpret_cast<uint8_t*>(this) + headerSizes[idx];
  const size_t num_slots = numOfSlots[idx];
  size_t bracket_size = IndexToBracketSize(idx);
  CHECK_EQ(slot_base + num_slots * bracket_size,
           reinterpret_cast<uint8_t*>(this) + numOfPages[idx] * kPageSize)
      << "Mismatch in the end address of the run " << Dump();
  // Check that the bulk free list is empty. It's only used during BulkFree().
  CHECK(IsBulkFreeListEmpty()) << "The bulk free isn't empty " << Dump();
  // Check the thread local runs, the current runs, and the run sets.
  if (IsThreadLocal()) {
    // If it's a thread local run, then it must be pointed to by an owner thread.
    bool owner_found = false;
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (auto it = thread_list.begin(); it != thread_list.end(); ++it) {
      Thread* thread = *it;
      for (size_t i = 0; i < kNumThreadLocalSizeBrackets; i++) {
        MutexLock mu(self, *rosalloc->size_bracket_locks_[i]);
        Run* thread_local_run = reinterpret_cast<Run*>(thread->GetRosAllocRun(i));
        if (thread_local_run == this) {
          CHECK(!owner_found)
              << "A thread local run has more than one owner thread " << Dump();
          CHECK_EQ(i, idx)
              << "A mismatching size bracket index in a thread local run " << Dump();
          owner_found = true;
        }
      }
    }
    CHECK(owner_found) << "A thread local run has no owner thread " << Dump();
  } else {
    // If it's not thread local, check that the thread local free list is empty.
    CHECK(IsThreadLocalFreeListEmpty())
        << "A non-thread-local run's thread local free list isn't empty "
        << Dump();
    // Check if it's a current run for the size bracket.
    bool is_current_run = false;
    for (size_t i = 0; i < kNumOfSizeBrackets; i++) {
      MutexLock mu(self, *rosalloc->size_bracket_locks_[i]);
      Run* current_run = rosalloc->current_runs_[i];
      if (idx == i) {
        if (this == current_run) {
          is_current_run = true;
        }
      } else {
        // If the size bucket index does not match, then it must not
        // be a current run.
        CHECK_NE(this, current_run)
            << "A current run points to a run with a wrong size bracket index " << Dump();
      }
    }
    // If it's neither a thread local or current run, then it must be
    // in a run set.
    if (!is_current_run) {
      MutexLock mu(self, rosalloc->lock_);
      auto& non_full_runs = rosalloc->non_full_runs_[idx];
      // If it's all free, it must be a free page run rather than a run.
      CHECK(!IsAllFree()) << "A free run must be in a free page run set " << Dump();
      if (!IsFull()) {
        // If it's not full, it must in the non-full run set.
        CHECK(non_full_runs.find(this) != non_full_runs.end())
            << "A non-full run isn't in the non-full run set " << Dump();
      } else {
        // If it's full, it must in the full run set (debug build only.)
        if (kIsDebugBuild) {
          auto& full_runs = rosalloc->full_runs_[idx];
          CHECK(full_runs.find(this) != full_runs.end())
              << " A full run isn't in the full run set " << Dump();
        }
      }
    }
  }
  // Check each slot.
  size_t memory_tool_modifier = running_on_memory_tool ?
      2 * ::art::gc::space::kDefaultMemoryToolRedZoneBytes :
      0U;
  // TODO: reuse InspectAllSlots().
  std::unique_ptr<bool[]> is_free(new bool[num_slots]());  // zero initialized
  // Mark the free slots and the remaining ones are allocated.
  for (Slot* slot = free_list_.Head(); slot != nullptr; slot = slot->Next()) {
    size_t slot_idx = SlotIndex(slot);
    DCHECK_LT(slot_idx, num_slots);
    is_free[slot_idx] = true;
  }
  if (IsThreadLocal()) {
    for (Slot* slot = thread_local_free_list_.Head(); slot != nullptr; slot = slot->Next()) {
      size_t slot_idx = SlotIndex(slot);
      DCHECK_LT(slot_idx, num_slots);
      is_free[slot_idx] = true;
    }
  }
  for (size_t slot_idx = 0; slot_idx < num_slots; ++slot_idx) {
    uint8_t* slot_addr = slot_base + slot_idx * bracket_size;
    if (running_on_memory_tool) {
      slot_addr += ::art::gc::space::kDefaultMemoryToolRedZoneBytes;
    }
    if (!is_free[slot_idx]) {
      // The slot is allocated
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(slot_addr);
      size_t obj_size = obj->SizeOf();
      CHECK_LE(obj_size + memory_tool_modifier, kLargeSizeThreshold)
          << "A run slot contains a large object " << Dump();
      CHECK_EQ(SizeToIndex(obj_size + memory_tool_modifier), idx)
          << PrettyTypeOf(obj) << " "
          << "obj_size=" << obj_size << "(" << obj_size + memory_tool_modifier << "), idx=" << idx
          << " A run slot contains an object with wrong size " << Dump();
    }
  }
}

size_t RosAlloc::ReleasePages() {
  VLOG(heap) << "RosAlloc::ReleasePages()";
  DCHECK(!DoesReleaseAllPages());
  Thread* self = Thread::Current();
  size_t reclaimed_bytes = 0;
  size_t i = 0;
  // Check the page map size which might have changed due to grow/shrink.
  while (i < page_map_size_) {
    // Reading the page map without a lock is racy but the race is benign since it should only
    // result in occasionally not releasing pages which we could release.
    uint8_t pm = page_map_[i];
    switch (pm) {
      case kPageMapReleased:
        // Fall through.
      case kPageMapEmpty: {
        // This is currently the start of a free page run.
        // Acquire the lock to prevent other threads racing in and modifying the page map.
        MutexLock mu(self, lock_);
        // Check that it's still empty after we acquired the lock since another thread could have
        // raced in and placed an allocation here.
        if (IsFreePage(i)) {
          // Free page runs can start with a released page if we coalesced a released page free
          // page run with an empty page run.
          FreePageRun* fpr = reinterpret_cast<FreePageRun*>(base_ + i * kPageSize);
          // There is a race condition where FreePage can coalesce fpr with the previous
          // free page run before we acquire lock_. In that case free_page_runs_.find will not find
          // a run starting at fpr. To handle this race, we skip reclaiming the page range and go
          // to the next page.
          if (free_page_runs_.find(fpr) != free_page_runs_.end()) {
            size_t fpr_size = fpr->ByteSize(this);
            DCHECK_ALIGNED(fpr_size, kPageSize);
            uint8_t* start = reinterpret_cast<uint8_t*>(fpr);
            reclaimed_bytes += ReleasePageRange(start, start + fpr_size);
            size_t pages = fpr_size / kPageSize;
            CHECK_GT(pages, 0U) << "Infinite loop probable";
            i += pages;
            DCHECK_LE(i, page_map_size_);
            break;
          }
        }
        FALLTHROUGH_INTENDED;
      }
      case kPageMapLargeObject:      // Fall through.
      case kPageMapLargeObjectPart:  // Fall through.
      case kPageMapRun:              // Fall through.
      case kPageMapRunPart:          // Fall through.
        ++i;
        break;  // Skip.
      default:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm);
        break;
    }
  }
  return reclaimed_bytes;
}

size_t RosAlloc::ReleasePageRange(uint8_t* start, uint8_t* end) {
  DCHECK_ALIGNED(start, kPageSize);
  DCHECK_ALIGNED(end, kPageSize);
  DCHECK_LT(start, end);
  if (kIsDebugBuild) {
    // In the debug build, the first page of a free page run
    // contains a magic number for debugging. Exclude it.
    start += kPageSize;

    // Single pages won't be released.
    if (start == end) {
      return 0;
    }
  }
  if (!kMadviseZeroes) {
    // TODO: Do this when we resurrect the page instead.
    memset(start, 0, end - start);
  }
  CHECK_EQ(madvise(start, end - start, MADV_DONTNEED), 0);
  size_t pm_idx = ToPageMapIndex(start);
  size_t reclaimed_bytes = 0;
  // Calculate reclaimed bytes and upate page map.
  const size_t max_idx = pm_idx + (end - start) / kPageSize;
  for (; pm_idx < max_idx; ++pm_idx) {
    DCHECK(IsFreePage(pm_idx));
    if (page_map_[pm_idx] == kPageMapEmpty) {
      // Mark the page as released and update how many bytes we released.
      reclaimed_bytes += kPageSize;
      page_map_[pm_idx] = kPageMapReleased;
    }
  }
  return reclaimed_bytes;
}

void RosAlloc::LogFragmentationAllocFailure(std::ostream& os, size_t failed_alloc_bytes) {
  Thread* self = Thread::Current();
  size_t largest_continuous_free_pages = 0;
  WriterMutexLock wmu(self, bulk_free_lock_);
  MutexLock mu(self, lock_);
  for (FreePageRun* fpr : free_page_runs_) {
    largest_continuous_free_pages = std::max(largest_continuous_free_pages,
                                             fpr->ByteSize(this));
  }
  if (failed_alloc_bytes > kLargeSizeThreshold) {
    // Large allocation.
    size_t required_bytes = RoundUp(failed_alloc_bytes, kPageSize);
    if (required_bytes > largest_continuous_free_pages) {
      os << "; failed due to fragmentation (required continguous free "
         << required_bytes << " bytes where largest contiguous free "
         <<  largest_continuous_free_pages << " bytes)";
    }
  } else {
    // Non-large allocation.
    size_t required_bytes = numOfPages[SizeToIndex(failed_alloc_bytes)] * kPageSize;
    if (required_bytes > largest_continuous_free_pages) {
      os << "; failed due to fragmentation (required continguous free "
         << required_bytes << " bytes for a new buffer where largest contiguous free "
         <<  largest_continuous_free_pages << " bytes)";
    }
  }
}

void RosAlloc::DumpStats(std::ostream& os) {
  Thread* self = Thread::Current();
  CHECK(Locks::mutator_lock_->IsExclusiveHeld(self))
      << "The mutator locks isn't exclusively locked at " << __PRETTY_FUNCTION__;
  size_t num_large_objects = 0;
  size_t num_pages_large_objects = 0;
  // These arrays are zero initialized.
  std::unique_ptr<size_t[]> num_runs(new size_t[kNumOfSizeBrackets]());
  std::unique_ptr<size_t[]> num_pages_runs(new size_t[kNumOfSizeBrackets]());
  std::unique_ptr<size_t[]> num_slots(new size_t[kNumOfSizeBrackets]());
  std::unique_ptr<size_t[]> num_used_slots(new size_t[kNumOfSizeBrackets]());
  std::unique_ptr<size_t[]> num_metadata_bytes(new size_t[kNumOfSizeBrackets]());
  ReaderMutexLock rmu(self, bulk_free_lock_);
  MutexLock lock_mu(self, lock_);
  for (size_t i = 0; i < page_map_size_; ) {
    uint8_t pm = page_map_[i];
    switch (pm) {
      case kPageMapReleased:
      case kPageMapEmpty:
        ++i;
        break;
      case kPageMapLargeObject: {
        size_t num_pages = 1;
        size_t idx = i + 1;
        while (idx < page_map_size_ && page_map_[idx] == kPageMapLargeObjectPart) {
          num_pages++;
          idx++;
        }
        num_large_objects++;
        num_pages_large_objects += num_pages;
        i += num_pages;
        break;
      }
      case kPageMapLargeObjectPart:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm) << std::endl
                   << DumpPageMap();
        break;
      case kPageMapRun: {
        Run* run = reinterpret_cast<Run*>(base_ + i * kPageSize);
        size_t idx = run->size_bracket_idx_;
        size_t num_pages = numOfPages[idx];
        num_runs[idx]++;
        num_pages_runs[idx] += num_pages;
        num_slots[idx] += numOfSlots[idx];
        size_t num_free_slots = run->NumberOfFreeSlots();
        num_used_slots[idx] += numOfSlots[idx] - num_free_slots;
        num_metadata_bytes[idx] += headerSizes[idx];
        i += num_pages;
        break;
      }
      case kPageMapRunPart:
        // Fall-through.
      default:
        LOG(FATAL) << "Unreachable - page map type: " << static_cast<int>(pm) << std::endl
                   << DumpPageMap();
        break;
    }
  }
  os << "RosAlloc stats:\n";
  for (size_t i = 0; i < kNumOfSizeBrackets; ++i) {
    os << "Bracket " << i << " (" << bracketSizes[i] << "):"
       << " #runs=" << num_runs[i]
       << " #pages=" << num_pages_runs[i]
       << " (" << PrettySize(num_pages_runs[i] * kPageSize) << ")"
       << " #metadata_bytes=" << PrettySize(num_metadata_bytes[i])
       << " #slots=" << num_slots[i] << " (" << PrettySize(num_slots[i] * bracketSizes[i]) << ")"
       << " #used_slots=" << num_used_slots[i]
       << " (" << PrettySize(num_used_slots[i] * bracketSizes[i]) << ")\n";
  }
  os << "Large #allocations=" << num_large_objects
     << " #pages=" << num_pages_large_objects
     << " (" << PrettySize(num_pages_large_objects * kPageSize) << ")\n";
  size_t total_num_pages = 0;
  size_t total_metadata_bytes = 0;
  size_t total_allocated_bytes = 0;
  for (size_t i = 0; i < kNumOfSizeBrackets; ++i) {
    total_num_pages += num_pages_runs[i];
    total_metadata_bytes += num_metadata_bytes[i];
    total_allocated_bytes += num_used_slots[i] * bracketSizes[i];
  }
  total_num_pages += num_pages_large_objects;
  total_allocated_bytes += num_pages_large_objects * kPageSize;
  os << "Total #total_bytes=" << PrettySize(total_num_pages * kPageSize)
     << " #metadata_bytes=" << PrettySize(total_metadata_bytes)
     << " #used_bytes=" << PrettySize(total_allocated_bytes) << "\n";
  os << "\n";
}

}  // namespace allocator
}  // namespace gc
}  // namespace art
