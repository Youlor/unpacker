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

#include "malloc_space.h"

#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/space/space-inl.h"
#include "gc/space/zygote_space.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

size_t MallocSpace::bitmap_index_ = 0;

MallocSpace::MallocSpace(const std::string& name, MemMap* mem_map,
                         uint8_t* begin, uint8_t* end, uint8_t* limit, size_t growth_limit,
                         bool create_bitmaps, bool can_move_objects, size_t starting_size,
                         size_t initial_size)
    : ContinuousMemMapAllocSpace(name, mem_map, begin, end, limit, kGcRetentionPolicyAlwaysCollect),
      recent_free_pos_(0), lock_("allocation space lock", kAllocSpaceLock),
      growth_limit_(growth_limit), can_move_objects_(can_move_objects),
      starting_size_(starting_size), initial_size_(initial_size) {
  if (create_bitmaps) {
    size_t bitmap_index = bitmap_index_++;
    static const uintptr_t kGcCardSize = static_cast<uintptr_t>(accounting::CardTable::kCardSize);
    CHECK_ALIGNED(reinterpret_cast<uintptr_t>(mem_map->Begin()), kGcCardSize);
    CHECK_ALIGNED(reinterpret_cast<uintptr_t>(mem_map->End()), kGcCardSize);
    live_bitmap_.reset(accounting::ContinuousSpaceBitmap::Create(
        StringPrintf("allocspace %s live-bitmap %d", name.c_str(), static_cast<int>(bitmap_index)),
        Begin(), NonGrowthLimitCapacity()));
    CHECK(live_bitmap_.get() != nullptr) << "could not create allocspace live bitmap #"
        << bitmap_index;
    mark_bitmap_.reset(accounting::ContinuousSpaceBitmap::Create(
        StringPrintf("allocspace %s mark-bitmap %d", name.c_str(), static_cast<int>(bitmap_index)),
        Begin(), NonGrowthLimitCapacity()));
    CHECK(mark_bitmap_.get() != nullptr) << "could not create allocspace mark bitmap #"
        << bitmap_index;
  }
  for (auto& freed : recent_freed_objects_) {
    freed.first = nullptr;
    freed.second = nullptr;
  }
}

MemMap* MallocSpace::CreateMemMap(const std::string& name, size_t starting_size, size_t* initial_size,
                                  size_t* growth_limit, size_t* capacity, uint8_t* requested_begin) {
  // Sanity check arguments
  if (starting_size > *initial_size) {
    *initial_size = starting_size;
  }
  if (*initial_size > *growth_limit) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the initial size ("
        << PrettySize(*initial_size) << ") is larger than its capacity ("
        << PrettySize(*growth_limit) << ")";
    return nullptr;
  }
  if (*growth_limit > *capacity) {
    LOG(ERROR) << "Failed to create alloc space (" << name << ") where the growth limit capacity ("
        << PrettySize(*growth_limit) << ") is larger than the capacity ("
        << PrettySize(*capacity) << ")";
    return nullptr;
  }

  // Page align growth limit and capacity which will be used to manage mmapped storage
  *growth_limit = RoundUp(*growth_limit, kPageSize);
  *capacity = RoundUp(*capacity, kPageSize);

  std::string error_msg;
  MemMap* mem_map = MemMap::MapAnonymous(name.c_str(), requested_begin, *capacity,
                                         PROT_READ | PROT_WRITE, true, false, &error_msg);
  if (mem_map == nullptr) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
               << PrettySize(*capacity) << ": " << error_msg;
  }
  return mem_map;
}

mirror::Class* MallocSpace::FindRecentFreedObject(const mirror::Object* obj) {
  size_t pos = recent_free_pos_;
  // Start at the most recently freed object and work our way back since there may be duplicates
  // caused by dlmalloc reusing memory.
  if (kRecentFreeCount > 0) {
    for (size_t i = 0; i + 1 < kRecentFreeCount + 1; ++i) {
      pos = pos != 0 ? pos - 1 : kRecentFreeMask;
      if (recent_freed_objects_[pos].first == obj) {
        return recent_freed_objects_[pos].second;
      }
    }
  }
  return nullptr;
}

void MallocSpace::RegisterRecentFree(mirror::Object* ptr) {
  // No verification since the object is dead.
  recent_freed_objects_[recent_free_pos_] = std::make_pair(ptr, ptr->GetClass<kVerifyNone>());
  recent_free_pos_ = (recent_free_pos_ + 1) & kRecentFreeMask;
}

void MallocSpace::SetGrowthLimit(size_t growth_limit) {
  growth_limit = RoundUp(growth_limit, kPageSize);
  growth_limit_ = growth_limit;
  if (Size() > growth_limit_) {
    SetEnd(begin_ + growth_limit);
  }
}

void* MallocSpace::MoreCore(intptr_t increment) {
  CheckMoreCoreForPrecondition();
  uint8_t* original_end = End();
  if (increment != 0) {
    VLOG(heap) << "MallocSpace::MoreCore " << PrettySize(increment);
    uint8_t* new_end = original_end + increment;
    if (increment > 0) {
      // Should never be asked to increase the allocation beyond the capacity of the space. Enforced
      // by mspace_set_footprint_limit.
      CHECK_LE(new_end, Begin() + Capacity());
      CHECK_MEMORY_CALL(mprotect, (original_end, increment, PROT_READ | PROT_WRITE), GetName());
    } else {
      // Should never be asked for negative footprint (ie before begin). Zero footprint is ok.
      CHECK_GE(original_end + increment, Begin());
      // Advise we don't need the pages and protect them
      // TODO: by removing permissions to the pages we may be causing TLB shoot-down which can be
      // expensive (note the same isn't true for giving permissions to a page as the protected
      // page shouldn't be in a TLB). We should investigate performance impact of just
      // removing ignoring the memory protection change here and in Space::CreateAllocSpace. It's
      // likely just a useful debug feature.
      size_t size = -increment;
      CHECK_MEMORY_CALL(madvise, (new_end, size, MADV_DONTNEED), GetName());
      CHECK_MEMORY_CALL(mprotect, (new_end, size, PROT_NONE), GetName());
    }
    // Update end_.
    SetEnd(new_end);
  }
  return original_end;
}

ZygoteSpace* MallocSpace::CreateZygoteSpace(const char* alloc_space_name, bool low_memory_mode,
                                            MallocSpace** out_malloc_space) {
  // For RosAlloc, revoke thread local runs before creating a new
  // alloc space so that we won't mix thread local runs from different
  // alloc spaces.
  RevokeAllThreadLocalBuffers();
  SetEnd(reinterpret_cast<uint8_t*>(RoundUp(reinterpret_cast<uintptr_t>(End()), kPageSize)));
  DCHECK_ALIGNED(begin_, accounting::CardTable::kCardSize);
  DCHECK_ALIGNED(End(), accounting::CardTable::kCardSize);
  DCHECK_ALIGNED(begin_, kPageSize);
  DCHECK_ALIGNED(End(), kPageSize);
  size_t size = RoundUp(Size(), kPageSize);
  // Trimming the heap should be done by the caller since we may have invalidated the accounting
  // stored in between objects.
  // Remaining size is for the new alloc space.
  const size_t growth_limit = growth_limit_ - size;
  // Use mem map limit in case error for clear growth limit.
  const size_t capacity = NonGrowthLimitCapacity() - size;
  VLOG(heap) << "Begin " << reinterpret_cast<const void*>(begin_) << "\n"
             << "End " << reinterpret_cast<const void*>(End()) << "\n"
             << "Size " << size << "\n"
             << "GrowthLimit " << growth_limit_ << "\n"
             << "Capacity " << Capacity();
  SetGrowthLimit(RoundUp(size, kPageSize));
  // FIXME: Do we need reference counted pointers here?
  // Make the two spaces share the same mark bitmaps since the bitmaps span both of the spaces.
  VLOG(heap) << "Creating new AllocSpace: ";
  VLOG(heap) << "Size " << GetMemMap()->Size();
  VLOG(heap) << "GrowthLimit " << PrettySize(growth_limit);
  VLOG(heap) << "Capacity " << PrettySize(capacity);
  // Remap the tail.
  std::string error_msg;
  std::unique_ptr<MemMap> mem_map(GetMemMap()->RemapAtEnd(End(), alloc_space_name,
                                                          PROT_READ | PROT_WRITE, &error_msg));
  CHECK(mem_map.get() != nullptr) << error_msg;
  void* allocator = CreateAllocator(End(), starting_size_, initial_size_, capacity,
                                    low_memory_mode);
  // Protect memory beyond the initial size.
  uint8_t* end = mem_map->Begin() + starting_size_;
  if (capacity > initial_size_) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - initial_size_, PROT_NONE), alloc_space_name);
  }
  *out_malloc_space = CreateInstance(mem_map.release(), alloc_space_name, allocator, End(), end,
                                     limit_, growth_limit, CanMoveObjects());
  SetLimit(End());
  live_bitmap_->SetHeapLimit(reinterpret_cast<uintptr_t>(End()));
  CHECK_EQ(live_bitmap_->HeapLimit(), reinterpret_cast<uintptr_t>(End()));
  mark_bitmap_->SetHeapLimit(reinterpret_cast<uintptr_t>(End()));
  CHECK_EQ(mark_bitmap_->HeapLimit(), reinterpret_cast<uintptr_t>(End()));

  // Create the actual zygote space.
  ZygoteSpace* zygote_space = ZygoteSpace::Create("Zygote space", ReleaseMemMap(),
                                                  live_bitmap_.release(), mark_bitmap_.release());
  if (UNLIKELY(zygote_space == nullptr)) {
    VLOG(heap) << "Failed creating zygote space from space " << GetName();
  } else {
    VLOG(heap) << "zygote space creation done";
  }
  return zygote_space;
}

void MallocSpace::Dump(std::ostream& os) const {
  os << GetType()
     << " begin=" << reinterpret_cast<void*>(Begin())
     << ",end=" << reinterpret_cast<void*>(End())
     << ",limit=" << reinterpret_cast<void*>(Limit())
     << ",size=" << PrettySize(Size()) << ",capacity=" << PrettySize(Capacity())
     << ",non_growth_limit_capacity=" << PrettySize(NonGrowthLimitCapacity())
     << ",name=\"" << GetName() << "\"]";
}

void MallocSpace::SweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  space::MallocSpace* space = context->space->AsMallocSpace();
  Thread* self = context->self;
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(self);
  // If the bitmaps aren't swapped we need to clear the bits since the GC isn't going to re-swap
  // the bitmaps as an optimization.
  if (!context->swap_bitmaps) {
    accounting::ContinuousSpaceBitmap* bitmap = space->GetLiveBitmap();
    for (size_t i = 0; i < num_ptrs; ++i) {
      bitmap->Clear(ptrs[i]);
    }
  }
  // Use a bulk free, that merges consecutive objects before freeing or free per object?
  // Documentation suggests better free performance with merging, but this may be at the expensive
  // of allocation.
  context->freed.objects += num_ptrs;
  context->freed.bytes += space->FreeList(self, num_ptrs, ptrs);
}

void MallocSpace::ClampGrowthLimit() {
  size_t new_capacity = Capacity();
  CHECK_LE(new_capacity, NonGrowthLimitCapacity());
  GetLiveBitmap()->SetHeapSize(new_capacity);
  GetMarkBitmap()->SetHeapSize(new_capacity);
  if (temp_bitmap_.get() != nullptr) {
    // If the bitmaps are clamped, then the temp bitmap is actually the mark bitmap.
    temp_bitmap_->SetHeapSize(new_capacity);
  }
  GetMemMap()->SetSize(new_capacity);
  limit_ = Begin() + new_capacity;
}

}  // namespace space
}  // namespace gc
}  // namespace art
