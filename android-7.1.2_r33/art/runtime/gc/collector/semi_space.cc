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

#include "semi_space-inl.h"

#include <climits>
#include <functional>
#include <numeric>
#include <sstream>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/remembered_set.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/reference_processor.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/bump_pointer_space-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "indirect_reference_table.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mark_sweep-inl.h"
#include "monitor.h"
#include "mirror/reference-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "thread-inl.h"
#include "thread_list.h"

using ::art::mirror::Object;

namespace art {
namespace gc {
namespace collector {

static constexpr bool kProtectFromSpace = true;
static constexpr bool kStoreStackTraces = false;
static constexpr size_t kBytesPromotedThreshold = 4 * MB;
static constexpr size_t kLargeObjectBytesAllocatedThreshold = 16 * MB;

void SemiSpace::BindBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect ||
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect) {
      immune_spaces_.AddSpace(space);
    } else if (space->GetLiveBitmap() != nullptr) {
      // TODO: We can probably also add this space to the immune region.
      if (space == to_space_ || collect_from_space_only_) {
        if (collect_from_space_only_) {
          // Bind the bitmaps of the main free list space and the non-moving space we are doing a
          // bump pointer space only collection.
          CHECK(space == GetHeap()->GetPrimaryFreeListSpace() ||
                space == GetHeap()->GetNonMovingSpace());
        }
        CHECK(space->IsContinuousMemMapAllocSpace());
        space->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
      }
    }
  }
  if (collect_from_space_only_) {
    // We won't collect the large object space if a bump pointer space only collection.
    is_large_object_space_immune_ = true;
  }
}

SemiSpace::SemiSpace(Heap* heap, bool generational, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix + (name_prefix.empty() ? "" : " ") + "marksweep + semispace"),
      mark_stack_(nullptr),
      is_large_object_space_immune_(false),
      to_space_(nullptr),
      to_space_live_bitmap_(nullptr),
      from_space_(nullptr),
      mark_bitmap_(nullptr),
      self_(nullptr),
      generational_(generational),
      last_gc_to_space_end_(nullptr),
      bytes_promoted_(0),
      bytes_promoted_since_last_whole_heap_collection_(0),
      large_object_bytes_allocated_at_last_whole_heap_collection_(0),
      collect_from_space_only_(generational),
      promo_dest_space_(nullptr),
      fallback_space_(nullptr),
      bytes_moved_(0U),
      objects_moved_(0U),
      saved_bytes_(0U),
      collector_name_(name_),
      swap_semi_spaces_(true) {
}

void SemiSpace::RunPhases() {
  Thread* self = Thread::Current();
  InitializePhase();
  // Semi-space collector is special since it is sometimes called with the mutators suspended
  // during the zygote creation and collector transitions. If we already exclusively hold the
  // mutator lock, then we can't lock it again since it will cause a deadlock.
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    GetHeap()->PreGcVerificationPaused(this);
    GetHeap()->PrePauseRosAllocVerification(this);
    MarkingPhase();
    ReclaimPhase();
    GetHeap()->PostGcVerificationPaused(this);
  } else {
    Locks::mutator_lock_->AssertNotHeld(self);
    {
      ScopedPause pause(this);
      GetHeap()->PreGcVerificationPaused(this);
      GetHeap()->PrePauseRosAllocVerification(this);
      MarkingPhase();
    }
    {
      ReaderMutexLock mu(self, *Locks::mutator_lock_);
      ReclaimPhase();
    }
    GetHeap()->PostGcVerification(this);
  }
  FinishPhase();
}

void SemiSpace::InitializePhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  mark_stack_ = heap_->GetMarkStack();
  DCHECK(mark_stack_ != nullptr);
  immune_spaces_.Reset();
  is_large_object_space_immune_ = false;
  saved_bytes_ = 0;
  bytes_moved_ = 0;
  objects_moved_ = 0;
  self_ = Thread::Current();
  CHECK(from_space_->CanMoveObjects()) << "Attempting to move from " << *from_space_;
  // Set the initial bitmap.
  to_space_live_bitmap_ = to_space_->GetLiveBitmap();
  {
    // TODO: I don't think we should need heap bitmap lock to Get the mark bitmap.
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    mark_bitmap_ = heap_->GetMarkBitmap();
  }
  if (generational_) {
    promo_dest_space_ = GetHeap()->GetPrimaryFreeListSpace();
  }
  fallback_space_ = GetHeap()->GetNonMovingSpace();
}

void SemiSpace::ProcessReferences(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->GetReferenceProcessor()->ProcessReferences(
      false, GetTimings(), GetCurrentIteration()->GetClearSoftReferences(), this);
}

void SemiSpace::MarkingPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  CHECK(Locks::mutator_lock_->IsExclusiveHeld(self_));
  if (kStoreStackTraces) {
    Locks::mutator_lock_->AssertExclusiveHeld(self_);
    // Store the stack traces into the runtime fault string in case we Get a heap corruption
    // related crash later.
    ThreadState old_state = self_->SetStateUnsafe(kRunnable);
    std::ostringstream oss;
    Runtime* runtime = Runtime::Current();
    runtime->GetThreadList()->DumpForSigQuit(oss);
    runtime->GetThreadList()->DumpNativeStacks(oss);
    runtime->SetFaultMessage(oss.str());
    CHECK_EQ(self_->SetStateUnsafe(old_state), kRunnable);
  }
  // Revoke the thread local buffers since the GC may allocate into a RosAllocSpace and this helps
  // to prevent fragmentation.
  RevokeAllThreadLocalBuffers();
  if (generational_) {
    if (GetCurrentIteration()->GetGcCause() == kGcCauseExplicit ||
        GetCurrentIteration()->GetGcCause() == kGcCauseForNativeAlloc ||
        GetCurrentIteration()->GetClearSoftReferences()) {
      // If an explicit, native allocation-triggered, or last attempt
      // collection, collect the whole heap.
      collect_from_space_only_ = false;
    }
    if (!collect_from_space_only_) {
      VLOG(heap) << "Whole heap collection";
      name_ = collector_name_ + " whole";
    } else {
      VLOG(heap) << "Bump pointer space only collection";
      name_ = collector_name_ + " bps";
    }
  }

  if (!collect_from_space_only_) {
    // If non-generational, always clear soft references.
    // If generational, clear soft references if a whole heap collection.
    GetCurrentIteration()->SetClearSoftReferences(true);
  }
  Locks::mutator_lock_->AssertExclusiveHeld(self_);
  if (generational_) {
    // If last_gc_to_space_end_ is out of the bounds of the from-space
    // (the to-space from last GC), then point it to the beginning of
    // the from-space. For example, the very first GC or the
    // pre-zygote compaction.
    if (!from_space_->HasAddress(reinterpret_cast<mirror::Object*>(last_gc_to_space_end_))) {
      last_gc_to_space_end_ = from_space_->Begin();
    }
    // Reset this before the marking starts below.
    bytes_promoted_ = 0;
  }
  // Assume the cleared space is already empty.
  BindBitmaps();
  // Process dirty cards and add dirty cards to mod-union tables.
  heap_->ProcessCards(GetTimings(), kUseRememberedSet && generational_, false, true);
  // Clear the whole card table since we cannot get any additional dirty cards during the
  // paused GC. This saves memory but only works for pause the world collectors.
  t.NewTiming("ClearCardTable");
  heap_->GetCardTable()->ClearCardTable();
  // Need to do this before the checkpoint since we don't want any threads to add references to
  // the live stack during the recursive mark.
  if (kUseThreadLocalAllocationStack) {
    TimingLogger::ScopedTiming t2("RevokeAllThreadLocalAllocationStacks", GetTimings());
    heap_->RevokeAllThreadLocalAllocationStacks(self_);
  }
  heap_->SwapStacks();
  {
    WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
    MarkRoots();
    // Recursively mark remaining objects.
    MarkReachableObjects();
  }
  ProcessReferences(self_);
  {
    ReaderMutexLock mu(self_, *Locks::heap_bitmap_lock_);
    SweepSystemWeaks();
  }
  Runtime::Current()->GetClassLinker()->CleanupClassLoaders();
  // Revoke buffers before measuring how many objects were moved since the TLABs need to be revoked
  // before they are properly counted.
  RevokeAllThreadLocalBuffers();
  GetHeap()->RecordFreeRevoke();  // this is for the non-moving rosalloc space used by GSS.
  // Record freed memory.
  const int64_t from_bytes = from_space_->GetBytesAllocated();
  const int64_t to_bytes = bytes_moved_;
  const uint64_t from_objects = from_space_->GetObjectsAllocated();
  const uint64_t to_objects = objects_moved_;
  CHECK_LE(to_objects, from_objects);
  // Note: Freed bytes can be negative if we copy form a compacted space to a free-list backed
  // space.
  RecordFree(ObjectBytePair(from_objects - to_objects, from_bytes - to_bytes));
  // Clear and protect the from space.
  from_space_->Clear();
  if (kProtectFromSpace && !from_space_->IsRosAllocSpace()) {
    // Protect with PROT_NONE.
    VLOG(heap) << "Protecting from_space_ : " << *from_space_;
    from_space_->GetMemMap()->Protect(PROT_NONE);
  } else {
    // If RosAllocSpace, we'll leave it as PROT_READ here so the
    // rosaloc verification can read the metadata magic number and
    // protect it with PROT_NONE later in FinishPhase().
    VLOG(heap) << "Protecting from_space_ with PROT_READ : " << *from_space_;
    from_space_->GetMemMap()->Protect(PROT_READ);
  }
  heap_->PreSweepingGcVerification(this);
  if (swap_semi_spaces_) {
    heap_->SwapSemiSpaces();
  }
}

// Used to verify that there's no references to the from-space.
class SemiSpace::VerifyNoFromSpaceReferencesVisitor {
 public:
  explicit VerifyNoFromSpaceReferencesVisitor(space::ContinuousMemMapAllocSpace* from_space)
      : from_space_(from_space) {}

  void operator()(Object* obj, MemberOffset offset, bool /* is_static */) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    if (from_space_->HasAddress(ref)) {
      Runtime::Current()->GetHeap()->DumpObject(LOG(INFO), obj);
      LOG(FATAL) << ref << " found in from space";
    }
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
    if (kIsDebugBuild) {
      Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    CHECK(!from_space_->HasAddress(root->AsMirrorPtr()));
  }

 private:
  space::ContinuousMemMapAllocSpace* const from_space_;
};

void SemiSpace::VerifyNoFromSpaceReferences(Object* obj) {
  DCHECK(!from_space_->HasAddress(obj)) << "Scanning object " << obj << " in from space";
  VerifyNoFromSpaceReferencesVisitor visitor(from_space_);
  obj->VisitReferences(visitor, VoidFunctor());
}

void SemiSpace::MarkReachableObjects() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  {
    TimingLogger::ScopedTiming t2("MarkStackAsLive", GetTimings());
    accounting::ObjectStack* live_stack = heap_->GetLiveStack();
    heap_->MarkAllocStackAsLive(live_stack);
    live_stack->Reset();
  }
  for (auto& space : heap_->GetContinuousSpaces()) {
    // If the space is immune then we need to mark the references to other spaces.
    accounting::ModUnionTable* table = heap_->FindModUnionTableFromSpace(space);
    if (table != nullptr) {
      // TODO: Improve naming.
      TimingLogger::ScopedTiming t2(
          space->IsZygoteSpace() ? "UpdateAndMarkZygoteModUnionTable" :
                                   "UpdateAndMarkImageModUnionTable",
                                   GetTimings());
      table->UpdateAndMarkReferences(this);
      DCHECK(GetHeap()->FindRememberedSetFromSpace(space) == nullptr);
    } else if ((space->IsImageSpace() || collect_from_space_only_) &&
               space->GetLiveBitmap() != nullptr) {
      // If the space has no mod union table (the non-moving space, app image spaces, main spaces
      // when the bump pointer space only collection is enabled,) then we need to scan its live
      // bitmap or dirty cards as roots (including the objects on the live stack which have just
      // marked in the live bitmap above in MarkAllocStackAsLive().)
      accounting::RememberedSet* rem_set = GetHeap()->FindRememberedSetFromSpace(space);
      if (!space->IsImageSpace()) {
        DCHECK(space == heap_->GetNonMovingSpace() || space == heap_->GetPrimaryFreeListSpace())
            << "Space " << space->GetName() << " "
            << "generational_=" << generational_ << " "
            << "collect_from_space_only_=" << collect_from_space_only_;
        // App images currently do not have remembered sets.
        DCHECK_EQ(kUseRememberedSet, rem_set != nullptr);
      } else {
        DCHECK(rem_set == nullptr);
      }
      if (rem_set != nullptr) {
        TimingLogger::ScopedTiming t2("UpdateAndMarkRememberedSet", GetTimings());
        rem_set->UpdateAndMarkReferences(from_space_, this);
      } else {
        TimingLogger::ScopedTiming t2("VisitLiveBits", GetTimings());
        accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
        live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                      reinterpret_cast<uintptr_t>(space->End()),
                                      [this](mirror::Object* obj)
           REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
          ScanObject(obj);
        });
      }
      if (kIsDebugBuild) {
        // Verify that there are no from-space references that
        // remain in the space, that is, the remembered set (and the
        // card table) didn't miss any from-space references in the
        // space.
        accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
        live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                      reinterpret_cast<uintptr_t>(space->End()),
                                      [this](Object* obj)
            SHARED_REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
          DCHECK(obj != nullptr);
          VerifyNoFromSpaceReferences(obj);
        });
      }
    }
  }

  CHECK_EQ(is_large_object_space_immune_, collect_from_space_only_);
  space::LargeObjectSpace* los = GetHeap()->GetLargeObjectsSpace();
  if (is_large_object_space_immune_ && los != nullptr) {
    TimingLogger::ScopedTiming t2("VisitLargeObjects", GetTimings());
    DCHECK(collect_from_space_only_);
    // Delay copying the live set to the marked set until here from
    // BindBitmaps() as the large objects on the allocation stack may
    // be newly added to the live set above in MarkAllocStackAsLive().
    los->CopyLiveToMarked();

    // When the large object space is immune, we need to scan the
    // large object space as roots as they contain references to their
    // classes (primitive array classes) that could move though they
    // don't contain any other references.
    accounting::LargeObjectBitmap* large_live_bitmap = los->GetLiveBitmap();
    large_live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(los->Begin()),
                                        reinterpret_cast<uintptr_t>(los->End()),
                                        [this](mirror::Object* obj)
        REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
      ScanObject(obj);
    });
  }
  // Recursively process the mark stack.
  ProcessMarkStack();
}

void SemiSpace::ReclaimPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
  // Reclaim unmarked objects.
  Sweep(false);
  // Swap the live and mark bitmaps for each space which we modified space. This is an
  // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
  // bitmaps.
  SwapBitmaps();
  // Unbind the live and mark bitmaps.
  GetHeap()->UnBindBitmaps();
  if (saved_bytes_ > 0) {
    VLOG(heap) << "Avoided dirtying " << PrettySize(saved_bytes_);
  }
  if (generational_) {
    // Record the end (top) of the to space so we can distinguish
    // between objects that were allocated since the last GC and the
    // older objects.
    last_gc_to_space_end_ = to_space_->End();
  }
}

void SemiSpace::ResizeMarkStack(size_t new_size) {
  std::vector<StackReference<Object>> temp(mark_stack_->Begin(), mark_stack_->End());
  CHECK_LE(mark_stack_->Size(), new_size);
  mark_stack_->Resize(new_size);
  for (auto& obj : temp) {
    mark_stack_->PushBack(obj.AsMirrorPtr());
  }
}

inline void SemiSpace::MarkStackPush(Object* obj) {
  if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
    ResizeMarkStack(mark_stack_->Capacity() * 2);
  }
  // The object must be pushed on to the mark stack.
  mark_stack_->PushBack(obj);
}

static inline size_t CopyAvoidingDirtyingPages(void* dest, const void* src, size_t size) {
  if (LIKELY(size <= static_cast<size_t>(kPageSize))) {
    // We will dirty the current page and somewhere in the middle of the next page. This means
    // that the next object copied will also dirty that page.
    // TODO: Worth considering the last object copied? We may end up dirtying one page which is
    // not necessary per GC.
    memcpy(dest, src, size);
    return 0;
  }
  size_t saved_bytes = 0;
  uint8_t* byte_dest = reinterpret_cast<uint8_t*>(dest);
  if (kIsDebugBuild) {
    for (size_t i = 0; i < size; ++i) {
      CHECK_EQ(byte_dest[i], 0U);
    }
  }
  // Process the start of the page. The page must already be dirty, don't bother with checking.
  const uint8_t* byte_src = reinterpret_cast<const uint8_t*>(src);
  const uint8_t* limit = byte_src + size;
  size_t page_remain = AlignUp(byte_dest, kPageSize) - byte_dest;
  // Copy the bytes until the start of the next page.
  memcpy(dest, src, page_remain);
  byte_src += page_remain;
  byte_dest += page_remain;
  DCHECK_ALIGNED(reinterpret_cast<uintptr_t>(byte_dest), kPageSize);
  DCHECK_ALIGNED(reinterpret_cast<uintptr_t>(byte_dest), sizeof(uintptr_t));
  DCHECK_ALIGNED(reinterpret_cast<uintptr_t>(byte_src), sizeof(uintptr_t));
  while (byte_src + kPageSize < limit) {
    bool all_zero = true;
    uintptr_t* word_dest = reinterpret_cast<uintptr_t*>(byte_dest);
    const uintptr_t* word_src = reinterpret_cast<const uintptr_t*>(byte_src);
    for (size_t i = 0; i < kPageSize / sizeof(*word_src); ++i) {
      // Assumes the destination of the copy is all zeros.
      if (word_src[i] != 0) {
        all_zero = false;
        word_dest[i] = word_src[i];
      }
    }
    if (all_zero) {
      // Avoided copying into the page since it was all zeros.
      saved_bytes += kPageSize;
    }
    byte_src += kPageSize;
    byte_dest += kPageSize;
  }
  // Handle the part of the page at the end.
  memcpy(byte_dest, byte_src, limit - byte_src);
  return saved_bytes;
}

mirror::Object* SemiSpace::MarkNonForwardedObject(mirror::Object* obj) {
  const size_t object_size = obj->SizeOf();
  size_t bytes_allocated, dummy;
  mirror::Object* forward_address = nullptr;
  if (generational_ && reinterpret_cast<uint8_t*>(obj) < last_gc_to_space_end_) {
    // If it's allocated before the last GC (older), move
    // (pseudo-promote) it to the main free list space (as sort
    // of an old generation.)
    forward_address = promo_dest_space_->AllocThreadUnsafe(self_, object_size, &bytes_allocated,
                                                           nullptr, &dummy);
    if (UNLIKELY(forward_address == nullptr)) {
      // If out of space, fall back to the to-space.
      forward_address = to_space_->AllocThreadUnsafe(self_, object_size, &bytes_allocated, nullptr,
                                                     &dummy);
      // No logic for marking the bitmap, so it must be null.
      DCHECK(to_space_live_bitmap_ == nullptr);
    } else {
      bytes_promoted_ += bytes_allocated;
      // Dirty the card at the destionation as it may contain
      // references (including the class pointer) to the bump pointer
      // space.
      GetHeap()->WriteBarrierEveryFieldOf(forward_address);
      // Handle the bitmaps marking.
      accounting::ContinuousSpaceBitmap* live_bitmap = promo_dest_space_->GetLiveBitmap();
      DCHECK(live_bitmap != nullptr);
      accounting::ContinuousSpaceBitmap* mark_bitmap = promo_dest_space_->GetMarkBitmap();
      DCHECK(mark_bitmap != nullptr);
      DCHECK(!live_bitmap->Test(forward_address));
      if (collect_from_space_only_) {
        // If collecting the bump pointer spaces only, live_bitmap == mark_bitmap.
        DCHECK_EQ(live_bitmap, mark_bitmap);

        // If a bump pointer space only collection, delay the live
        // bitmap marking of the promoted object until it's popped off
        // the mark stack (ProcessMarkStack()). The rationale: we may
        // be in the middle of scanning the objects in the promo
        // destination space for
        // non-moving-space-to-bump-pointer-space references by
        // iterating over the marked bits of the live bitmap
        // (MarkReachableObjects()). If we don't delay it (and instead
        // mark the promoted object here), the above promo destination
        // space scan could encounter the just-promoted object and
        // forward the references in the promoted object's fields even
        // through it is pushed onto the mark stack. If this happens,
        // the promoted object would be in an inconsistent state, that
        // is, it's on the mark stack (gray) but its fields are
        // already forwarded (black), which would cause a
        // DCHECK(!to_space_->HasAddress(obj)) failure below.
      } else {
        // Mark forward_address on the live bit map.
        live_bitmap->Set(forward_address);
        // Mark forward_address on the mark bit map.
        DCHECK(!mark_bitmap->Test(forward_address));
        mark_bitmap->Set(forward_address);
      }
    }
  } else {
    // If it's allocated after the last GC (younger), copy it to the to-space.
    forward_address = to_space_->AllocThreadUnsafe(self_, object_size, &bytes_allocated, nullptr,
                                                   &dummy);
    if (forward_address != nullptr && to_space_live_bitmap_ != nullptr) {
      to_space_live_bitmap_->Set(forward_address);
    }
  }
  // If it's still null, attempt to use the fallback space.
  if (UNLIKELY(forward_address == nullptr)) {
    forward_address = fallback_space_->AllocThreadUnsafe(self_, object_size, &bytes_allocated,
                                                         nullptr, &dummy);
    CHECK(forward_address != nullptr) << "Out of memory in the to-space and fallback space.";
    accounting::ContinuousSpaceBitmap* bitmap = fallback_space_->GetLiveBitmap();
    if (bitmap != nullptr) {
      bitmap->Set(forward_address);
    }
  }
  ++objects_moved_;
  bytes_moved_ += bytes_allocated;
  // Copy over the object and add it to the mark stack since we still need to update its
  // references.
  saved_bytes_ +=
      CopyAvoidingDirtyingPages(reinterpret_cast<void*>(forward_address), obj, object_size);
  if (kUseBakerOrBrooksReadBarrier) {
    obj->AssertReadBarrierPointer();
    if (kUseBrooksReadBarrier) {
      DCHECK_EQ(forward_address->GetReadBarrierPointer(), obj);
      forward_address->SetReadBarrierPointer(forward_address);
    }
    forward_address->AssertReadBarrierPointer();
  }
  DCHECK(to_space_->HasAddress(forward_address) ||
         fallback_space_->HasAddress(forward_address) ||
         (generational_ && promo_dest_space_->HasAddress(forward_address)))
      << forward_address << "\n" << GetHeap()->DumpSpaces();
  return forward_address;
}

mirror::Object* SemiSpace::MarkObject(mirror::Object* root) {
  auto ref = StackReference<mirror::Object>::FromMirrorPtr(root);
  MarkObjectIfNotInToSpace(&ref);
  return ref.AsMirrorPtr();
}

void SemiSpace::MarkHeapReference(mirror::HeapReference<mirror::Object>* obj_ptr) {
  MarkObject(obj_ptr);
}

void SemiSpace::VisitRoots(mirror::Object*** roots, size_t count,
                           const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    auto* root = roots[i];
    auto ref = StackReference<mirror::Object>::FromMirrorPtr(*root);
    // The root can be in the to-space since we may visit the declaring class of an ArtMethod
    // multiple times if it is on the call stack.
    MarkObjectIfNotInToSpace(&ref);
    if (*root != ref.AsMirrorPtr()) {
      *root = ref.AsMirrorPtr();
    }
  }
}

void SemiSpace::VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count,
                           const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    MarkObjectIfNotInToSpace(roots[i]);
  }
}

// Marks all objects in the root set.
void SemiSpace::MarkRoots() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime::Current()->VisitRoots(this);
}

void SemiSpace::SweepSystemWeaks() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime::Current()->SweepSystemWeaks(this);
}

bool SemiSpace::ShouldSweepSpace(space::ContinuousSpace* space) const {
  return space != from_space_ && space != to_space_;
}

void SemiSpace::Sweep(bool swap_bitmaps) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  DCHECK(mark_stack_->IsEmpty());
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (!ShouldSweepSpace(alloc_space)) {
        continue;
      }
      TimingLogger::ScopedTiming split(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepAllocSpace", GetTimings());
      RecordFree(alloc_space->Sweep(swap_bitmaps));
    }
  }
  if (!is_large_object_space_immune_) {
    SweepLargeObjects(swap_bitmaps);
  }
}

void SemiSpace::SweepLargeObjects(bool swap_bitmaps) {
  DCHECK(!is_large_object_space_immune_);
  space::LargeObjectSpace* los = heap_->GetLargeObjectsSpace();
  if (los != nullptr) {
    TimingLogger::ScopedTiming split("SweepLargeObjects", GetTimings());
    RecordFreeLOS(los->Sweep(swap_bitmaps));
  }
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void SemiSpace::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* reference) {
  heap_->GetReferenceProcessor()->DelayReferenceReferent(klass, reference, this);
}

class SemiSpace::MarkObjectVisitor {
 public:
  explicit MarkObjectVisitor(SemiSpace* collector) : collector_(collector) {}

  void operator()(Object* obj, MemberOffset offset, bool /* is_static */) const ALWAYS_INLINE
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    // Object was already verified when we scanned it.
    collector_->MarkObject(obj->GetFieldObjectReferenceAddr<kVerifyNone>(offset));
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
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
    if (kIsDebugBuild) {
      Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    // We may visit the same root multiple times, so avoid marking things in the to-space since
    // this is not handled by the GC.
    collector_->MarkObjectIfNotInToSpace(root);
  }

 private:
  SemiSpace* const collector_;
};

// Visit all of the references of an object and update.
void SemiSpace::ScanObject(Object* obj) {
  DCHECK(!from_space_->HasAddress(obj)) << "Scanning object " << obj << " in from space";
  MarkObjectVisitor visitor(this);
  obj->VisitReferences(visitor, visitor);
}

// Scan anything that's on the mark stack.
void SemiSpace::ProcessMarkStack() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  accounting::ContinuousSpaceBitmap* live_bitmap = nullptr;
  if (collect_from_space_only_) {
    // If a bump pointer space only collection (and the promotion is
    // enabled,) we delay the live-bitmap marking of promoted objects
    // from MarkObject() until this function.
    live_bitmap = promo_dest_space_->GetLiveBitmap();
    DCHECK(live_bitmap != nullptr);
    accounting::ContinuousSpaceBitmap* mark_bitmap = promo_dest_space_->GetMarkBitmap();
    DCHECK(mark_bitmap != nullptr);
    DCHECK_EQ(live_bitmap, mark_bitmap);
  }
  while (!mark_stack_->IsEmpty()) {
    Object* obj = mark_stack_->PopBack();
    if (collect_from_space_only_ && promo_dest_space_->HasAddress(obj)) {
      // obj has just been promoted. Mark the live bitmap for it,
      // which is delayed from MarkObject().
      DCHECK(!live_bitmap->Test(obj));
      live_bitmap->Set(obj);
    }
    ScanObject(obj);
  }
}

mirror::Object* SemiSpace::IsMarked(mirror::Object* obj) {
  // All immune objects are assumed marked.
  if (from_space_->HasAddress(obj)) {
    // Returns either the forwarding address or null.
    return GetForwardingAddressInFromSpace(obj);
  } else if (collect_from_space_only_ ||
             immune_spaces_.IsInImmuneRegion(obj) ||
             to_space_->HasAddress(obj)) {
    return obj;  // Already forwarded, must be marked.
  }
  return mark_bitmap_->Test(obj) ? obj : nullptr;
}

bool SemiSpace::IsMarkedHeapReference(mirror::HeapReference<mirror::Object>* object) {
  mirror::Object* obj = object->AsMirrorPtr();
  mirror::Object* new_obj = IsMarked(obj);
  if (new_obj == nullptr) {
    return false;
  }
  if (new_obj != obj) {
    // Write barrier is not necessary since it still points to the same object, just at a different
    // address.
    object->Assign(new_obj);
  }
  return true;
}

void SemiSpace::SetToSpace(space::ContinuousMemMapAllocSpace* to_space) {
  DCHECK(to_space != nullptr);
  to_space_ = to_space;
}

void SemiSpace::SetFromSpace(space::ContinuousMemMapAllocSpace* from_space) {
  DCHECK(from_space != nullptr);
  from_space_ = from_space;
}

void SemiSpace::FinishPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  if (kProtectFromSpace && from_space_->IsRosAllocSpace()) {
    VLOG(heap) << "Protecting from_space_ with PROT_NONE : " << *from_space_;
    from_space_->GetMemMap()->Protect(PROT_NONE);
  }
  // Null the "to" and "from" spaces since compacting from one to the other isn't valid until
  // further action is done by the heap.
  to_space_ = nullptr;
  from_space_ = nullptr;
  CHECK(mark_stack_->IsEmpty());
  mark_stack_->Reset();
  space::LargeObjectSpace* los = GetHeap()->GetLargeObjectsSpace();
  if (generational_) {
    // Decide whether to do a whole heap collection or a bump pointer
    // only space collection at the next collection by updating
    // collect_from_space_only_.
    if (collect_from_space_only_) {
      // Disable collect_from_space_only_ if the bytes promoted since the
      // last whole heap collection or the large object bytes
      // allocated exceeds a threshold.
      bytes_promoted_since_last_whole_heap_collection_ += bytes_promoted_;
      bool bytes_promoted_threshold_exceeded =
          bytes_promoted_since_last_whole_heap_collection_ >= kBytesPromotedThreshold;
      uint64_t current_los_bytes_allocated = los != nullptr ? los->GetBytesAllocated() : 0U;
      uint64_t last_los_bytes_allocated =
          large_object_bytes_allocated_at_last_whole_heap_collection_;
      bool large_object_bytes_threshold_exceeded =
          current_los_bytes_allocated >=
          last_los_bytes_allocated + kLargeObjectBytesAllocatedThreshold;
      if (bytes_promoted_threshold_exceeded || large_object_bytes_threshold_exceeded) {
        collect_from_space_only_ = false;
      }
    } else {
      // Reset the counters.
      bytes_promoted_since_last_whole_heap_collection_ = bytes_promoted_;
      large_object_bytes_allocated_at_last_whole_heap_collection_ =
          los != nullptr ? los->GetBytesAllocated() : 0U;
      collect_from_space_only_ = true;
    }
  }
  // Clear all of the spaces' mark bitmaps.
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  heap_->ClearMarkedObjects();
}

void SemiSpace::RevokeAllThreadLocalBuffers() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  GetHeap()->RevokeAllThreadLocalBuffers();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
