/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "dlmalloc_space-inl.h"

#include "base/time_utils.h"
#include "gc/accounting/card_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "memory_tool_malloc_space-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

static constexpr bool kPrefetchDuringDlMallocFreeList = true;

DlMallocSpace::DlMallocSpace(MemMap* mem_map, size_t initial_size, const std::string& name,
                             void* mspace, uint8_t* begin, uint8_t* end, uint8_t* limit,
                             size_t growth_limit, bool can_move_objects, size_t starting_size)
    : MallocSpace(name, mem_map, begin, end, limit, growth_limit, true, can_move_objects,
                  starting_size, initial_size),
      mspace_(mspace) {
  CHECK(mspace != nullptr);
}

DlMallocSpace* DlMallocSpace::CreateFromMemMap(MemMap* mem_map, const std::string& name,
                                               size_t starting_size, size_t initial_size,
                                               size_t growth_limit, size_t capacity,
                                               bool can_move_objects) {
  DCHECK(mem_map != nullptr);
  void* mspace = CreateMspace(mem_map->Begin(), starting_size, initial_size);
  if (mspace == nullptr) {
    LOG(ERROR) << "Failed to initialize mspace for alloc space (" << name << ")";
    return nullptr;
  }

  // Protect memory beyond the starting size. morecore will add r/w permissions when necessory
  uint8_t* end = mem_map->Begin() + starting_size;
  if (capacity - starting_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - starting_size, PROT_NONE), name);
  }

  // Everything is set so record in immutable structure and leave
  uint8_t* begin = mem_map->Begin();
  if (Runtime::Current()->IsRunningOnMemoryTool()) {
    return new MemoryToolMallocSpace<DlMallocSpace, kDefaultMemoryToolRedZoneBytes, true, false>(
        mem_map, initial_size, name, mspace, begin, end, begin + capacity, growth_limit,
        can_move_objects, starting_size);
  } else {
    return new DlMallocSpace(mem_map, initial_size, name, mspace, begin, end, begin + capacity,
                             growth_limit, can_move_objects, starting_size);
  }
}

DlMallocSpace* DlMallocSpace::Create(const std::string& name, size_t initial_size,
                                     size_t growth_limit, size_t capacity, uint8_t* requested_begin,
                                     bool can_move_objects) {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    LOG(INFO) << "DlMallocSpace::Create entering " << name
        << " initial_size=" << PrettySize(initial_size)
        << " growth_limit=" << PrettySize(growth_limit)
        << " capacity=" << PrettySize(capacity)
        << " requested_begin=" << reinterpret_cast<void*>(requested_begin);
  }

  // Memory we promise to dlmalloc before it asks for morecore.
  // Note: making this value large means that large allocations are unlikely to succeed as dlmalloc
  // will ask for this memory from sys_alloc which will fail as the footprint (this value plus the
  // size of the large allocation) will be greater than the footprint limit.
  size_t starting_size = kPageSize;
  MemMap* mem_map = CreateMemMap(name, starting_size, &initial_size, &growth_limit, &capacity,
                                 requested_begin);
  if (mem_map == nullptr) {
    LOG(ERROR) << "Failed to create mem map for alloc space (" << name << ") of size "
               << PrettySize(capacity);
    return nullptr;
  }
  DlMallocSpace* space = CreateFromMemMap(mem_map, name, starting_size, initial_size,
                                          growth_limit, capacity, can_move_objects);
  // We start out with only the initial size possibly containing objects.
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "DlMallocSpace::Create exiting (" << PrettyDuration(NanoTime() - start_time)
        << " ) " << *space;
  }
  return space;
}

void* DlMallocSpace::CreateMspace(void* begin, size_t morecore_start, size_t initial_size) {
  // clear errno to allow PLOG on error
  errno = 0;
  // create mspace using our backing storage starting at begin and with a footprint of
  // morecore_start. Don't use an internal dlmalloc lock (as we already hold heap lock). When
  // morecore_start bytes of memory is exhaused morecore will be called.
  void* msp = create_mspace_with_base(begin, morecore_start, false /*locked*/);
  if (msp != nullptr) {
    // Do not allow morecore requests to succeed beyond the initial size of the heap
    mspace_set_footprint_limit(msp, initial_size);
  } else {
    PLOG(ERROR) << "create_mspace_with_base failed";
  }
  return msp;
}

mirror::Object* DlMallocSpace::AllocWithGrowth(Thread* self, size_t num_bytes,
                                               size_t* bytes_allocated, size_t* usable_size,
                                               size_t* bytes_tl_bulk_allocated) {
  mirror::Object* result;
  {
    MutexLock mu(self, lock_);
    // Grow as much as possible within the space.
    size_t max_allowed = Capacity();
    mspace_set_footprint_limit(mspace_, max_allowed);
    // Try the allocation.
    result = AllocWithoutGrowthLocked(self, num_bytes, bytes_allocated, usable_size,
                                      bytes_tl_bulk_allocated);
    // Shrink back down as small as possible.
    size_t footprint = mspace_footprint(mspace_);
    mspace_set_footprint_limit(mspace_, footprint);
  }
  if (result != nullptr) {
    // Zero freshly allocated memory, done while not holding the space's lock.
    memset(result, 0, num_bytes);
    // Check that the result is contained in the space.
    CHECK(!kDebugSpaces || Contains(result));
  }
  return result;
}

MallocSpace* DlMallocSpace::CreateInstance(MemMap* mem_map, const std::string& name,
                                           void* allocator, uint8_t* begin, uint8_t* end,
                                           uint8_t* limit, size_t growth_limit,
                                           bool can_move_objects) {
  if (Runtime::Current()->IsRunningOnMemoryTool()) {
    return new MemoryToolMallocSpace<DlMallocSpace, kDefaultMemoryToolRedZoneBytes, true, false>(
        mem_map, initial_size_, name, allocator, begin, end, limit, growth_limit,
        can_move_objects, starting_size_);
  } else {
    return new DlMallocSpace(mem_map, initial_size_, name, allocator, begin, end, limit,
                             growth_limit, can_move_objects, starting_size_);
  }
}

size_t DlMallocSpace::Free(Thread* self, mirror::Object* ptr) {
  MutexLock mu(self, lock_);
  if (kDebugSpaces) {
    CHECK(ptr != nullptr);
    CHECK(Contains(ptr)) << "Free (" << ptr << ") not in bounds of heap " << *this;
  }
  const size_t bytes_freed = AllocationSizeNonvirtual(ptr, nullptr);
  if (kRecentFreeCount > 0) {
    RegisterRecentFree(ptr);
  }
  mspace_free(mspace_, ptr);
  return bytes_freed;
}

size_t DlMallocSpace::FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
  DCHECK(ptrs != nullptr);

  // Don't need the lock to calculate the size of the freed pointers.
  size_t bytes_freed = 0;
  for (size_t i = 0; i < num_ptrs; i++) {
    mirror::Object* ptr = ptrs[i];
    const size_t look_ahead = 8;
    if (kPrefetchDuringDlMallocFreeList && i + look_ahead < num_ptrs) {
      // The head of chunk for the allocation is sizeof(size_t) behind the allocation.
      __builtin_prefetch(reinterpret_cast<char*>(ptrs[i + look_ahead]) - sizeof(size_t));
    }
    bytes_freed += AllocationSizeNonvirtual(ptr, nullptr);
  }

  if (kRecentFreeCount > 0) {
    MutexLock mu(self, lock_);
    for (size_t i = 0; i < num_ptrs; i++) {
      RegisterRecentFree(ptrs[i]);
    }
  }

  if (kDebugSpaces) {
    size_t num_broken_ptrs = 0;
    for (size_t i = 0; i < num_ptrs; i++) {
      if (!Contains(ptrs[i])) {
        num_broken_ptrs++;
        LOG(ERROR) << "FreeList[" << i << "] (" << ptrs[i] << ") not in bounds of heap " << *this;
      } else {
        size_t size = mspace_usable_size(ptrs[i]);
        memset(ptrs[i], 0xEF, size);
      }
    }
    CHECK_EQ(num_broken_ptrs, 0u);
  }

  {
    MutexLock mu(self, lock_);
    mspace_bulk_free(mspace_, reinterpret_cast<void**>(ptrs), num_ptrs);
    return bytes_freed;
  }
}

size_t DlMallocSpace::Trim() {
  MutexLock mu(Thread::Current(), lock_);
  // Trim to release memory at the end of the space.
  mspace_trim(mspace_, 0);
  // Visit space looking for page-sized holes to advise the kernel we don't need.
  size_t reclaimed = 0;
  mspace_inspect_all(mspace_, DlmallocMadviseCallback, &reclaimed);
  return reclaimed;
}

void DlMallocSpace::Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                      void* arg) {
  MutexLock mu(Thread::Current(), lock_);
  mspace_inspect_all(mspace_, callback, arg);
  callback(nullptr, nullptr, 0, arg);  // Indicate end of a space.
}

size_t DlMallocSpace::GetFootprint() {
  MutexLock mu(Thread::Current(), lock_);
  return mspace_footprint(mspace_);
}

size_t DlMallocSpace::GetFootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return mspace_footprint_limit(mspace_);
}

void DlMallocSpace::SetFootprintLimit(size_t new_size) {
  MutexLock mu(Thread::Current(), lock_);
  VLOG(heap) << "DlMallocSpace::SetFootprintLimit " << PrettySize(new_size);
  // Compare against the actual footprint, rather than the Size(), because the heap may not have
  // grown all the way to the allowed size yet.
  size_t current_space_size = mspace_footprint(mspace_);
  if (new_size < current_space_size) {
    // Don't let the space grow any more.
    new_size = current_space_size;
  }
  mspace_set_footprint_limit(mspace_, new_size);
}

uint64_t DlMallocSpace::GetBytesAllocated() {
  MutexLock mu(Thread::Current(), lock_);
  size_t bytes_allocated = 0;
  mspace_inspect_all(mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

uint64_t DlMallocSpace::GetObjectsAllocated() {
  MutexLock mu(Thread::Current(), lock_);
  size_t objects_allocated = 0;
  mspace_inspect_all(mspace_, DlmallocObjectsAllocatedCallback, &objects_allocated);
  return objects_allocated;
}

void DlMallocSpace::Clear() {
  size_t footprint_limit = GetFootprintLimit();
  madvise(GetMemMap()->Begin(), GetMemMap()->Size(), MADV_DONTNEED);
  live_bitmap_->Clear();
  mark_bitmap_->Clear();
  SetEnd(Begin() + starting_size_);
  mspace_ = CreateMspace(mem_map_->Begin(), starting_size_, initial_size_);
  SetFootprintLimit(footprint_limit);
}

#ifndef NDEBUG
void DlMallocSpace::CheckMoreCoreForPrecondition() {
  lock_.AssertHeld(Thread::Current());
}
#endif

static void MSpaceChunkCallback(void* start, void* end, size_t used_bytes, void* arg) {
  size_t chunk_size = reinterpret_cast<uint8_t*>(end) - reinterpret_cast<uint8_t*>(start);
  if (used_bytes < chunk_size) {
    size_t chunk_free_bytes = chunk_size - used_bytes;
    size_t& max_contiguous_allocation = *reinterpret_cast<size_t*>(arg);
    max_contiguous_allocation = std::max(max_contiguous_allocation, chunk_free_bytes);
  }
}

void DlMallocSpace::LogFragmentationAllocFailure(std::ostream& os,
                                                 size_t failed_alloc_bytes ATTRIBUTE_UNUSED) {
  Thread* const self = Thread::Current();
  size_t max_contiguous_allocation = 0;
  // To allow the Walk/InspectAll() to exclusively-lock the mutator
  // lock, temporarily release the shared access to the mutator
  // lock here by transitioning to the suspended state.
  Locks::mutator_lock_->AssertSharedHeld(self);
  ScopedThreadSuspension sts(self, kSuspended);
  Walk(MSpaceChunkCallback, &max_contiguous_allocation);
  os << "; failed due to fragmentation (largest possible contiguous allocation "
     <<  max_contiguous_allocation << " bytes)";
}

}  // namespace space

namespace allocator {

// Implement the dlmalloc morecore callback.
void* ArtDlMallocMoreCore(void* mspace, intptr_t increment) SHARED_REQUIRES(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  Heap* heap = runtime->GetHeap();
  ::art::gc::space::DlMallocSpace* dlmalloc_space = heap->GetDlMallocSpace();
  // Support for multiple DlMalloc provided by a slow path.
  if (UNLIKELY(dlmalloc_space == nullptr || dlmalloc_space->GetMspace() != mspace)) {
    if (LIKELY(runtime->GetJit() != nullptr)) {
      jit::JitCodeCache* code_cache = runtime->GetJit()->GetCodeCache();
      if (code_cache->OwnsSpace(mspace)) {
        return code_cache->MoreCore(mspace, increment);
      }
    }
    dlmalloc_space = nullptr;
    for (space::ContinuousSpace* space : heap->GetContinuousSpaces()) {
      if (space->IsDlMallocSpace()) {
        ::art::gc::space::DlMallocSpace* cur_dlmalloc_space = space->AsDlMallocSpace();
        if (cur_dlmalloc_space->GetMspace() == mspace) {
          dlmalloc_space = cur_dlmalloc_space;
          break;
        }
      }
    }
    CHECK(dlmalloc_space != nullptr) << "Couldn't find DlmMallocSpace with mspace=" << mspace;
  }
  return dlmalloc_space->MoreCore(increment);
}

}  // namespace allocator

}  // namespace gc
}  // namespace art
