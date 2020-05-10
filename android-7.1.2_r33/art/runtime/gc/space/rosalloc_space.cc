
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

#include "rosalloc_space-inl.h"

#include "base/time_utils.h"
#include "gc/accounting/card_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"
#include "memory_tool_malloc_space-inl.h"

namespace art {
namespace gc {
namespace space {

static constexpr bool kPrefetchDuringRosAllocFreeList = false;
static constexpr size_t kPrefetchLookAhead = 8;
// Use this only for verification, it is not safe to use since the class of the object may have
// been freed.
static constexpr bool kVerifyFreedBytes = false;

// TODO: Fix
// template class MemoryToolMallocSpace<RosAllocSpace, allocator::RosAlloc*>;

RosAllocSpace::RosAllocSpace(MemMap* mem_map, size_t initial_size, const std::string& name,
                             art::gc::allocator::RosAlloc* rosalloc, uint8_t* begin, uint8_t* end,
                             uint8_t* limit, size_t growth_limit, bool can_move_objects,
                             size_t starting_size, bool low_memory_mode)
    : MallocSpace(name, mem_map, begin, end, limit, growth_limit, true, can_move_objects,
                  starting_size, initial_size),
      rosalloc_(rosalloc), low_memory_mode_(low_memory_mode) {
  CHECK(rosalloc != nullptr);
}

RosAllocSpace* RosAllocSpace::CreateFromMemMap(MemMap* mem_map, const std::string& name,
                                               size_t starting_size, size_t initial_size,
                                               size_t growth_limit, size_t capacity,
                                               bool low_memory_mode, bool can_move_objects) {
  DCHECK(mem_map != nullptr);

  bool running_on_memory_tool = Runtime::Current()->IsRunningOnMemoryTool();

  allocator::RosAlloc* rosalloc = CreateRosAlloc(mem_map->Begin(), starting_size, initial_size,
                                                 capacity, low_memory_mode, running_on_memory_tool);
  if (rosalloc == nullptr) {
    LOG(ERROR) << "Failed to initialize rosalloc for alloc space (" << name << ")";
    return nullptr;
  }

  // Protect memory beyond the starting size. MoreCore will add r/w permissions when necessory
  uint8_t* end = mem_map->Begin() + starting_size;
  if (capacity - starting_size > 0) {
    CHECK_MEMORY_CALL(mprotect, (end, capacity - starting_size, PROT_NONE), name);
  }

  // Everything is set so record in immutable structure and leave
  uint8_t* begin = mem_map->Begin();
  // TODO: Fix RosAllocSpace to support Valgrind/ASan. There is currently some issues with
  // AllocationSize caused by redzones. b/12944686
  if (running_on_memory_tool) {
    return new MemoryToolMallocSpace<RosAllocSpace, kDefaultMemoryToolRedZoneBytes, false, true>(
        mem_map, initial_size, name, rosalloc, begin, end, begin + capacity, growth_limit,
        can_move_objects, starting_size, low_memory_mode);
  } else {
    return new RosAllocSpace(mem_map, initial_size, name, rosalloc, begin, end, begin + capacity,
                             growth_limit, can_move_objects, starting_size, low_memory_mode);
  }
}

RosAllocSpace::~RosAllocSpace() {
  delete rosalloc_;
}

RosAllocSpace* RosAllocSpace::Create(const std::string& name, size_t initial_size,
                                     size_t growth_limit, size_t capacity, uint8_t* requested_begin,
                                     bool low_memory_mode, bool can_move_objects) {
  uint64_t start_time = 0;
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    start_time = NanoTime();
    VLOG(startup) << "RosAllocSpace::Create entering " << name
                  << " initial_size=" << PrettySize(initial_size)
                  << " growth_limit=" << PrettySize(growth_limit)
                  << " capacity=" << PrettySize(capacity)
                  << " requested_begin=" << reinterpret_cast<void*>(requested_begin);
  }

  // Memory we promise to rosalloc before it asks for morecore.
  // Note: making this value large means that large allocations are unlikely to succeed as rosalloc
  // will ask for this memory from sys_alloc which will fail as the footprint (this value plus the
  // size of the large allocation) will be greater than the footprint limit.
  size_t starting_size = Heap::kDefaultStartingSize;
  MemMap* mem_map = CreateMemMap(name, starting_size, &initial_size, &growth_limit, &capacity,
                                 requested_begin);
  if (mem_map == nullptr) {
    LOG(ERROR) << "Failed to create mem map for alloc space (" << name << ") of size "
               << PrettySize(capacity);
    return nullptr;
  }

  RosAllocSpace* space = CreateFromMemMap(mem_map, name, starting_size, initial_size,
                                          growth_limit, capacity, low_memory_mode,
                                          can_move_objects);
  // We start out with only the initial size possibly containing objects.
  if (VLOG_IS_ON(heap) || VLOG_IS_ON(startup)) {
    LOG(INFO) << "RosAllocSpace::Create exiting (" << PrettyDuration(NanoTime() - start_time)
        << " ) " << *space;
  }
  return space;
}

allocator::RosAlloc* RosAllocSpace::CreateRosAlloc(void* begin, size_t morecore_start,
                                                   size_t initial_size,
                                                   size_t maximum_size, bool low_memory_mode,
                                                   bool running_on_memory_tool) {
  // clear errno to allow PLOG on error
  errno = 0;
  // create rosalloc using our backing storage starting at begin and
  // with a footprint of morecore_start. When morecore_start bytes of
  // memory is exhaused morecore will be called.
  allocator::RosAlloc* rosalloc = new art::gc::allocator::RosAlloc(
      begin, morecore_start, maximum_size,
      low_memory_mode ?
          art::gc::allocator::RosAlloc::kPageReleaseModeAll :
          art::gc::allocator::RosAlloc::kPageReleaseModeSizeAndEnd,
      running_on_memory_tool);
  if (rosalloc != nullptr) {
    rosalloc->SetFootprintLimit(initial_size);
  } else {
    PLOG(ERROR) << "RosAlloc::Create failed";
  }
  return rosalloc;
}

mirror::Object* RosAllocSpace::AllocWithGrowth(Thread* self, size_t num_bytes,
                                               size_t* bytes_allocated, size_t* usable_size,
                                               size_t* bytes_tl_bulk_allocated) {
  mirror::Object* result;
  {
    MutexLock mu(self, lock_);
    // Grow as much as possible within the space.
    size_t max_allowed = Capacity();
    rosalloc_->SetFootprintLimit(max_allowed);
    // Try the allocation.
    result = AllocCommon(self, num_bytes, bytes_allocated, usable_size,
                         bytes_tl_bulk_allocated);
    // Shrink back down as small as possible.
    size_t footprint = rosalloc_->Footprint();
    rosalloc_->SetFootprintLimit(footprint);
  }
  // Note RosAlloc zeroes memory internally.
  // Return the new allocation or null.
  CHECK(!kDebugSpaces || result == nullptr || Contains(result));
  return result;
}

MallocSpace* RosAllocSpace::CreateInstance(MemMap* mem_map, const std::string& name,
                                           void* allocator, uint8_t* begin, uint8_t* end,
                                           uint8_t* limit, size_t growth_limit,
                                           bool can_move_objects) {
  if (Runtime::Current()->IsRunningOnMemoryTool()) {
    return new MemoryToolMallocSpace<RosAllocSpace, kDefaultMemoryToolRedZoneBytes, false, true>(
        mem_map, initial_size_, name, reinterpret_cast<allocator::RosAlloc*>(allocator), begin, end,
        limit, growth_limit, can_move_objects, starting_size_, low_memory_mode_);
  } else {
    return new RosAllocSpace(mem_map, initial_size_, name,
                             reinterpret_cast<allocator::RosAlloc*>(allocator), begin, end, limit,
                             growth_limit, can_move_objects, starting_size_, low_memory_mode_);
  }
}

size_t RosAllocSpace::Free(Thread* self, mirror::Object* ptr) {
  if (kDebugSpaces) {
    CHECK(ptr != nullptr);
    CHECK(Contains(ptr)) << "Free (" << ptr << ") not in bounds of heap " << *this;
  }
  if (kRecentFreeCount > 0) {
    MutexLock mu(self, lock_);
    RegisterRecentFree(ptr);
  }
  return rosalloc_->Free(self, ptr);
}

size_t RosAllocSpace::FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) {
  DCHECK(ptrs != nullptr);

  size_t verify_bytes = 0;
  for (size_t i = 0; i < num_ptrs; i++) {
    if (kPrefetchDuringRosAllocFreeList && i + kPrefetchLookAhead < num_ptrs) {
      __builtin_prefetch(reinterpret_cast<char*>(ptrs[i + kPrefetchLookAhead]));
    }
    if (kVerifyFreedBytes) {
      verify_bytes += AllocationSizeNonvirtual<true>(ptrs[i], nullptr);
    }
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
        size_t size = rosalloc_->UsableSize(ptrs[i]);
        memset(ptrs[i], 0xEF, size);
      }
    }
    CHECK_EQ(num_broken_ptrs, 0u);
  }

  const size_t bytes_freed = rosalloc_->BulkFree(self, reinterpret_cast<void**>(ptrs), num_ptrs);
  if (kVerifyFreedBytes) {
    CHECK_EQ(verify_bytes, bytes_freed);
  }
  return bytes_freed;
}

size_t RosAllocSpace::Trim() {
  VLOG(heap) << "RosAllocSpace::Trim() ";
  {
    Thread* const self = Thread::Current();
    // SOA required for Rosalloc::Trim() -> ArtRosAllocMoreCore() -> Heap::GetRosAllocSpace.
    ScopedObjectAccess soa(self);
    MutexLock mu(self, lock_);
    // Trim to release memory at the end of the space.
    rosalloc_->Trim();
  }
  // Attempt to release pages if it does not release all empty pages.
  if (!rosalloc_->DoesReleaseAllPages()) {
    return rosalloc_->ReleasePages();
  }
  return 0;
}

void RosAllocSpace::Walk(void(*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                         void* arg) {
  InspectAllRosAlloc(callback, arg, true);
}

size_t RosAllocSpace::GetFootprint() {
  MutexLock mu(Thread::Current(), lock_);
  return rosalloc_->Footprint();
}

size_t RosAllocSpace::GetFootprintLimit() {
  MutexLock mu(Thread::Current(), lock_);
  return rosalloc_->FootprintLimit();
}

void RosAllocSpace::SetFootprintLimit(size_t new_size) {
  MutexLock mu(Thread::Current(), lock_);
  VLOG(heap) << "RosAllocSpace::SetFootprintLimit " << PrettySize(new_size);
  // Compare against the actual footprint, rather than the Size(), because the heap may not have
  // grown all the way to the allowed size yet.
  size_t current_space_size = rosalloc_->Footprint();
  if (new_size < current_space_size) {
    // Don't let the space grow any more.
    new_size = current_space_size;
  }
  rosalloc_->SetFootprintLimit(new_size);
}

uint64_t RosAllocSpace::GetBytesAllocated() {
  size_t bytes_allocated = 0;
  InspectAllRosAlloc(art::gc::allocator::RosAlloc::BytesAllocatedCallback, &bytes_allocated, false);
  return bytes_allocated;
}

uint64_t RosAllocSpace::GetObjectsAllocated() {
  size_t objects_allocated = 0;
  InspectAllRosAlloc(art::gc::allocator::RosAlloc::ObjectsAllocatedCallback, &objects_allocated, false);
  return objects_allocated;
}

void RosAllocSpace::InspectAllRosAllocWithSuspendAll(
    void (*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
    void* arg, bool do_null_callback_at_end) NO_THREAD_SAFETY_ANALYSIS {
  // TODO: NO_THREAD_SAFETY_ANALYSIS.
  Thread* self = Thread::Current();
  ScopedSuspendAll ssa(__FUNCTION__);
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  rosalloc_->InspectAll(callback, arg);
  if (do_null_callback_at_end) {
    callback(nullptr, nullptr, 0, arg);  // Indicate end of a space.
  }
}

void RosAllocSpace::InspectAllRosAlloc(void (*callback)(void *start, void *end, size_t num_bytes, void* callback_arg),
                                       void* arg, bool do_null_callback_at_end) NO_THREAD_SAFETY_ANALYSIS {
  // TODO: NO_THREAD_SAFETY_ANALYSIS.
  Thread* self = Thread::Current();
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // The mutators are already suspended. For example, a call path
    // from SignalCatcher::HandleSigQuit().
    rosalloc_->InspectAll(callback, arg);
    if (do_null_callback_at_end) {
      callback(nullptr, nullptr, 0, arg);  // Indicate end of a space.
    }
  } else if (Locks::mutator_lock_->IsSharedHeld(self)) {
    // The mutators are not suspended yet and we have a shared access
    // to the mutator lock. Temporarily release the shared access by
    // transitioning to the suspend state, and suspend the mutators.
    ScopedThreadSuspension sts(self, kSuspended);
    InspectAllRosAllocWithSuspendAll(callback, arg, do_null_callback_at_end);
  } else {
    // The mutators are not suspended yet. Suspend the mutators.
    InspectAllRosAllocWithSuspendAll(callback, arg, do_null_callback_at_end);
  }
}

size_t RosAllocSpace::RevokeThreadLocalBuffers(Thread* thread) {
  return rosalloc_->RevokeThreadLocalRuns(thread);
}

size_t RosAllocSpace::RevokeAllThreadLocalBuffers() {
  return rosalloc_->RevokeAllThreadLocalRuns();
}

void RosAllocSpace::AssertThreadLocalBuffersAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    rosalloc_->AssertThreadLocalRunsAreRevoked(thread);
  }
}

void RosAllocSpace::AssertAllThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    rosalloc_->AssertAllThreadLocalRunsAreRevoked();
  }
}

void RosAllocSpace::Clear() {
  size_t footprint_limit = GetFootprintLimit();
  madvise(GetMemMap()->Begin(), GetMemMap()->Size(), MADV_DONTNEED);
  live_bitmap_->Clear();
  mark_bitmap_->Clear();
  SetEnd(begin_ + starting_size_);
  delete rosalloc_;
  rosalloc_ = CreateRosAlloc(mem_map_->Begin(), starting_size_, initial_size_,
                             NonGrowthLimitCapacity(), low_memory_mode_,
                             Runtime::Current()->IsRunningOnMemoryTool());
  SetFootprintLimit(footprint_limit);
}

void RosAllocSpace::DumpStats(std::ostream& os) {
  ScopedSuspendAll ssa(__FUNCTION__);
  rosalloc_->DumpStats(os);
}

}  // namespace space

namespace allocator {

// Callback from rosalloc when it needs to increase the footprint.
void* ArtRosAllocMoreCore(allocator::RosAlloc* rosalloc, intptr_t increment)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Heap* heap = Runtime::Current()->GetHeap();
  art::gc::space::RosAllocSpace* rosalloc_space = heap->GetRosAllocSpace(rosalloc);
  DCHECK(rosalloc_space != nullptr);
  DCHECK_EQ(rosalloc_space->GetRosAlloc(), rosalloc);
  return rosalloc_space->MoreCore(increment);
}

}  // namespace allocator

}  // namespace gc
}  // namespace art
