/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "gc/heap.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "sticky_mark_sweep.h"
#include "thread-inl.h"

namespace art {
namespace gc {
namespace collector {

StickyMarkSweep::StickyMarkSweep(Heap* heap, bool is_concurrent, const std::string& name_prefix)
    : PartialMarkSweep(heap, is_concurrent, name_prefix.empty() ? "sticky " : name_prefix) {
  cumulative_timings_.SetName(GetName());
}

void StickyMarkSweep::BindBitmaps() {
  PartialMarkSweep::BindBitmaps();
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // For sticky GC, we want to bind the bitmaps of all spaces as the allocation stack lets us
  // know what was allocated since the last GC. A side-effect of binding the allocation space mark
  // and live bitmap is that marking the objects will place them in the live bitmap.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace() &&
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) {
      DCHECK(space->IsContinuousMemMapAllocSpace());
      space->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
    }
  }
  for (const auto& space : GetHeap()->GetDiscontinuousSpaces()) {
    CHECK(space->IsLargeObjectSpace());
    space->AsLargeObjectSpace()->CopyLiveToMarked();
  }
}

void StickyMarkSweep::MarkReachableObjects() {
  // All reachable objects must be referenced by a root or a dirty card, so we can clear the mark
  // stack here since all objects in the mark stack will Get scanned by the card scanning anyways.
  // TODO: Not put these objects in the mark stack in the first place.
  mark_stack_->Reset();
  RecursiveMarkDirtyObjects(false, accounting::CardTable::kCardDirty - 1);
}

void StickyMarkSweep::MarkConcurrentRoots(VisitRootFlags flags) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Visit all runtime roots and clear dirty flags including class loader. This is done to prevent
  // incorrect class unloading since the GC does not card mark when storing store the class during
  // object allocation. Doing this for each allocation would be slow.
  // Since the card is not dirty, it means the object may not get scanned. This can cause class
  // unloading to occur even though the class and class loader are reachable through the object's
  // class.
  Runtime::Current()->VisitConcurrentRoots(
      this,
      static_cast<VisitRootFlags>(flags | kVisitRootFlagClassLoader));
}

void StickyMarkSweep::Sweep(bool swap_bitmaps ATTRIBUTE_UNUSED) {
  SweepArray(GetHeap()->GetLiveStack(), false);
}

}  // namespace collector
}  // namespace gc
}  // namespace art
