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

#include "mark_compact.h"

#include "base/logging.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/reference_processor.h"
#include "gc/space/bump_pointer_space-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "stack.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {
namespace gc {
namespace collector {

void MarkCompact::BindBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect ||
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect) {
      immune_spaces_.AddSpace(space);
    }
  }
}

MarkCompact::MarkCompact(Heap* heap, const std::string& name_prefix)
    : GarbageCollector(heap, name_prefix + (name_prefix.empty() ? "" : " ") + "mark compact"),
      space_(nullptr),
      collector_name_(name_),
      updating_references_(false) {}

void MarkCompact::RunPhases() {
  Thread* self = Thread::Current();
  InitializePhase();
  CHECK(!Locks::mutator_lock_->IsExclusiveHeld(self));
  {
    ScopedPause pause(this);
    GetHeap()->PreGcVerificationPaused(this);
    GetHeap()->PrePauseRosAllocVerification(this);
    MarkingPhase();
    ReclaimPhase();
  }
  GetHeap()->PostGcVerification(this);
  FinishPhase();
}

void MarkCompact::ForwardObject(mirror::Object* obj) {
  const size_t alloc_size = RoundUp(obj->SizeOf(), space::BumpPointerSpace::kAlignment);
  LockWord lock_word = obj->GetLockWord(false);
  // If we have a non empty lock word, store it and restore it later.
  if (!LockWord::IsDefault(lock_word)) {
    // Set the bit in the bitmap so that we know to restore it later.
    objects_with_lockword_->Set(obj);
    lock_words_to_restore_.push_back(lock_word);
  }
  obj->SetLockWord(LockWord::FromForwardingAddress(reinterpret_cast<size_t>(bump_pointer_)),
                   false);
  bump_pointer_ += alloc_size;
  ++live_objects_in_space_;
}


void MarkCompact::CalculateObjectForwardingAddresses() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // The bump pointer in the space where the next forwarding address will be.
  bump_pointer_ = reinterpret_cast<uint8_t*>(space_->Begin());
  // Visit all the marked objects in the bitmap.
  objects_before_forwarding_->VisitMarkedRange(reinterpret_cast<uintptr_t>(space_->Begin()),
                                               reinterpret_cast<uintptr_t>(space_->End()),
                                               [this](mirror::Object* obj)
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    DCHECK_ALIGNED(obj, space::BumpPointerSpace::kAlignment);
    DCHECK(IsMarked(obj) != nullptr);
    ForwardObject(obj);
  });
}

void MarkCompact::InitializePhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  mark_stack_ = heap_->GetMarkStack();
  DCHECK(mark_stack_ != nullptr);
  immune_spaces_.Reset();
  CHECK(space_->CanMoveObjects()) << "Attempting compact non-movable space from " << *space_;
  // TODO: I don't think we should need heap bitmap lock to Get the mark bitmap.
  ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  mark_bitmap_ = heap_->GetMarkBitmap();
  live_objects_in_space_ = 0;
}

void MarkCompact::ProcessReferences(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  heap_->GetReferenceProcessor()->ProcessReferences(
      false, GetTimings(), GetCurrentIteration()->GetClearSoftReferences(), this);
}

inline mirror::Object* MarkCompact::MarkObject(mirror::Object* obj) {
  if (obj == nullptr) {
    return nullptr;
  }
  if (kUseBakerOrBrooksReadBarrier) {
    // Verify all the objects have the correct forward pointer installed.
    obj->AssertReadBarrierPointer();
  }
  if (!immune_spaces_.IsInImmuneRegion(obj)) {
    if (objects_before_forwarding_->HasAddress(obj)) {
      if (!objects_before_forwarding_->Set(obj)) {
        MarkStackPush(obj);  // This object was not previously marked.
      }
    } else {
      DCHECK(!space_->HasAddress(obj));
      auto slow_path = [this](const mirror::Object* ref)
          SHARED_REQUIRES(Locks::mutator_lock_) {
        // Marking a large object, make sure its aligned as a sanity check.
        if (!IsAligned<kPageSize>(ref)) {
          Runtime::Current()->GetHeap()->DumpSpaces(LOG(ERROR));
          LOG(FATAL) << ref;
        }
      };
      if (!mark_bitmap_->Set(obj, slow_path)) {
        // This object was not previously marked.
        MarkStackPush(obj);
      }
    }
  }
  return obj;
}

void MarkCompact::MarkingPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Thread* self = Thread::Current();
  // Bitmap which describes which objects we have to move.
  objects_before_forwarding_.reset(accounting::ContinuousSpaceBitmap::Create(
      "objects before forwarding", space_->Begin(), space_->Size()));
  // Bitmap which describes which lock words we need to restore.
  objects_with_lockword_.reset(accounting::ContinuousSpaceBitmap::Create(
      "objects with lock words", space_->Begin(), space_->Size()));
  CHECK(Locks::mutator_lock_->IsExclusiveHeld(self));
  // Assume the cleared space is already empty.
  BindBitmaps();
  t.NewTiming("ProcessCards");
  // Process dirty cards and add dirty cards to mod-union tables.
  heap_->ProcessCards(GetTimings(), false, false, true);
  // Clear the whole card table since we cannot get any additional dirty cards during the
  // paused GC. This saves memory but only works for pause the world collectors.
  t.NewTiming("ClearCardTable");
  heap_->GetCardTable()->ClearCardTable();
  // Need to do this before the checkpoint since we don't want any threads to add references to
  // the live stack during the recursive mark.
  if (kUseThreadLocalAllocationStack) {
    t.NewTiming("RevokeAllThreadLocalAllocationStacks");
    heap_->RevokeAllThreadLocalAllocationStacks(self);
  }
  t.NewTiming("SwapStacks");
  heap_->SwapStacks();
  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    MarkRoots();
    // Mark roots of immune spaces.
    UpdateAndMarkModUnion();
    // Recursively mark remaining objects.
    MarkReachableObjects();
  }
  ProcessReferences(self);
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    SweepSystemWeaks();
  }
  Runtime::Current()->GetClassLinker()->CleanupClassLoaders();
  // Revoke buffers before measuring how many objects were moved since the TLABs need to be revoked
  // before they are properly counted.
  RevokeAllThreadLocalBuffers();
  // Disabled due to an issue where we have objects in the bump pointer space which reference dead
  // objects.
  // heap_->PreSweepingGcVerification(this);
}

void MarkCompact::UpdateAndMarkModUnion() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  for (auto& space : heap_->GetContinuousSpaces()) {
    // If the space is immune then we need to mark the references to other spaces.
    if (immune_spaces_.ContainsSpace(space)) {
      accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
      if (table != nullptr) {
        // TODO: Improve naming.
        TimingLogger::ScopedTiming t2(
            space->IsZygoteSpace() ? "UpdateAndMarkZygoteModUnionTable" :
                                     "UpdateAndMarkImageModUnionTable", GetTimings());
        table->UpdateAndMarkReferences(this);
      }
    }
  }
}

void MarkCompact::MarkReachableObjects() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  accounting::ObjectStack* live_stack = heap_->GetLiveStack();
  {
    TimingLogger::ScopedTiming t2("MarkAllocStackAsLive", GetTimings());
    heap_->MarkAllocStackAsLive(live_stack);
  }
  live_stack->Reset();
  // Recursively process the mark stack.
  ProcessMarkStack();
}

void MarkCompact::ReclaimPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // Reclaim unmarked objects.
  Sweep(false);
  // Swap the live and mark bitmaps for each space which we modified space. This is an
  // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
  // bitmaps.
  SwapBitmaps();
  GetHeap()->UnBindBitmaps();  // Unbind the live and mark bitmaps.
  Compact();
}

void MarkCompact::ResizeMarkStack(size_t new_size) {
  std::vector<StackReference<mirror::Object>> temp(mark_stack_->Begin(), mark_stack_->End());
  CHECK_LE(mark_stack_->Size(), new_size);
  mark_stack_->Resize(new_size);
  for (auto& obj : temp) {
    mark_stack_->PushBack(obj.AsMirrorPtr());
  }
}

inline void MarkCompact::MarkStackPush(mirror::Object* obj) {
  if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
    ResizeMarkStack(mark_stack_->Capacity() * 2);
  }
  // The object must be pushed on to the mark stack.
  mark_stack_->PushBack(obj);
}

void MarkCompact::MarkHeapReference(mirror::HeapReference<mirror::Object>* obj_ptr) {
  if (updating_references_) {
    UpdateHeapReference(obj_ptr);
  } else {
    MarkObject(obj_ptr->AsMirrorPtr());
  }
}

void MarkCompact::VisitRoots(
    mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    MarkObject(*roots[i]);
  }
}

void MarkCompact::VisitRoots(
    mirror::CompressedReference<mirror::Object>** roots, size_t count,
    const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    MarkObject(roots[i]->AsMirrorPtr());
  }
}

class MarkCompact::UpdateRootVisitor : public RootVisitor {
 public:
  explicit UpdateRootVisitor(MarkCompact* collector) : collector_(collector) {}

  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE REQUIRES(Locks::mutator_lock_)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_) {
    for (size_t i = 0; i < count; ++i) {
      mirror::Object* obj = *roots[i];
      mirror::Object* new_obj = collector_->GetMarkedForwardAddress(obj);
      if (obj != new_obj) {
        *roots[i] = new_obj;
        DCHECK(new_obj != nullptr);
      }
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE REQUIRES(Locks::mutator_lock_)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_) {
    for (size_t i = 0; i < count; ++i) {
      mirror::Object* obj = roots[i]->AsMirrorPtr();
      mirror::Object* new_obj = collector_->GetMarkedForwardAddress(obj);
      if (obj != new_obj) {
        roots[i]->Assign(new_obj);
        DCHECK(new_obj != nullptr);
      }
    }
  }

 private:
  MarkCompact* const collector_;
};

class MarkCompact::UpdateObjectReferencesVisitor {
 public:
  explicit UpdateObjectReferencesVisitor(MarkCompact* collector) : collector_(collector) {}

  void operator()(mirror::Object* obj) const SHARED_REQUIRES(Locks::heap_bitmap_lock_)
          REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    collector_->UpdateObjectReferences(obj);
  }

 private:
  MarkCompact* const collector_;
};

void MarkCompact::UpdateReferences() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  updating_references_ = true;
  Runtime* runtime = Runtime::Current();
  // Update roots.
  UpdateRootVisitor update_root_visitor(this);
  runtime->VisitRoots(&update_root_visitor);
  // Update object references in mod union tables and spaces.
  for (const auto& space : heap_->GetContinuousSpaces()) {
    // If the space is immune then we need to mark the references to other spaces.
    accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
    if (table != nullptr) {
      // TODO: Improve naming.
      TimingLogger::ScopedTiming t2(
          space->IsZygoteSpace() ? "UpdateZygoteModUnionTableReferences" :
                                   "UpdateImageModUnionTableReferences",
                                   GetTimings());
      table->UpdateAndMarkReferences(this);
    } else {
      // No mod union table, so we need to scan the space using bitmap visit.
      // Scan the space using bitmap visit.
      accounting::ContinuousSpaceBitmap* bitmap = space->GetLiveBitmap();
      if (bitmap != nullptr) {
        UpdateObjectReferencesVisitor visitor(this);
        bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                 reinterpret_cast<uintptr_t>(space->End()),
                                 visitor);
      }
    }
  }
  CHECK(!kMovingClasses)
      << "Didn't update large object classes since they are assumed to not move.";
  // Update the system weaks, these should already have been swept.
  runtime->SweepSystemWeaks(this);
  // Update the objects in the bump pointer space last, these objects don't have a bitmap.
  UpdateObjectReferencesVisitor visitor(this);
  objects_before_forwarding_->VisitMarkedRange(reinterpret_cast<uintptr_t>(space_->Begin()),
                                               reinterpret_cast<uintptr_t>(space_->End()),
                                               visitor);
  // Update the reference processor cleared list.
  heap_->GetReferenceProcessor()->UpdateRoots(this);
  updating_references_ = false;
}

void MarkCompact::Compact() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  CalculateObjectForwardingAddresses();
  UpdateReferences();
  MoveObjects();
  // Space
  int64_t objects_freed = space_->GetObjectsAllocated() - live_objects_in_space_;
  int64_t bytes_freed = reinterpret_cast<int64_t>(space_->End()) -
      reinterpret_cast<int64_t>(bump_pointer_);
  t.NewTiming("RecordFree");
  space_->RecordFree(objects_freed, bytes_freed);
  RecordFree(ObjectBytePair(objects_freed, bytes_freed));
  space_->SetEnd(bump_pointer_);
  // Need to zero out the memory we freed. TODO: Use madvise for pages.
  memset(bump_pointer_, 0, bytes_freed);
}

// Marks all objects in the root set.
void MarkCompact::MarkRoots() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime::Current()->VisitRoots(this);
}

inline void MarkCompact::UpdateHeapReference(mirror::HeapReference<mirror::Object>* reference) {
  mirror::Object* obj = reference->AsMirrorPtr();
  if (obj != nullptr) {
    mirror::Object* new_obj = GetMarkedForwardAddress(obj);
    if (obj != new_obj) {
      DCHECK(new_obj != nullptr);
      reference->Assign(new_obj);
    }
  }
}

class MarkCompact::UpdateReferenceVisitor {
 public:
  explicit UpdateReferenceVisitor(MarkCompact* collector) : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /*is_static*/) const
      ALWAYS_INLINE REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    collector_->UpdateHeapReference(obj->GetFieldObjectReferenceAddr<kVerifyNone>(offset));
  }

  void operator()(mirror::Class* /*klass*/, mirror::Reference* ref) const
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    collector_->UpdateHeapReference(
        ref->GetFieldObjectReferenceAddr<kVerifyNone>(mirror::Reference::ReferentOffset()));
  }

  // TODO: Remove NO_THREAD_SAFETY_ANALYSIS when clang better understands visitors.
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      NO_THREAD_SAFETY_ANALYSIS {
    root->Assign(collector_->GetMarkedForwardAddress(root->AsMirrorPtr()));
  }

 private:
  MarkCompact* const collector_;
};

void MarkCompact::UpdateObjectReferences(mirror::Object* obj) {
  UpdateReferenceVisitor visitor(this);
  obj->VisitReferences(visitor, visitor);
}

inline mirror::Object* MarkCompact::GetMarkedForwardAddress(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  if (objects_before_forwarding_->HasAddress(obj)) {
    DCHECK(objects_before_forwarding_->Test(obj));
    mirror::Object* ret =
        reinterpret_cast<mirror::Object*>(obj->GetLockWord(false).ForwardingAddress());
    DCHECK(ret != nullptr);
    return ret;
  }
  DCHECK(!space_->HasAddress(obj));
  return obj;
}

mirror::Object* MarkCompact::IsMarked(mirror::Object* object) {
  if (immune_spaces_.IsInImmuneRegion(object)) {
    return object;
  }
  if (updating_references_) {
    return GetMarkedForwardAddress(object);
  }
  if (objects_before_forwarding_->HasAddress(object)) {
    return objects_before_forwarding_->Test(object) ? object : nullptr;
  }
  return mark_bitmap_->Test(object) ? object : nullptr;
}

bool MarkCompact::IsMarkedHeapReference(mirror::HeapReference<mirror::Object>* ref_ptr) {
  // Side effect free since we call this before ever moving objects.
  return IsMarked(ref_ptr->AsMirrorPtr()) != nullptr;
}

void MarkCompact::SweepSystemWeaks() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime::Current()->SweepSystemWeaks(this);
}

bool MarkCompact::ShouldSweepSpace(space::ContinuousSpace* space) const {
  return space != space_ && !immune_spaces_.ContainsSpace(space);
}

void MarkCompact::MoveObject(mirror::Object* obj, size_t len) {
  // Look at the forwarding address stored in the lock word to know where to copy.
  DCHECK(space_->HasAddress(obj)) << obj;
  uintptr_t dest_addr = obj->GetLockWord(false).ForwardingAddress();
  mirror::Object* dest_obj = reinterpret_cast<mirror::Object*>(dest_addr);
  DCHECK(space_->HasAddress(dest_obj)) << dest_obj;
  // Use memmove since there may be overlap.
  memmove(reinterpret_cast<void*>(dest_addr), reinterpret_cast<const void*>(obj), len);
  // Restore the saved lock word if needed.
  LockWord lock_word = LockWord::Default();
  if (UNLIKELY(objects_with_lockword_->Test(obj))) {
    lock_word = lock_words_to_restore_.front();
    lock_words_to_restore_.pop_front();
  }
  dest_obj->SetLockWord(lock_word, false);
}

void MarkCompact::MoveObjects() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Move the objects in the before forwarding bitmap.
  objects_before_forwarding_->VisitMarkedRange(reinterpret_cast<uintptr_t>(space_->Begin()),
                                               reinterpret_cast<uintptr_t>(space_->End()),
                                               [this](mirror::Object* obj)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    MoveObject(obj, obj->SizeOf());
  });
  CHECK(lock_words_to_restore_.empty());
}

void MarkCompact::Sweep(bool swap_bitmaps) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  DCHECK(mark_stack_->IsEmpty());
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (!ShouldSweepSpace(alloc_space)) {
        continue;
      }
      TimingLogger::ScopedTiming t2(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepAllocSpace", GetTimings());
      RecordFree(alloc_space->Sweep(swap_bitmaps));
    }
  }
  SweepLargeObjects(swap_bitmaps);
}

void MarkCompact::SweepLargeObjects(bool swap_bitmaps) {
  space::LargeObjectSpace* los = heap_->GetLargeObjectsSpace();
  if (los != nullptr) {
    TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());\
    RecordFreeLOS(los->Sweep(swap_bitmaps));
  }
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void MarkCompact::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* reference) {
  heap_->GetReferenceProcessor()->DelayReferenceReferent(klass, reference, this);
}

class MarkCompact::MarkObjectVisitor {
 public:
  explicit MarkObjectVisitor(MarkCompact* collector) : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /*is_static*/) const ALWAYS_INLINE
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    // Object was already verified when we scanned it.
    collector_->MarkObject(obj->GetFieldObject<mirror::Object, kVerifyNone>(offset));
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_) {
    collector_->DelayReferenceReferent(klass, ref);
  }

  // TODO: Remove NO_THREAD_SAFETY_ANALYSIS when clang better understands visitors.
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      NO_THREAD_SAFETY_ANALYSIS {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      NO_THREAD_SAFETY_ANALYSIS {
    collector_->MarkObject(root->AsMirrorPtr());
  }

 private:
  MarkCompact* const collector_;
};

// Visit all of the references of an object and update.
void MarkCompact::ScanObject(mirror::Object* obj) {
  MarkObjectVisitor visitor(this);
  obj->VisitReferences(visitor, visitor);
}

// Scan anything that's on the mark stack.
void MarkCompact::ProcessMarkStack() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  while (!mark_stack_->IsEmpty()) {
    mirror::Object* obj = mark_stack_->PopBack();
    DCHECK(obj != nullptr);
    ScanObject(obj);
  }
}

void MarkCompact::SetSpace(space::BumpPointerSpace* space) {
  DCHECK(space != nullptr);
  space_ = space;
}

void MarkCompact::FinishPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  space_ = nullptr;
  CHECK(mark_stack_->IsEmpty());
  mark_stack_->Reset();
  // Clear all of the spaces' mark bitmaps.
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  heap_->ClearMarkedObjects();
  // Release our bitmaps.
  objects_before_forwarding_.reset(nullptr);
  objects_with_lockword_.reset(nullptr);
}

void MarkCompact::RevokeAllThreadLocalBuffers() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  GetHeap()->RevokeAllThreadLocalBuffers();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
