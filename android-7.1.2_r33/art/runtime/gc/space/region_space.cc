/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "bump_pointer_space.h"
#include "bump_pointer_space-inl.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"
#include "thread_list.h"

namespace art {
namespace gc {
namespace space {

// If a region has live objects whose size is less than this percent
// value of the region size, evaculate the region.
static constexpr uint kEvaculateLivePercentThreshold = 75U;

RegionSpace* RegionSpace::Create(const std::string& name, size_t capacity,
                                 uint8_t* requested_begin) {
  capacity = RoundUp(capacity, kRegionSize);
  std::string error_msg;
  std::unique_ptr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), requested_begin, capacity,
                                                       PROT_READ | PROT_WRITE, true, false,
                                                       &error_msg));
  if (mem_map.get() == nullptr) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
        << PrettySize(capacity) << " with message " << error_msg;
    MemMap::DumpMaps(LOG(ERROR));
    return nullptr;
  }
  return new RegionSpace(name, mem_map.release());
}

RegionSpace::RegionSpace(const std::string& name, MemMap* mem_map)
    : ContinuousMemMapAllocSpace(name, mem_map, mem_map->Begin(), mem_map->End(), mem_map->End(),
                                 kGcRetentionPolicyAlwaysCollect),
      region_lock_("Region lock", kRegionSpaceRegionLock), time_(1U) {
  size_t mem_map_size = mem_map->Size();
  CHECK_ALIGNED(mem_map_size, kRegionSize);
  CHECK_ALIGNED(mem_map->Begin(), kRegionSize);
  num_regions_ = mem_map_size / kRegionSize;
  num_non_free_regions_ = 0U;
  DCHECK_GT(num_regions_, 0U);
  regions_.reset(new Region[num_regions_]);
  uint8_t* region_addr = mem_map->Begin();
  for (size_t i = 0; i < num_regions_; ++i, region_addr += kRegionSize) {
    regions_[i] = Region(i, region_addr, region_addr + kRegionSize);
  }
  if (kIsDebugBuild) {
    CHECK_EQ(regions_[0].Begin(), Begin());
    for (size_t i = 0; i < num_regions_; ++i) {
      CHECK(regions_[i].IsFree());
      CHECK_EQ(static_cast<size_t>(regions_[i].End() - regions_[i].Begin()), kRegionSize);
      if (i + 1 < num_regions_) {
        CHECK_EQ(regions_[i].End(), regions_[i + 1].Begin());
      }
    }
    CHECK_EQ(regions_[num_regions_ - 1].End(), Limit());
  }
  full_region_ = Region();
  DCHECK(!full_region_.IsFree());
  DCHECK(full_region_.IsAllocated());
  current_region_ = &full_region_;
  evac_region_ = nullptr;
  size_t ignored;
  DCHECK(full_region_.Alloc(kAlignment, &ignored, nullptr, &ignored) == nullptr);
}

size_t RegionSpace::FromSpaceSize() {
  uint64_t num_regions = 0;
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    if (r->IsInFromSpace()) {
      ++num_regions;
    }
  }
  return num_regions * kRegionSize;
}

size_t RegionSpace::UnevacFromSpaceSize() {
  uint64_t num_regions = 0;
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    if (r->IsInUnevacFromSpace()) {
      ++num_regions;
    }
  }
  return num_regions * kRegionSize;
}

size_t RegionSpace::ToSpaceSize() {
  uint64_t num_regions = 0;
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    if (r->IsInToSpace()) {
      ++num_regions;
    }
  }
  return num_regions * kRegionSize;
}

inline bool RegionSpace::Region::ShouldBeEvacuated() {
  DCHECK((IsAllocated() || IsLarge()) && IsInToSpace());
  // if the region was allocated after the start of the
  // previous GC or the live ratio is below threshold, evacuate
  // it.
  bool result;
  if (is_newly_allocated_) {
    result = true;
  } else {
    bool is_live_percent_valid = live_bytes_ != static_cast<size_t>(-1);
    if (is_live_percent_valid) {
      uint live_percent = GetLivePercent();
      if (IsAllocated()) {
        // Side node: live_percent == 0 does not necessarily mean
        // there's no live objects due to rounding (there may be a
        // few).
        result = live_percent < kEvaculateLivePercentThreshold;
      } else {
        DCHECK(IsLarge());
        result = live_percent == 0U;
      }
    } else {
      result = false;
    }
  }
  return result;
}

// Determine which regions to evacuate and mark them as
// from-space. Mark the rest as unevacuated from-space.
void RegionSpace::SetFromSpace(accounting::ReadBarrierTable* rb_table, bool force_evacuate_all) {
  ++time_;
  if (kUseTableLookupReadBarrier) {
    DCHECK(rb_table->IsAllCleared());
    rb_table->SetAll();
  }
  MutexLock mu(Thread::Current(), region_lock_);
  size_t num_expected_large_tails = 0;
  bool prev_large_evacuated = false;
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    RegionState state = r->State();
    RegionType type = r->Type();
    if (!r->IsFree()) {
      DCHECK(r->IsInToSpace());
      if (LIKELY(num_expected_large_tails == 0U)) {
        DCHECK((state == RegionState::kRegionStateAllocated ||
                state == RegionState::kRegionStateLarge) &&
               type == RegionType::kRegionTypeToSpace);
        bool should_evacuate = force_evacuate_all || r->ShouldBeEvacuated();
        if (should_evacuate) {
          r->SetAsFromSpace();
          DCHECK(r->IsInFromSpace());
        } else {
          r->SetAsUnevacFromSpace();
          DCHECK(r->IsInUnevacFromSpace());
        }
        if (UNLIKELY(state == RegionState::kRegionStateLarge &&
                     type == RegionType::kRegionTypeToSpace)) {
          prev_large_evacuated = should_evacuate;
          num_expected_large_tails = RoundUp(r->BytesAllocated(), kRegionSize) / kRegionSize - 1;
          DCHECK_GT(num_expected_large_tails, 0U);
        }
      } else {
        DCHECK(state == RegionState::kRegionStateLargeTail &&
               type == RegionType::kRegionTypeToSpace);
        if (prev_large_evacuated) {
          r->SetAsFromSpace();
          DCHECK(r->IsInFromSpace());
        } else {
          r->SetAsUnevacFromSpace();
          DCHECK(r->IsInUnevacFromSpace());
        }
        --num_expected_large_tails;
      }
    } else {
      DCHECK_EQ(num_expected_large_tails, 0U);
      if (kUseTableLookupReadBarrier) {
        // Clear the rb table for to-space regions.
        rb_table->Clear(r->Begin(), r->End());
      }
    }
  }
  current_region_ = &full_region_;
  evac_region_ = &full_region_;
}

void RegionSpace::ClearFromSpace() {
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    if (r->IsInFromSpace()) {
      r->Clear();
      --num_non_free_regions_;
    } else if (r->IsInUnevacFromSpace()) {
      r->SetUnevacFromSpaceAsToSpace();
    }
  }
  evac_region_ = nullptr;
}

void RegionSpace::AssertAllRegionLiveBytesZeroOrCleared() {
  if (kIsDebugBuild) {
    MutexLock mu(Thread::Current(), region_lock_);
    for (size_t i = 0; i < num_regions_; ++i) {
      Region* r = &regions_[i];
      size_t live_bytes = r->LiveBytes();
      CHECK(live_bytes == 0U || live_bytes == static_cast<size_t>(-1)) << live_bytes;
    }
  }
}

void RegionSpace::LogFragmentationAllocFailure(std::ostream& os,
                                               size_t /* failed_alloc_bytes */) {
  size_t max_contiguous_allocation = 0;
  MutexLock mu(Thread::Current(), region_lock_);
  if (current_region_->End() - current_region_->Top() > 0) {
    max_contiguous_allocation = current_region_->End() - current_region_->Top();
  }
  if (num_non_free_regions_ * 2 < num_regions_) {
    // We reserve half of the regions for evaluation only. If we
    // occupy more than half the regions, do not report the free
    // regions as available.
    size_t max_contiguous_free_regions = 0;
    size_t num_contiguous_free_regions = 0;
    bool prev_free_region = false;
    for (size_t i = 0; i < num_regions_; ++i) {
      Region* r = &regions_[i];
      if (r->IsFree()) {
        if (!prev_free_region) {
          CHECK_EQ(num_contiguous_free_regions, 0U);
          prev_free_region = true;
        }
        ++num_contiguous_free_regions;
      } else {
        if (prev_free_region) {
          CHECK_NE(num_contiguous_free_regions, 0U);
          max_contiguous_free_regions = std::max(max_contiguous_free_regions,
                                                 num_contiguous_free_regions);
          num_contiguous_free_regions = 0U;
          prev_free_region = false;
        }
      }
    }
    max_contiguous_allocation = std::max(max_contiguous_allocation,
                                         max_contiguous_free_regions * kRegionSize);
  }
  os << "; failed due to fragmentation (largest possible contiguous allocation "
     <<  max_contiguous_allocation << " bytes)";
  // Caller's job to print failed_alloc_bytes.
}

void RegionSpace::Clear() {
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    if (!r->IsFree()) {
      --num_non_free_regions_;
    }
    r->Clear();
  }
  current_region_ = &full_region_;
  evac_region_ = &full_region_;
}

void RegionSpace::Dump(std::ostream& os) const {
  os << GetName() << " "
      << reinterpret_cast<void*>(Begin()) << "-" << reinterpret_cast<void*>(Limit());
}

void RegionSpace::FreeLarge(mirror::Object* large_obj, size_t bytes_allocated) {
  DCHECK(Contains(large_obj));
  DCHECK_ALIGNED(large_obj, kRegionSize);
  MutexLock mu(Thread::Current(), region_lock_);
  uint8_t* begin_addr = reinterpret_cast<uint8_t*>(large_obj);
  uint8_t* end_addr = AlignUp(reinterpret_cast<uint8_t*>(large_obj) + bytes_allocated, kRegionSize);
  CHECK_LT(begin_addr, end_addr);
  for (uint8_t* addr = begin_addr; addr < end_addr; addr += kRegionSize) {
    Region* reg = RefToRegionLocked(reinterpret_cast<mirror::Object*>(addr));
    if (addr == begin_addr) {
      DCHECK(reg->IsLarge());
    } else {
      DCHECK(reg->IsLargeTail());
    }
    reg->Clear();
    --num_non_free_regions_;
  }
  if (end_addr < Limit()) {
    // If we aren't at the end of the space, check that the next region is not a large tail.
    Region* following_reg = RefToRegionLocked(reinterpret_cast<mirror::Object*>(end_addr));
    DCHECK(!following_reg->IsLargeTail());
  }
}

void RegionSpace::DumpRegions(std::ostream& os) {
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    regions_[i].Dump(os);
  }
}

void RegionSpace::DumpNonFreeRegions(std::ostream& os) {
  MutexLock mu(Thread::Current(), region_lock_);
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* reg = &regions_[i];
    if (!reg->IsFree()) {
      reg->Dump(os);
    }
  }
}

void RegionSpace::RecordAlloc(mirror::Object* ref) {
  CHECK(ref != nullptr);
  Region* r = RefToRegion(ref);
  reinterpret_cast<Atomic<uint64_t>*>(&r->objects_allocated_)->FetchAndAddSequentiallyConsistent(1);
}

bool RegionSpace::AllocNewTlab(Thread* self) {
  MutexLock mu(self, region_lock_);
  RevokeThreadLocalBuffersLocked(self);
  // Retain sufficient free regions for full evacuation.
  if ((num_non_free_regions_ + 1) * 2 > num_regions_) {
    return false;
  }
  for (size_t i = 0; i < num_regions_; ++i) {
    Region* r = &regions_[i];
    if (r->IsFree()) {
      r->Unfree(time_);
      ++num_non_free_regions_;
      // TODO: this is buggy. Debug it.
      // r->SetNewlyAllocated();
      r->SetTop(r->End());
      r->is_a_tlab_ = true;
      r->thread_ = self;
      self->SetTlab(r->Begin(), r->End());
      return true;
    }
  }
  return false;
}

size_t RegionSpace::RevokeThreadLocalBuffers(Thread* thread) {
  MutexLock mu(Thread::Current(), region_lock_);
  RevokeThreadLocalBuffersLocked(thread);
  return 0U;
}

void RegionSpace::RevokeThreadLocalBuffersLocked(Thread* thread) {
  uint8_t* tlab_start = thread->GetTlabStart();
  DCHECK_EQ(thread->HasTlab(), tlab_start != nullptr);
  if (tlab_start != nullptr) {
    DCHECK_ALIGNED(tlab_start, kRegionSize);
    Region* r = RefToRegionLocked(reinterpret_cast<mirror::Object*>(tlab_start));
    DCHECK(r->IsAllocated());
    DCHECK_EQ(thread->GetThreadLocalBytesAllocated(), kRegionSize);
    r->RecordThreadLocalAllocations(thread->GetThreadLocalObjectsAllocated(),
                                    thread->GetThreadLocalBytesAllocated());
    r->is_a_tlab_ = false;
    r->thread_ = nullptr;
  }
  thread->SetTlab(nullptr, nullptr);
}

size_t RegionSpace::RevokeAllThreadLocalBuffers() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* thread : thread_list) {
    RevokeThreadLocalBuffers(thread);
  }
  return 0U;
}

void RegionSpace::AssertThreadLocalBuffersAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    DCHECK(!thread->HasTlab());
  }
}

void RegionSpace::AssertAllThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    Thread* self = Thread::Current();
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    MutexLock mu2(self, *Locks::thread_list_lock_);
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (Thread* thread : thread_list) {
      AssertThreadLocalBuffersAreRevoked(thread);
    }
  }
}

void RegionSpace::Region::Dump(std::ostream& os) const {
  os << "Region[" << idx_ << "]=" << reinterpret_cast<void*>(begin_) << "-" << reinterpret_cast<void*>(top_)
     << "-" << reinterpret_cast<void*>(end_)
     << " state=" << static_cast<uint>(state_) << " type=" << static_cast<uint>(type_)
     << " objects_allocated=" << objects_allocated_
     << " alloc_time=" << alloc_time_ << " live_bytes=" << live_bytes_
     << " is_newly_allocated=" << is_newly_allocated_ << " is_a_tlab=" << is_a_tlab_ << " thread=" << thread_ << "\n";
}

}  // namespace space
}  // namespace gc
}  // namespace art
