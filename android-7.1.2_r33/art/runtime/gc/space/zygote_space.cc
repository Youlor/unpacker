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

#include "zygote_space.h"

#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {

class CountObjectsAllocated {
 public:
  explicit CountObjectsAllocated(size_t* objects_allocated)
      : objects_allocated_(objects_allocated) {}

  void operator()(mirror::Object* obj ATTRIBUTE_UNUSED) const {
    ++*objects_allocated_;
  }

 private:
  size_t* const objects_allocated_;
};

ZygoteSpace* ZygoteSpace::Create(const std::string& name, MemMap* mem_map,
                                 accounting::ContinuousSpaceBitmap* live_bitmap,
                                 accounting::ContinuousSpaceBitmap* mark_bitmap) {
  DCHECK(live_bitmap != nullptr);
  DCHECK(mark_bitmap != nullptr);
  size_t objects_allocated = 0;
  CountObjectsAllocated visitor(&objects_allocated);
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(mem_map->Begin()),
                                reinterpret_cast<uintptr_t>(mem_map->End()), visitor);
  ZygoteSpace* zygote_space = new ZygoteSpace(name, mem_map, objects_allocated);
  CHECK(zygote_space->live_bitmap_.get() == nullptr);
  CHECK(zygote_space->mark_bitmap_.get() == nullptr);
  zygote_space->live_bitmap_.reset(live_bitmap);
  zygote_space->mark_bitmap_.reset(mark_bitmap);
  return zygote_space;
}

void ZygoteSpace::Clear() {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

ZygoteSpace::ZygoteSpace(const std::string& name, MemMap* mem_map, size_t objects_allocated)
    : ContinuousMemMapAllocSpace(name, mem_map, mem_map->Begin(), mem_map->End(), mem_map->End(),
                                 kGcRetentionPolicyFullCollect),
      objects_allocated_(objects_allocated) {
}

void ZygoteSpace::Dump(std::ostream& os) const {
  os << GetType()
      << " begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size())
      << ",name=\"" << GetName() << "\"]";
}

mirror::Object* ZygoteSpace::Alloc(Thread*, size_t, size_t*, size_t*, size_t*) {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

size_t ZygoteSpace::AllocationSize(mirror::Object*, size_t*) {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

size_t ZygoteSpace::Free(Thread*, mirror::Object*) {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

size_t ZygoteSpace::FreeList(Thread*, size_t, mirror::Object**) {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

void ZygoteSpace::LogFragmentationAllocFailure(std::ostream&, size_t) {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

void ZygoteSpace::SweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg) {
  SweepCallbackContext* context = static_cast<SweepCallbackContext*>(arg);
  DCHECK(context->space->IsZygoteSpace());
  ZygoteSpace* zygote_space = context->space->AsZygoteSpace();
  Locks::heap_bitmap_lock_->AssertExclusiveHeld(context->self);
  accounting::CardTable* card_table = Runtime::Current()->GetHeap()->GetCardTable();
  // If the bitmaps aren't swapped we need to clear the bits since the GC isn't going to re-swap
  // the bitmaps as an optimization.
  if (!context->swap_bitmaps) {
    accounting::ContinuousSpaceBitmap* bitmap = zygote_space->GetLiveBitmap();
    for (size_t i = 0; i < num_ptrs; ++i) {
      bitmap->Clear(ptrs[i]);
    }
  }
  // We don't free any actual memory to avoid dirtying the shared zygote pages.
  for (size_t i = 0; i < num_ptrs; ++i) {
    // Need to mark the card since this will update the mod-union table next GC cycle.
    card_table->MarkCard(ptrs[i]);
  }
  zygote_space->objects_allocated_.FetchAndSubSequentiallyConsistent(num_ptrs);
}

}  // namespace space
}  // namespace gc
}  // namespace art
