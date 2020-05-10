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

#include "concurrent_copying.h"

#include "art_field-inl.h"
#include "base/stl_util.h"
#include "debugger.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/reference_processor.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "image-inl.h"
#include "intern_table.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art {
namespace gc {
namespace collector {

static constexpr size_t kDefaultGcMarkStackSize = 2 * MB;

ConcurrentCopying::ConcurrentCopying(Heap* heap, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix + (name_prefix.empty() ? "" : " ") +
                       "concurrent copying + mark sweep"),
      region_space_(nullptr), gc_barrier_(new Barrier(0)),
      gc_mark_stack_(accounting::ObjectStack::Create("concurrent copying gc mark stack",
                                                     kDefaultGcMarkStackSize,
                                                     kDefaultGcMarkStackSize)),
      mark_stack_lock_("concurrent copying mark stack lock", kMarkSweepMarkStackLock),
      thread_running_gc_(nullptr),
      is_marking_(false), is_active_(false), is_asserting_to_space_invariant_(false),
      heap_mark_bitmap_(nullptr), live_stack_freeze_size_(0), mark_stack_mode_(kMarkStackModeOff),
      weak_ref_access_enabled_(true),
      skipped_blocks_lock_("concurrent copying bytes blocks lock", kMarkSweepMarkStackLock),
      rb_table_(heap_->GetReadBarrierTable()),
      force_evacuate_all_(false) {
  static_assert(space::RegionSpace::kRegionSize == accounting::ReadBarrierTable::kRegionSize,
                "The region space size and the read barrier table region size must match");
  cc_heap_bitmap_.reset(new accounting::HeapBitmap(heap));
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Cache this so that we won't have to lock heap_bitmap_lock_ in
    // Mark() which could cause a nested lock on heap_bitmap_lock_
    // when GC causes a RB while doing GC or a lock order violation
    // (class_linker_lock_ and heap_bitmap_lock_).
    heap_mark_bitmap_ = heap->GetMarkBitmap();
  }
  {
    MutexLock mu(self, mark_stack_lock_);
    for (size_t i = 0; i < kMarkStackPoolSize; ++i) {
      accounting::AtomicStack<mirror::Object>* mark_stack =
          accounting::AtomicStack<mirror::Object>::Create(
              "thread local mark stack", kMarkStackSize, kMarkStackSize);
      pooled_mark_stacks_.push_back(mark_stack);
    }
  }
}

void ConcurrentCopying::MarkHeapReference(mirror::HeapReference<mirror::Object>* from_ref) {
  // Used for preserving soft references, should be OK to not have a CAS here since there should be
  // no other threads which can trigger read barriers on the same referent during reference
  // processing.
  from_ref->Assign(Mark(from_ref->AsMirrorPtr()));
  DCHECK(!from_ref->IsNull());
}

ConcurrentCopying::~ConcurrentCopying() {
  STLDeleteElements(&pooled_mark_stacks_);
}

void ConcurrentCopying::RunPhases() {
  CHECK(kUseBakerReadBarrier || kUseTableLookupReadBarrier);
  CHECK(!is_active_);
  is_active_ = true;
  Thread* self = Thread::Current();
  thread_running_gc_ = self;
  Locks::mutator_lock_->AssertNotHeld(self);
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    InitializePhase();
  }
  FlipThreadRoots();
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    MarkingPhase();
  }
  // Verify no from space refs. This causes a pause.
  if (kEnableNoFromSpaceRefsVerification || kIsDebugBuild) {
    TimingLogger::ScopedTiming split("(Paused)VerifyNoFromSpaceReferences", GetTimings());
    ScopedPause pause(this);
    CheckEmptyMarkStack();
    if (kVerboseMode) {
      LOG(INFO) << "Verifying no from-space refs";
    }
    VerifyNoFromSpaceReferences();
    if (kVerboseMode) {
      LOG(INFO) << "Done verifying no from-space refs";
    }
    CheckEmptyMarkStack();
  }
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    ReclaimPhase();
  }
  FinishPhase();
  CHECK(is_active_);
  is_active_ = false;
  thread_running_gc_ = nullptr;
}

void ConcurrentCopying::BindBitmaps() {
  Thread* self = Thread::Current();
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : heap_->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect ||
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect) {
      CHECK(space->IsZygoteSpace() || space->IsImageSpace());
      immune_spaces_.AddSpace(space);
      const char* bitmap_name = space->IsImageSpace() ? "cc image space bitmap" :
          "cc zygote space bitmap";
      // TODO: try avoiding using bitmaps for image/zygote to save space.
      accounting::ContinuousSpaceBitmap* bitmap =
          accounting::ContinuousSpaceBitmap::Create(bitmap_name, space->Begin(), space->Capacity());
      cc_heap_bitmap_->AddContinuousSpaceBitmap(bitmap);
      cc_bitmaps_.push_back(bitmap);
    } else if (space == region_space_) {
      accounting::ContinuousSpaceBitmap* bitmap =
          accounting::ContinuousSpaceBitmap::Create("cc region space bitmap",
                                                    space->Begin(), space->Capacity());
      cc_heap_bitmap_->AddContinuousSpaceBitmap(bitmap);
      cc_bitmaps_.push_back(bitmap);
      region_space_bitmap_ = bitmap;
    }
  }
}

void ConcurrentCopying::InitializePhase() {
  TimingLogger::ScopedTiming split("InitializePhase", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "GC InitializePhase";
    LOG(INFO) << "Region-space : " << reinterpret_cast<void*>(region_space_->Begin()) << "-"
              << reinterpret_cast<void*>(region_space_->Limit());
  }
  CheckEmptyMarkStack();
  immune_spaces_.Reset();
  bytes_moved_.StoreRelaxed(0);
  objects_moved_.StoreRelaxed(0);
  if (GetCurrentIteration()->GetGcCause() == kGcCauseExplicit ||
      GetCurrentIteration()->GetGcCause() == kGcCauseForNativeAlloc ||
      GetCurrentIteration()->GetClearSoftReferences()) {
    force_evacuate_all_ = true;
  } else {
    force_evacuate_all_ = false;
  }
  BindBitmaps();
  if (kVerboseMode) {
    LOG(INFO) << "force_evacuate_all=" << force_evacuate_all_;
    LOG(INFO) << "Largest immune region: " << immune_spaces_.GetLargestImmuneRegion().Begin()
              << "-" << immune_spaces_.GetLargestImmuneRegion().End();
    for (space::ContinuousSpace* space : immune_spaces_.GetSpaces()) {
      LOG(INFO) << "Immune space: " << *space;
    }
    LOG(INFO) << "GC end of InitializePhase";
  }
}

// Used to switch the thread roots of a thread from from-space refs to to-space refs.
class ConcurrentCopying::ThreadFlipVisitor : public Closure {
 public:
  ThreadFlipVisitor(ConcurrentCopying* concurrent_copying, bool use_tlab)
      : concurrent_copying_(concurrent_copying), use_tlab_(use_tlab) {
  }

  virtual void Run(Thread* thread) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    thread->SetIsGcMarking(true);
    if (use_tlab_ && thread->HasTlab()) {
      if (ConcurrentCopying::kEnableFromSpaceAccountingCheck) {
        // This must come before the revoke.
        size_t thread_local_objects = thread->GetThreadLocalObjectsAllocated();
        concurrent_copying_->region_space_->RevokeThreadLocalBuffers(thread);
        reinterpret_cast<Atomic<size_t>*>(&concurrent_copying_->from_space_num_objects_at_first_pause_)->
            FetchAndAddSequentiallyConsistent(thread_local_objects);
      } else {
        concurrent_copying_->region_space_->RevokeThreadLocalBuffers(thread);
      }
    }
    if (kUseThreadLocalAllocationStack) {
      thread->RevokeThreadLocalAllocationStack();
    }
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    thread->VisitRoots(concurrent_copying_);
    concurrent_copying_->GetBarrier().Pass(self);
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
  const bool use_tlab_;
};

// Called back from Runtime::FlipThreadRoots() during a pause.
class ConcurrentCopying::FlipCallback : public Closure {
 public:
  explicit FlipCallback(ConcurrentCopying* concurrent_copying)
      : concurrent_copying_(concurrent_copying) {
  }

  virtual void Run(Thread* thread) OVERRIDE REQUIRES(Locks::mutator_lock_) {
    ConcurrentCopying* cc = concurrent_copying_;
    TimingLogger::ScopedTiming split("(Paused)FlipCallback", cc->GetTimings());
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self);
    Locks::mutator_lock_->AssertExclusiveHeld(self);
    cc->region_space_->SetFromSpace(cc->rb_table_, cc->force_evacuate_all_);
    cc->SwapStacks();
    if (ConcurrentCopying::kEnableFromSpaceAccountingCheck) {
      cc->RecordLiveStackFreezeSize(self);
      cc->from_space_num_objects_at_first_pause_ = cc->region_space_->GetObjectsAllocated();
      cc->from_space_num_bytes_at_first_pause_ = cc->region_space_->GetBytesAllocated();
    }
    cc->is_marking_ = true;
    cc->mark_stack_mode_.StoreRelaxed(ConcurrentCopying::kMarkStackModeThreadLocal);
    if (UNLIKELY(Runtime::Current()->IsActiveTransaction())) {
      CHECK(Runtime::Current()->IsAotCompiler());
      TimingLogger::ScopedTiming split2("(Paused)VisitTransactionRoots", cc->GetTimings());
      Runtime::Current()->VisitTransactionRoots(cc);
    }
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
};

// Switch threads that from from-space to to-space refs. Forward/mark the thread roots.
void ConcurrentCopying::FlipThreadRoots() {
  TimingLogger::ScopedTiming split("FlipThreadRoots", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "time=" << region_space_->Time();
    region_space_->DumpNonFreeRegions(LOG(INFO));
  }
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  gc_barrier_->Init(self, 0);
  ThreadFlipVisitor thread_flip_visitor(this, heap_->use_tlab_);
  FlipCallback flip_callback(this);
  heap_->ThreadFlipBegin(self);  // Sync with JNI critical calls.
  size_t barrier_count = Runtime::Current()->FlipThreadRoots(
      &thread_flip_visitor, &flip_callback, this);
  heap_->ThreadFlipEnd(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  is_asserting_to_space_invariant_ = true;
  QuasiAtomic::ThreadFenceForConstructor();
  if (kVerboseMode) {
    LOG(INFO) << "time=" << region_space_->Time();
    region_space_->DumpNonFreeRegions(LOG(INFO));
    LOG(INFO) << "GC end of FlipThreadRoots";
  }
}

void ConcurrentCopying::SwapStacks() {
  heap_->SwapStacks();
}

void ConcurrentCopying::RecordLiveStackFreezeSize(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  live_stack_freeze_size_ = heap_->GetLiveStack()->Size();
}

// Used to visit objects in the immune spaces.
class ConcurrentCopying::ImmuneSpaceObjVisitor {
 public:
  explicit ImmuneSpaceObjVisitor(ConcurrentCopying* cc) : collector_(cc) {}

  void operator()(mirror::Object* obj) const SHARED_REQUIRES(Locks::mutator_lock_)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(collector_->immune_spaces_.ContainsObject(obj));
    accounting::ContinuousSpaceBitmap* cc_bitmap =
        collector_->cc_heap_bitmap_->GetContinuousSpaceBitmap(obj);
    DCHECK(cc_bitmap != nullptr)
        << "An immune space object must have a bitmap";
    if (kIsDebugBuild) {
      DCHECK(collector_->heap_->GetMarkBitmap()->Test(obj))
          << "Immune space object must be already marked";
    }
    // This may or may not succeed, which is ok.
    if (kUseBakerReadBarrier) {
      obj->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
    }
    if (cc_bitmap->AtomicTestAndSet(obj)) {
      // Already marked. Do nothing.
    } else {
      // Newly marked. Set the gray bit and push it onto the mark stack.
      CHECK(!kUseBakerReadBarrier || obj->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
      collector_->PushOntoMarkStack(obj);
    }
  }

 private:
  ConcurrentCopying* const collector_;
};

class EmptyCheckpoint : public Closure {
 public:
  explicit EmptyCheckpoint(ConcurrentCopying* concurrent_copying)
      : concurrent_copying_(concurrent_copying) {
  }

  virtual void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    // If thread is a running mutator, then act on behalf of the garbage collector.
    // See the code in ThreadList::RunCheckpoint.
    concurrent_copying_->GetBarrier().Pass(self);
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
};

// Concurrently mark roots that are guarded by read barriers and process the mark stack.
void ConcurrentCopying::MarkingPhase() {
  TimingLogger::ScopedTiming split("MarkingPhase", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "GC MarkingPhase";
  }
  CHECK(weak_ref_access_enabled_);
  {
    // Mark the image root. The WB-based collectors do not need to
    // scan the image objects from roots by relying on the card table,
    // but it's necessary for the RB to-space invariant to hold.
    TimingLogger::ScopedTiming split1("VisitImageRoots", GetTimings());
    for (space::ContinuousSpace* space : heap_->GetContinuousSpaces()) {
      if (space->IsImageSpace()) {
        gc::space::ImageSpace* image = space->AsImageSpace();
        if (image != nullptr) {
          mirror::ObjectArray<mirror::Object>* image_root = image->GetImageHeader().GetImageRoots();
          mirror::Object* marked_image_root = Mark(image_root);
          CHECK_EQ(image_root, marked_image_root) << "An image object does not move";
          if (ReadBarrier::kEnableToSpaceInvariantChecks) {
            AssertToSpaceInvariant(nullptr, MemberOffset(0), marked_image_root);
          }
        }
      }
    }
  }
  {
    TimingLogger::ScopedTiming split2("VisitConcurrentRoots", GetTimings());
    Runtime::Current()->VisitConcurrentRoots(this, kVisitRootFlagAllRoots);
  }
  {
    // TODO: don't visit the transaction roots if it's not active.
    TimingLogger::ScopedTiming split5("VisitNonThreadRoots", GetTimings());
    Runtime::Current()->VisitNonThreadRoots(this);
  }

  // Immune spaces.
  for (auto& space : immune_spaces_.GetSpaces()) {
    DCHECK(space->IsImageSpace() || space->IsZygoteSpace());
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    ImmuneSpaceObjVisitor visitor(this);
    live_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                  reinterpret_cast<uintptr_t>(space->Limit()),
                                  visitor);
  }

  Thread* self = Thread::Current();
  {
    TimingLogger::ScopedTiming split7("ProcessMarkStack", GetTimings());
    // We transition through three mark stack modes (thread-local, shared, GC-exclusive). The
    // primary reasons are the fact that we need to use a checkpoint to process thread-local mark
    // stacks, but after we disable weak refs accesses, we can't use a checkpoint due to a deadlock
    // issue because running threads potentially blocking at WaitHoldingLocks, and that once we
    // reach the point where we process weak references, we can avoid using a lock when accessing
    // the GC mark stack, which makes mark stack processing more efficient.

    // Process the mark stack once in the thread local stack mode. This marks most of the live
    // objects, aside from weak ref accesses with read barriers (Reference::GetReferent() and system
    // weaks) that may happen concurrently while we processing the mark stack and newly mark/gray
    // objects and push refs on the mark stack.
    ProcessMarkStack();
    // Switch to the shared mark stack mode. That is, revoke and process thread-local mark stacks
    // for the last time before transitioning to the shared mark stack mode, which would process new
    // refs that may have been concurrently pushed onto the mark stack during the ProcessMarkStack()
    // call above. At the same time, disable weak ref accesses using a per-thread flag. It's
    // important to do these together in a single checkpoint so that we can ensure that mutators
    // won't newly gray objects and push new refs onto the mark stack due to weak ref accesses and
    // mutators safely transition to the shared mark stack mode (without leaving unprocessed refs on
    // the thread-local mark stacks), without a race. This is why we use a thread-local weak ref
    // access flag Thread::tls32_.weak_ref_access_enabled_ instead of the global ones.
    SwitchToSharedMarkStackMode();
    CHECK(!self->GetWeakRefAccessEnabled());
    // Now that weak refs accesses are disabled, once we exhaust the shared mark stack again here
    // (which may be non-empty if there were refs found on thread-local mark stacks during the above
    // SwitchToSharedMarkStackMode() call), we won't have new refs to process, that is, mutators
    // (via read barriers) have no way to produce any more refs to process. Marking converges once
    // before we process weak refs below.
    ProcessMarkStack();
    CheckEmptyMarkStack();
    // Switch to the GC exclusive mark stack mode so that we can process the mark stack without a
    // lock from this point on.
    SwitchToGcExclusiveMarkStackMode();
    CheckEmptyMarkStack();
    if (kVerboseMode) {
      LOG(INFO) << "ProcessReferences";
    }
    // Process weak references. This may produce new refs to process and have them processed via
    // ProcessMarkStack (in the GC exclusive mark stack mode).
    ProcessReferences(self);
    CheckEmptyMarkStack();
    if (kVerboseMode) {
      LOG(INFO) << "SweepSystemWeaks";
    }
    SweepSystemWeaks(self);
    if (kVerboseMode) {
      LOG(INFO) << "SweepSystemWeaks done";
    }
    // Process the mark stack here one last time because the above SweepSystemWeaks() call may have
    // marked some objects (strings alive) as hash_set::Erase() can call the hash function for
    // arbitrary elements in the weak intern table in InternTable::Table::SweepWeaks().
    ProcessMarkStack();
    CheckEmptyMarkStack();
    // Re-enable weak ref accesses.
    ReenableWeakRefAccess(self);
    // Free data for class loaders that we unloaded.
    Runtime::Current()->GetClassLinker()->CleanupClassLoaders();
    // Marking is done. Disable marking.
    DisableMarking();
    CheckEmptyMarkStack();
  }

  CHECK(weak_ref_access_enabled_);
  if (kVerboseMode) {
    LOG(INFO) << "GC end of MarkingPhase";
  }
}

void ConcurrentCopying::ReenableWeakRefAccess(Thread* self) {
  if (kVerboseMode) {
    LOG(INFO) << "ReenableWeakRefAccess";
  }
  weak_ref_access_enabled_.StoreRelaxed(true);  // This is for new threads.
  QuasiAtomic::ThreadFenceForConstructor();
  // Iterate all threads (don't need to or can't use a checkpoint) and re-enable weak ref access.
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (Thread* thread : thread_list) {
      thread->SetWeakRefAccessEnabled(true);
    }
  }
  // Unblock blocking threads.
  GetHeap()->GetReferenceProcessor()->BroadcastForSlowPath(self);
  Runtime::Current()->BroadcastForNewSystemWeaks();
}

class ConcurrentCopying::DisableMarkingCheckpoint : public Closure {
 public:
  explicit DisableMarkingCheckpoint(ConcurrentCopying* concurrent_copying)
      : concurrent_copying_(concurrent_copying) {
  }

  void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    DCHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    // Disable the thread-local is_gc_marking flag.
    // Note a thread that has just started right before this checkpoint may have already this flag
    // set to false, which is ok.
    thread->SetIsGcMarking(false);
    // If thread is a running mutator, then act on behalf of the garbage collector.
    // See the code in ThreadList::RunCheckpoint.
    concurrent_copying_->GetBarrier().Pass(self);
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
};

void ConcurrentCopying::IssueDisableMarkingCheckpoint() {
  Thread* self = Thread::Current();
  DisableMarkingCheckpoint check_point(this);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  gc_barrier_->Init(self, 0);
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // If there are no threads to wait which implies that all the checkpoint functions are finished,
  // then no need to release the mutator lock.
  if (barrier_count == 0) {
    return;
  }
  // Release locks then wait for all mutator threads to pass the barrier.
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
}

void ConcurrentCopying::DisableMarking() {
  // Change the global is_marking flag to false. Do a fence before doing a checkpoint to update the
  // thread-local flags so that a new thread starting up will get the correct is_marking flag.
  is_marking_ = false;
  QuasiAtomic::ThreadFenceForConstructor();
  // Use a checkpoint to turn off the thread-local is_gc_marking flags and to ensure no threads are
  // still in the middle of a read barrier which may have a from-space ref cached in a local
  // variable.
  IssueDisableMarkingCheckpoint();
  if (kUseTableLookupReadBarrier) {
    heap_->rb_table_->ClearAll();
    DCHECK(heap_->rb_table_->IsAllCleared());
  }
  is_mark_stack_push_disallowed_.StoreSequentiallyConsistent(1);
  mark_stack_mode_.StoreSequentiallyConsistent(kMarkStackModeOff);
}

void ConcurrentCopying::IssueEmptyCheckpoint() {
  Thread* self = Thread::Current();
  EmptyCheckpoint check_point(this);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  gc_barrier_->Init(self, 0);
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // If there are no threads to wait which implys that all the checkpoint functions are finished,
  // then no need to release the mutator lock.
  if (barrier_count == 0) {
    return;
  }
  // Release locks then wait for all mutator threads to pass the barrier.
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
}

void ConcurrentCopying::ExpandGcMarkStack() {
  DCHECK(gc_mark_stack_->IsFull());
  const size_t new_size = gc_mark_stack_->Capacity() * 2;
  std::vector<StackReference<mirror::Object>> temp(gc_mark_stack_->Begin(),
                                                   gc_mark_stack_->End());
  gc_mark_stack_->Resize(new_size);
  for (auto& ref : temp) {
    gc_mark_stack_->PushBack(ref.AsMirrorPtr());
  }
  DCHECK(!gc_mark_stack_->IsFull());
}

void ConcurrentCopying::PushOntoMarkStack(mirror::Object* to_ref) {
  CHECK_EQ(is_mark_stack_push_disallowed_.LoadRelaxed(), 0)
      << " " << to_ref << " " << PrettyTypeOf(to_ref);
  Thread* self = Thread::Current();  // TODO: pass self as an argument from call sites?
  CHECK(thread_running_gc_ != nullptr);
  MarkStackMode mark_stack_mode = mark_stack_mode_.LoadRelaxed();
  if (LIKELY(mark_stack_mode == kMarkStackModeThreadLocal)) {
    if (LIKELY(self == thread_running_gc_)) {
      // If GC-running thread, use the GC mark stack instead of a thread-local mark stack.
      CHECK(self->GetThreadLocalMarkStack() == nullptr);
      if (UNLIKELY(gc_mark_stack_->IsFull())) {
        ExpandGcMarkStack();
      }
      gc_mark_stack_->PushBack(to_ref);
    } else {
      // Otherwise, use a thread-local mark stack.
      accounting::AtomicStack<mirror::Object>* tl_mark_stack = self->GetThreadLocalMarkStack();
      if (UNLIKELY(tl_mark_stack == nullptr || tl_mark_stack->IsFull())) {
        MutexLock mu(self, mark_stack_lock_);
        // Get a new thread local mark stack.
        accounting::AtomicStack<mirror::Object>* new_tl_mark_stack;
        if (!pooled_mark_stacks_.empty()) {
          // Use a pooled mark stack.
          new_tl_mark_stack = pooled_mark_stacks_.back();
          pooled_mark_stacks_.pop_back();
        } else {
          // None pooled. Create a new one.
          new_tl_mark_stack =
              accounting::AtomicStack<mirror::Object>::Create(
                  "thread local mark stack", 4 * KB, 4 * KB);
        }
        DCHECK(new_tl_mark_stack != nullptr);
        DCHECK(new_tl_mark_stack->IsEmpty());
        new_tl_mark_stack->PushBack(to_ref);
        self->SetThreadLocalMarkStack(new_tl_mark_stack);
        if (tl_mark_stack != nullptr) {
          // Store the old full stack into a vector.
          revoked_mark_stacks_.push_back(tl_mark_stack);
        }
      } else {
        tl_mark_stack->PushBack(to_ref);
      }
    }
  } else if (mark_stack_mode == kMarkStackModeShared) {
    // Access the shared GC mark stack with a lock.
    MutexLock mu(self, mark_stack_lock_);
    if (UNLIKELY(gc_mark_stack_->IsFull())) {
      ExpandGcMarkStack();
    }
    gc_mark_stack_->PushBack(to_ref);
  } else {
    CHECK_EQ(static_cast<uint32_t>(mark_stack_mode),
             static_cast<uint32_t>(kMarkStackModeGcExclusive))
        << "ref=" << to_ref
        << " self->gc_marking=" << self->GetIsGcMarking()
        << " cc->is_marking=" << is_marking_;
    CHECK(self == thread_running_gc_)
        << "Only GC-running thread should access the mark stack "
        << "in the GC exclusive mark stack mode";
    // Access the GC mark stack without a lock.
    if (UNLIKELY(gc_mark_stack_->IsFull())) {
      ExpandGcMarkStack();
    }
    gc_mark_stack_->PushBack(to_ref);
  }
}

accounting::ObjectStack* ConcurrentCopying::GetAllocationStack() {
  return heap_->allocation_stack_.get();
}

accounting::ObjectStack* ConcurrentCopying::GetLiveStack() {
  return heap_->live_stack_.get();
}

// The following visitors are that used to verify that there's no
// references to the from-space left after marking.
class ConcurrentCopying::VerifyNoFromSpaceRefsVisitor : public SingleRootVisitor {
 public:
  explicit VerifyNoFromSpaceRefsVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    if (ref == nullptr) {
      // OK.
      return;
    }
    collector_->AssertToSpaceInvariant(nullptr, MemberOffset(0), ref);
    if (kUseBakerReadBarrier) {
      if (collector_->RegionSpace()->IsInToSpace(ref)) {
        CHECK(ref->GetReadBarrierPointer() == nullptr)
            << "To-space ref " << ref << " " << PrettyTypeOf(ref)
            << " has non-white rb_ptr " << ref->GetReadBarrierPointer();
      } else {
        CHECK(ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr() ||
              (ref->GetReadBarrierPointer() == ReadBarrier::WhitePtr() &&
               collector_->IsOnAllocStack(ref)))
            << "Non-moving/unevac from space ref " << ref << " " << PrettyTypeOf(ref)
            << " has non-black rb_ptr " << ref->GetReadBarrierPointer()
            << " but isn't on the alloc stack (and has white rb_ptr)."
            << " Is it in the non-moving space="
            << (collector_->GetHeap()->GetNonMovingSpace()->HasAddress(ref));
      }
    }
  }

  void VisitRoot(mirror::Object* root, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(root != nullptr);
    operator()(root);
  }

 private:
  ConcurrentCopying* const collector_;
};

class ConcurrentCopying::VerifyNoFromSpaceRefsFieldVisitor {
 public:
  explicit VerifyNoFromSpaceRefsFieldVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    mirror::Object* ref =
        obj->GetFieldObject<mirror::Object, kDefaultVerifyFlags, kWithoutReadBarrier>(offset);
    VerifyNoFromSpaceRefsVisitor visitor(collector_);
    visitor(ref);
  }
  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    CHECK(klass->IsTypeOfReferenceClass());
    this->operator()(ref, mirror::Reference::ReferentOffset(), false);
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    VerifyNoFromSpaceRefsVisitor visitor(collector_);
    visitor(root->AsMirrorPtr());
  }

 private:
  ConcurrentCopying* const collector_;
};

class ConcurrentCopying::VerifyNoFromSpaceRefsObjectVisitor {
 public:
  explicit VerifyNoFromSpaceRefsObjectVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}
  void operator()(mirror::Object* obj) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ObjectCallback(obj, collector_);
  }
  static void ObjectCallback(mirror::Object* obj, void *arg)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK(obj != nullptr);
    ConcurrentCopying* collector = reinterpret_cast<ConcurrentCopying*>(arg);
    space::RegionSpace* region_space = collector->RegionSpace();
    CHECK(!region_space->IsInFromSpace(obj)) << "Scanning object " << obj << " in from space";
    VerifyNoFromSpaceRefsFieldVisitor visitor(collector);
    obj->VisitReferences(visitor, visitor);
    if (kUseBakerReadBarrier) {
      if (collector->RegionSpace()->IsInToSpace(obj)) {
        CHECK(obj->GetReadBarrierPointer() == nullptr)
            << "obj=" << obj << " non-white rb_ptr " << obj->GetReadBarrierPointer();
      } else {
        CHECK(obj->GetReadBarrierPointer() == ReadBarrier::BlackPtr() ||
              (obj->GetReadBarrierPointer() == ReadBarrier::WhitePtr() &&
               collector->IsOnAllocStack(obj)))
            << "Non-moving space/unevac from space ref " << obj << " " << PrettyTypeOf(obj)
            << " has non-black rb_ptr " << obj->GetReadBarrierPointer()
            << " but isn't on the alloc stack (and has white rb_ptr). Is it in the non-moving space="
            << (collector->GetHeap()->GetNonMovingSpace()->HasAddress(obj));
      }
    }
  }

 private:
  ConcurrentCopying* const collector_;
};

// Verify there's no from-space references left after the marking phase.
void ConcurrentCopying::VerifyNoFromSpaceReferences() {
  Thread* self = Thread::Current();
  DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self));
  // Verify all threads have is_gc_marking to be false
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (Thread* thread : thread_list) {
      CHECK(!thread->GetIsGcMarking());
    }
  }
  VerifyNoFromSpaceRefsObjectVisitor visitor(this);
  // Roots.
  {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    VerifyNoFromSpaceRefsVisitor ref_visitor(this);
    Runtime::Current()->VisitRoots(&ref_visitor);
  }
  // The to-space.
  region_space_->WalkToSpace(VerifyNoFromSpaceRefsObjectVisitor::ObjectCallback, this);
  // Non-moving spaces.
  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap_->GetMarkBitmap()->Visit(visitor);
  }
  // The alloc stack.
  {
    VerifyNoFromSpaceRefsVisitor ref_visitor(this);
    for (auto* it = heap_->allocation_stack_->Begin(), *end = heap_->allocation_stack_->End();
        it < end; ++it) {
      mirror::Object* const obj = it->AsMirrorPtr();
      if (obj != nullptr && obj->GetClass() != nullptr) {
        // TODO: need to call this only if obj is alive?
        ref_visitor(obj);
        visitor(obj);
      }
    }
  }
  // TODO: LOS. But only refs in LOS are classes.
}

// The following visitors are used to assert the to-space invariant.
class ConcurrentCopying::AssertToSpaceInvariantRefsVisitor {
 public:
  explicit AssertToSpaceInvariantRefsVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    if (ref == nullptr) {
      // OK.
      return;
    }
    collector_->AssertToSpaceInvariant(nullptr, MemberOffset(0), ref);
  }

 private:
  ConcurrentCopying* const collector_;
};

class ConcurrentCopying::AssertToSpaceInvariantFieldVisitor {
 public:
  explicit AssertToSpaceInvariantFieldVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    mirror::Object* ref =
        obj->GetFieldObject<mirror::Object, kDefaultVerifyFlags, kWithoutReadBarrier>(offset);
    AssertToSpaceInvariantRefsVisitor visitor(collector_);
    visitor(ref);
  }
  void operator()(mirror::Class* klass, mirror::Reference* ref ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    CHECK(klass->IsTypeOfReferenceClass());
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    AssertToSpaceInvariantRefsVisitor visitor(collector_);
    visitor(root->AsMirrorPtr());
  }

 private:
  ConcurrentCopying* const collector_;
};

class ConcurrentCopying::AssertToSpaceInvariantObjectVisitor {
 public:
  explicit AssertToSpaceInvariantObjectVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}
  void operator()(mirror::Object* obj) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ObjectCallback(obj, collector_);
  }
  static void ObjectCallback(mirror::Object* obj, void *arg)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK(obj != nullptr);
    ConcurrentCopying* collector = reinterpret_cast<ConcurrentCopying*>(arg);
    space::RegionSpace* region_space = collector->RegionSpace();
    CHECK(!region_space->IsInFromSpace(obj)) << "Scanning object " << obj << " in from space";
    collector->AssertToSpaceInvariant(nullptr, MemberOffset(0), obj);
    AssertToSpaceInvariantFieldVisitor visitor(collector);
    obj->VisitReferences(visitor, visitor);
  }

 private:
  ConcurrentCopying* const collector_;
};

class ConcurrentCopying::RevokeThreadLocalMarkStackCheckpoint : public Closure {
 public:
  RevokeThreadLocalMarkStackCheckpoint(ConcurrentCopying* concurrent_copying,
                                       bool disable_weak_ref_access)
      : concurrent_copying_(concurrent_copying),
        disable_weak_ref_access_(disable_weak_ref_access) {
  }

  virtual void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    // Revoke thread local mark stacks.
    accounting::AtomicStack<mirror::Object>* tl_mark_stack = thread->GetThreadLocalMarkStack();
    if (tl_mark_stack != nullptr) {
      MutexLock mu(self, concurrent_copying_->mark_stack_lock_);
      concurrent_copying_->revoked_mark_stacks_.push_back(tl_mark_stack);
      thread->SetThreadLocalMarkStack(nullptr);
    }
    // Disable weak ref access.
    if (disable_weak_ref_access_) {
      thread->SetWeakRefAccessEnabled(false);
    }
    // If thread is a running mutator, then act on behalf of the garbage collector.
    // See the code in ThreadList::RunCheckpoint.
    concurrent_copying_->GetBarrier().Pass(self);
  }

 private:
  ConcurrentCopying* const concurrent_copying_;
  const bool disable_weak_ref_access_;
};

void ConcurrentCopying::RevokeThreadLocalMarkStacks(bool disable_weak_ref_access) {
  Thread* self = Thread::Current();
  RevokeThreadLocalMarkStackCheckpoint check_point(this, disable_weak_ref_access);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  gc_barrier_->Init(self, 0);
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // If there are no threads to wait which implys that all the checkpoint functions are finished,
  // then no need to release the mutator lock.
  if (barrier_count == 0) {
    return;
  }
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
}

void ConcurrentCopying::RevokeThreadLocalMarkStack(Thread* thread) {
  Thread* self = Thread::Current();
  CHECK_EQ(self, thread);
  accounting::AtomicStack<mirror::Object>* tl_mark_stack = thread->GetThreadLocalMarkStack();
  if (tl_mark_stack != nullptr) {
    CHECK(is_marking_);
    MutexLock mu(self, mark_stack_lock_);
    revoked_mark_stacks_.push_back(tl_mark_stack);
    thread->SetThreadLocalMarkStack(nullptr);
  }
}

void ConcurrentCopying::ProcessMarkStack() {
  if (kVerboseMode) {
    LOG(INFO) << "ProcessMarkStack. ";
  }
  bool empty_prev = false;
  while (true) {
    bool empty = ProcessMarkStackOnce();
    if (empty_prev && empty) {
      // Saw empty mark stack for a second time, done.
      break;
    }
    empty_prev = empty;
  }
}

bool ConcurrentCopying::ProcessMarkStackOnce() {
  Thread* self = Thread::Current();
  CHECK(thread_running_gc_ != nullptr);
  CHECK(self == thread_running_gc_);
  CHECK(self->GetThreadLocalMarkStack() == nullptr);
  size_t count = 0;
  MarkStackMode mark_stack_mode = mark_stack_mode_.LoadRelaxed();
  if (mark_stack_mode == kMarkStackModeThreadLocal) {
    // Process the thread-local mark stacks and the GC mark stack.
    count += ProcessThreadLocalMarkStacks(false);
    while (!gc_mark_stack_->IsEmpty()) {
      mirror::Object* to_ref = gc_mark_stack_->PopBack();
      ProcessMarkStackRef(to_ref);
      ++count;
    }
    gc_mark_stack_->Reset();
  } else if (mark_stack_mode == kMarkStackModeShared) {
    // Process the shared GC mark stack with a lock.
    {
      MutexLock mu(self, mark_stack_lock_);
      CHECK(revoked_mark_stacks_.empty());
    }
    while (true) {
      std::vector<mirror::Object*> refs;
      {
        // Copy refs with lock. Note the number of refs should be small.
        MutexLock mu(self, mark_stack_lock_);
        if (gc_mark_stack_->IsEmpty()) {
          break;
        }
        for (StackReference<mirror::Object>* p = gc_mark_stack_->Begin();
             p != gc_mark_stack_->End(); ++p) {
          refs.push_back(p->AsMirrorPtr());
        }
        gc_mark_stack_->Reset();
      }
      for (mirror::Object* ref : refs) {
        ProcessMarkStackRef(ref);
        ++count;
      }
    }
  } else {
    CHECK_EQ(static_cast<uint32_t>(mark_stack_mode),
             static_cast<uint32_t>(kMarkStackModeGcExclusive));
    {
      MutexLock mu(self, mark_stack_lock_);
      CHECK(revoked_mark_stacks_.empty());
    }
    // Process the GC mark stack in the exclusive mode. No need to take the lock.
    while (!gc_mark_stack_->IsEmpty()) {
      mirror::Object* to_ref = gc_mark_stack_->PopBack();
      ProcessMarkStackRef(to_ref);
      ++count;
    }
    gc_mark_stack_->Reset();
  }

  // Return true if the stack was empty.
  return count == 0;
}

size_t ConcurrentCopying::ProcessThreadLocalMarkStacks(bool disable_weak_ref_access) {
  // Run a checkpoint to collect all thread local mark stacks and iterate over them all.
  RevokeThreadLocalMarkStacks(disable_weak_ref_access);
  size_t count = 0;
  std::vector<accounting::AtomicStack<mirror::Object>*> mark_stacks;
  {
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    // Make a copy of the mark stack vector.
    mark_stacks = revoked_mark_stacks_;
    revoked_mark_stacks_.clear();
  }
  for (accounting::AtomicStack<mirror::Object>* mark_stack : mark_stacks) {
    for (StackReference<mirror::Object>* p = mark_stack->Begin(); p != mark_stack->End(); ++p) {
      mirror::Object* to_ref = p->AsMirrorPtr();
      ProcessMarkStackRef(to_ref);
      ++count;
    }
    {
      MutexLock mu(Thread::Current(), mark_stack_lock_);
      if (pooled_mark_stacks_.size() >= kMarkStackPoolSize) {
        // The pool has enough. Delete it.
        delete mark_stack;
      } else {
        // Otherwise, put it into the pool for later reuse.
        mark_stack->Reset();
        pooled_mark_stacks_.push_back(mark_stack);
      }
    }
  }
  return count;
}

inline void ConcurrentCopying::ProcessMarkStackRef(mirror::Object* to_ref) {
  DCHECK(!region_space_->IsInFromSpace(to_ref));
  if (kUseBakerReadBarrier) {
    DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr())
        << " " << to_ref << " " << to_ref->GetReadBarrierPointer()
        << " is_marked=" << IsMarked(to_ref);
  }
  // Scan ref fields.
  Scan(to_ref);
  // Mark the gray ref as white or black.
  if (kUseBakerReadBarrier) {
    DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr())
        << " " << to_ref << " " << to_ref->GetReadBarrierPointer()
        << " is_marked=" << IsMarked(to_ref);
  }
#ifdef USE_BAKER_OR_BROOKS_READ_BARRIER
  if (UNLIKELY((to_ref->GetClass<kVerifyNone, kWithoutReadBarrier>()->IsTypeOfReferenceClass() &&
                to_ref->AsReference()->GetReferent<kWithoutReadBarrier>() != nullptr &&
                !IsInToSpace(to_ref->AsReference()->GetReferent<kWithoutReadBarrier>())))) {
    // Leave this Reference gray in the queue so that GetReferent() will trigger a read barrier. We
    // will change it to black or white later in ReferenceQueue::DequeuePendingReference().
    DCHECK(to_ref->AsReference()->GetPendingNext() != nullptr) << "Left unenqueued ref gray " << to_ref;
  } else {
    // We may occasionally leave a Reference black or white in the queue if its referent happens to
    // be concurrently marked after the Scan() call above has enqueued the Reference, in which case
    // the above IsInToSpace() evaluates to true and we change the color from gray to black or white
    // here in this else block.
    if (kUseBakerReadBarrier) {
      if (region_space_->IsInToSpace(to_ref)) {
        // If to-space, change from gray to white.
        bool success = to_ref->AtomicSetReadBarrierPointer</*kCasRelease*/true>(
            ReadBarrier::GrayPtr(),
            ReadBarrier::WhitePtr());
        DCHECK(success) << "Must succeed as we won the race.";
        DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::WhitePtr());
      } else {
        // If non-moving space/unevac from space, change from gray
        // to black. We can't change gray to white because it's not
        // safe to use CAS if two threads change values in opposite
        // directions (A->B and B->A). So, we change it to black to
        // indicate non-moving objects that have been marked
        // through. Note we'd need to change from black to white
        // later (concurrently).
        bool success = to_ref->AtomicSetReadBarrierPointer</*kCasRelease*/true>(
            ReadBarrier::GrayPtr(),
            ReadBarrier::BlackPtr());
        DCHECK(success) << "Must succeed as we won the race.";
        DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr());
      }
    }
  }
#else
  DCHECK(!kUseBakerReadBarrier);
#endif
  if (ReadBarrier::kEnableToSpaceInvariantChecks || kIsDebugBuild) {
    AssertToSpaceInvariantObjectVisitor visitor(this);
    visitor(to_ref);
  }
}

void ConcurrentCopying::SwitchToSharedMarkStackMode() {
  Thread* self = Thread::Current();
  CHECK(thread_running_gc_ != nullptr);
  CHECK_EQ(self, thread_running_gc_);
  CHECK(self->GetThreadLocalMarkStack() == nullptr);
  MarkStackMode before_mark_stack_mode = mark_stack_mode_.LoadRelaxed();
  CHECK_EQ(static_cast<uint32_t>(before_mark_stack_mode),
           static_cast<uint32_t>(kMarkStackModeThreadLocal));
  mark_stack_mode_.StoreRelaxed(kMarkStackModeShared);
  CHECK(weak_ref_access_enabled_.LoadRelaxed());
  weak_ref_access_enabled_.StoreRelaxed(false);
  QuasiAtomic::ThreadFenceForConstructor();
  // Process the thread local mark stacks one last time after switching to the shared mark stack
  // mode and disable weak ref accesses.
  ProcessThreadLocalMarkStacks(true);
  if (kVerboseMode) {
    LOG(INFO) << "Switched to shared mark stack mode and disabled weak ref access";
  }
}

void ConcurrentCopying::SwitchToGcExclusiveMarkStackMode() {
  Thread* self = Thread::Current();
  CHECK(thread_running_gc_ != nullptr);
  CHECK_EQ(self, thread_running_gc_);
  CHECK(self->GetThreadLocalMarkStack() == nullptr);
  MarkStackMode before_mark_stack_mode = mark_stack_mode_.LoadRelaxed();
  CHECK_EQ(static_cast<uint32_t>(before_mark_stack_mode),
           static_cast<uint32_t>(kMarkStackModeShared));
  mark_stack_mode_.StoreRelaxed(kMarkStackModeGcExclusive);
  QuasiAtomic::ThreadFenceForConstructor();
  if (kVerboseMode) {
    LOG(INFO) << "Switched to GC exclusive mark stack mode";
  }
}

void ConcurrentCopying::CheckEmptyMarkStack() {
  Thread* self = Thread::Current();
  CHECK(thread_running_gc_ != nullptr);
  CHECK_EQ(self, thread_running_gc_);
  CHECK(self->GetThreadLocalMarkStack() == nullptr);
  MarkStackMode mark_stack_mode = mark_stack_mode_.LoadRelaxed();
  if (mark_stack_mode == kMarkStackModeThreadLocal) {
    // Thread-local mark stack mode.
    RevokeThreadLocalMarkStacks(false);
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    if (!revoked_mark_stacks_.empty()) {
      for (accounting::AtomicStack<mirror::Object>* mark_stack : revoked_mark_stacks_) {
        while (!mark_stack->IsEmpty()) {
          mirror::Object* obj = mark_stack->PopBack();
          if (kUseBakerReadBarrier) {
            mirror::Object* rb_ptr = obj->GetReadBarrierPointer();
            LOG(INFO) << "On mark queue : " << obj << " " << PrettyTypeOf(obj) << " rb_ptr=" << rb_ptr
                      << " is_marked=" << IsMarked(obj);
          } else {
            LOG(INFO) << "On mark queue : " << obj << " " << PrettyTypeOf(obj)
                      << " is_marked=" << IsMarked(obj);
          }
        }
      }
      LOG(FATAL) << "mark stack is not empty";
    }
  } else {
    // Shared, GC-exclusive, or off.
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    CHECK(gc_mark_stack_->IsEmpty());
    CHECK(revoked_mark_stacks_.empty());
  }
}

void ConcurrentCopying::SweepSystemWeaks(Thread* self) {
  TimingLogger::ScopedTiming split("SweepSystemWeaks", GetTimings());
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  Runtime::Current()->SweepSystemWeaks(this);
}

void ConcurrentCopying::Sweep(bool swap_bitmaps) {
  {
    TimingLogger::ScopedTiming t("MarkStackAsLive", GetTimings());
    accounting::ObjectStack* live_stack = heap_->GetLiveStack();
    if (kEnableFromSpaceAccountingCheck) {
      CHECK_GE(live_stack_freeze_size_, live_stack->Size());
    }
    heap_->MarkAllocStackAsLive(live_stack);
    live_stack->Reset();
  }
  CheckEmptyMarkStack();
  TimingLogger::ScopedTiming split("Sweep", GetTimings());
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      if (space == region_space_ || immune_spaces_.ContainsSpace(space)) {
        continue;
      }
      TimingLogger::ScopedTiming split2(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepAllocSpace", GetTimings());
      RecordFree(alloc_space->Sweep(swap_bitmaps));
    }
  }
  SweepLargeObjects(swap_bitmaps);
}

void ConcurrentCopying::SweepLargeObjects(bool swap_bitmaps) {
  TimingLogger::ScopedTiming split("SweepLargeObjects", GetTimings());
  RecordFreeLOS(heap_->GetLargeObjectsSpace()->Sweep(swap_bitmaps));
}

class ConcurrentCopying::ClearBlackPtrsVisitor {
 public:
  explicit ClearBlackPtrsVisitor(ConcurrentCopying* cc) : collector_(cc) {}
  void operator()(mirror::Object* obj) const SHARED_REQUIRES(Locks::mutator_lock_)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_) {
    DCHECK(obj != nullptr);
    DCHECK(collector_->heap_->GetMarkBitmap()->Test(obj)) << obj;
    DCHECK_EQ(obj->GetReadBarrierPointer(), ReadBarrier::BlackPtr()) << obj;
    obj->AtomicSetReadBarrierPointer(ReadBarrier::BlackPtr(), ReadBarrier::WhitePtr());
    DCHECK_EQ(obj->GetReadBarrierPointer(), ReadBarrier::WhitePtr()) << obj;
  }

 private:
  ConcurrentCopying* const collector_;
};

// Clear the black ptrs in non-moving objects back to white.
void ConcurrentCopying::ClearBlackPtrs() {
  CHECK(kUseBakerReadBarrier);
  TimingLogger::ScopedTiming split("ClearBlackPtrs", GetTimings());
  ClearBlackPtrsVisitor visitor(this);
  for (auto& space : heap_->GetContinuousSpaces()) {
    if (space == region_space_) {
      continue;
    }
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (kVerboseMode) {
      LOG(INFO) << "ClearBlackPtrs: " << *space << " bitmap: " << *mark_bitmap;
    }
    mark_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                  reinterpret_cast<uintptr_t>(space->Limit()),
                                  visitor);
  }
  space::LargeObjectSpace* large_object_space = heap_->GetLargeObjectsSpace();
  large_object_space->GetMarkBitmap()->VisitMarkedRange(
      reinterpret_cast<uintptr_t>(large_object_space->Begin()),
      reinterpret_cast<uintptr_t>(large_object_space->End()),
      visitor);
  // Objects on the allocation stack?
  if (ReadBarrier::kEnableReadBarrierInvariantChecks || kIsDebugBuild) {
    size_t count = GetAllocationStack()->Size();
    auto* it = GetAllocationStack()->Begin();
    auto* end = GetAllocationStack()->End();
    for (size_t i = 0; i < count; ++i, ++it) {
      CHECK_LT(it, end);
      mirror::Object* obj = it->AsMirrorPtr();
      if (obj != nullptr) {
        // Must have been cleared above.
        CHECK_EQ(obj->GetReadBarrierPointer(), ReadBarrier::WhitePtr()) << obj;
      }
    }
  }
}

void ConcurrentCopying::ReclaimPhase() {
  TimingLogger::ScopedTiming split("ReclaimPhase", GetTimings());
  if (kVerboseMode) {
    LOG(INFO) << "GC ReclaimPhase";
  }
  Thread* self = Thread::Current();

  {
    // Double-check that the mark stack is empty.
    // Note: need to set this after VerifyNoFromSpaceRef().
    is_asserting_to_space_invariant_ = false;
    QuasiAtomic::ThreadFenceForConstructor();
    if (kVerboseMode) {
      LOG(INFO) << "Issue an empty check point. ";
    }
    IssueEmptyCheckpoint();
    // Disable the check.
    is_mark_stack_push_disallowed_.StoreSequentiallyConsistent(0);
    CheckEmptyMarkStack();
  }

  {
    // Record freed objects.
    TimingLogger::ScopedTiming split2("RecordFree", GetTimings());
    // Don't include thread-locals that are in the to-space.
    uint64_t from_bytes = region_space_->GetBytesAllocatedInFromSpace();
    uint64_t from_objects = region_space_->GetObjectsAllocatedInFromSpace();
    uint64_t unevac_from_bytes = region_space_->GetBytesAllocatedInUnevacFromSpace();
    uint64_t unevac_from_objects = region_space_->GetObjectsAllocatedInUnevacFromSpace();
    uint64_t to_bytes = bytes_moved_.LoadSequentiallyConsistent();
    uint64_t to_objects = objects_moved_.LoadSequentiallyConsistent();
    if (kEnableFromSpaceAccountingCheck) {
      CHECK_EQ(from_space_num_objects_at_first_pause_, from_objects + unevac_from_objects);
      CHECK_EQ(from_space_num_bytes_at_first_pause_, from_bytes + unevac_from_bytes);
    }
    CHECK_LE(to_objects, from_objects);
    CHECK_LE(to_bytes, from_bytes);
    int64_t freed_bytes = from_bytes - to_bytes;
    int64_t freed_objects = from_objects - to_objects;
    if (kVerboseMode) {
      LOG(INFO) << "RecordFree:"
                << " from_bytes=" << from_bytes << " from_objects=" << from_objects
                << " unevac_from_bytes=" << unevac_from_bytes << " unevac_from_objects=" << unevac_from_objects
                << " to_bytes=" << to_bytes << " to_objects=" << to_objects
                << " freed_bytes=" << freed_bytes << " freed_objects=" << freed_objects
                << " from_space size=" << region_space_->FromSpaceSize()
                << " unevac_from_space size=" << region_space_->UnevacFromSpaceSize()
                << " to_space size=" << region_space_->ToSpaceSize();
      LOG(INFO) << "(before) num_bytes_allocated=" << heap_->num_bytes_allocated_.LoadSequentiallyConsistent();
    }
    RecordFree(ObjectBytePair(freed_objects, freed_bytes));
    if (kVerboseMode) {
      LOG(INFO) << "(after) num_bytes_allocated=" << heap_->num_bytes_allocated_.LoadSequentiallyConsistent();
    }
  }

  {
    TimingLogger::ScopedTiming split3("ComputeUnevacFromSpaceLiveRatio", GetTimings());
    ComputeUnevacFromSpaceLiveRatio();
  }

  {
    TimingLogger::ScopedTiming split4("ClearFromSpace", GetTimings());
    region_space_->ClearFromSpace();
  }

  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    if (kUseBakerReadBarrier) {
      ClearBlackPtrs();
    }
    Sweep(false);
    SwapBitmaps();
    heap_->UnBindBitmaps();

    // Remove bitmaps for the immune spaces.
    while (!cc_bitmaps_.empty()) {
      accounting::ContinuousSpaceBitmap* cc_bitmap = cc_bitmaps_.back();
      cc_heap_bitmap_->RemoveContinuousSpaceBitmap(cc_bitmap);
      delete cc_bitmap;
      cc_bitmaps_.pop_back();
    }
    region_space_bitmap_ = nullptr;
  }

  CheckEmptyMarkStack();

  if (kVerboseMode) {
    LOG(INFO) << "GC end of ReclaimPhase";
  }
}

class ConcurrentCopying::ComputeUnevacFromSpaceLiveRatioVisitor {
 public:
  explicit ComputeUnevacFromSpaceLiveRatioVisitor(ConcurrentCopying* cc)
      : collector_(cc) {}
  void operator()(mirror::Object* ref) const SHARED_REQUIRES(Locks::mutator_lock_)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_) {
    DCHECK(ref != nullptr);
    DCHECK(collector_->region_space_bitmap_->Test(ref)) << ref;
    DCHECK(collector_->region_space_->IsInUnevacFromSpace(ref)) << ref;
    if (kUseBakerReadBarrier) {
      DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::BlackPtr()) << ref;
      // Clear the black ptr.
      ref->AtomicSetReadBarrierPointer(ReadBarrier::BlackPtr(), ReadBarrier::WhitePtr());
      DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::WhitePtr()) << ref;
    }
    size_t obj_size = ref->SizeOf();
    size_t alloc_size = RoundUp(obj_size, space::RegionSpace::kAlignment);
    collector_->region_space_->AddLiveBytes(ref, alloc_size);
  }

 private:
  ConcurrentCopying* const collector_;
};

// Compute how much live objects are left in regions.
void ConcurrentCopying::ComputeUnevacFromSpaceLiveRatio() {
  region_space_->AssertAllRegionLiveBytesZeroOrCleared();
  ComputeUnevacFromSpaceLiveRatioVisitor visitor(this);
  region_space_bitmap_->VisitMarkedRange(reinterpret_cast<uintptr_t>(region_space_->Begin()),
                                         reinterpret_cast<uintptr_t>(region_space_->Limit()),
                                         visitor);
}

// Assert the to-space invariant.
void ConcurrentCopying::AssertToSpaceInvariant(mirror::Object* obj, MemberOffset offset,
                                               mirror::Object* ref) {
  CHECK(heap_->collector_type_ == kCollectorTypeCC) << static_cast<size_t>(heap_->collector_type_);
  if (is_asserting_to_space_invariant_) {
    if (region_space_->IsInToSpace(ref)) {
      // OK.
      return;
    } else if (region_space_->IsInUnevacFromSpace(ref)) {
      CHECK(region_space_bitmap_->Test(ref)) << ref;
    } else if (region_space_->IsInFromSpace(ref)) {
      // Not OK. Do extra logging.
      if (obj != nullptr) {
        LogFromSpaceRefHolder(obj, offset);
      }
      ref->GetLockWord(false).Dump(LOG(INTERNAL_FATAL));
      CHECK(false) << "Found from-space ref " << ref << " " << PrettyTypeOf(ref);
    } else {
      AssertToSpaceInvariantInNonMovingSpace(obj, ref);
    }
  }
}

class RootPrinter {
 public:
  RootPrinter() { }

  template <class MirrorType>
  ALWAYS_INLINE void VisitRootIfNonNull(mirror::CompressedReference<MirrorType>* root)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  template <class MirrorType>
  void VisitRoot(mirror::Object** root)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    LOG(INTERNAL_FATAL) << "root=" << root << " ref=" << *root;
  }

  template <class MirrorType>
  void VisitRoot(mirror::CompressedReference<MirrorType>* root)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    LOG(INTERNAL_FATAL) << "root=" << root << " ref=" << root->AsMirrorPtr();
  }
};

void ConcurrentCopying::AssertToSpaceInvariant(GcRootSource* gc_root_source,
                                               mirror::Object* ref) {
  CHECK(heap_->collector_type_ == kCollectorTypeCC) << static_cast<size_t>(heap_->collector_type_);
  if (is_asserting_to_space_invariant_) {
    if (region_space_->IsInToSpace(ref)) {
      // OK.
      return;
    } else if (region_space_->IsInUnevacFromSpace(ref)) {
      CHECK(region_space_bitmap_->Test(ref)) << ref;
    } else if (region_space_->IsInFromSpace(ref)) {
      // Not OK. Do extra logging.
      if (gc_root_source == nullptr) {
        // No info.
      } else if (gc_root_source->HasArtField()) {
        ArtField* field = gc_root_source->GetArtField();
        LOG(INTERNAL_FATAL) << "gc root in field " << field << " " << PrettyField(field);
        RootPrinter root_printer;
        field->VisitRoots(root_printer);
      } else if (gc_root_source->HasArtMethod()) {
        ArtMethod* method = gc_root_source->GetArtMethod();
        LOG(INTERNAL_FATAL) << "gc root in method " << method << " " << PrettyMethod(method);
        RootPrinter root_printer;
        method->VisitRoots(root_printer, sizeof(void*));
      }
      ref->GetLockWord(false).Dump(LOG(INTERNAL_FATAL));
      region_space_->DumpNonFreeRegions(LOG(INTERNAL_FATAL));
      PrintFileToLog("/proc/self/maps", LogSeverity::INTERNAL_FATAL);
      MemMap::DumpMaps(LOG(INTERNAL_FATAL), true);
      CHECK(false) << "Found from-space ref " << ref << " " << PrettyTypeOf(ref);
    } else {
      AssertToSpaceInvariantInNonMovingSpace(nullptr, ref);
    }
  }
}

void ConcurrentCopying::LogFromSpaceRefHolder(mirror::Object* obj, MemberOffset offset) {
  if (kUseBakerReadBarrier) {
    LOG(INFO) << "holder=" << obj << " " << PrettyTypeOf(obj)
              << " holder rb_ptr=" << obj->GetReadBarrierPointer();
  } else {
    LOG(INFO) << "holder=" << obj << " " << PrettyTypeOf(obj);
  }
  if (region_space_->IsInFromSpace(obj)) {
    LOG(INFO) << "holder is in the from-space.";
  } else if (region_space_->IsInToSpace(obj)) {
    LOG(INFO) << "holder is in the to-space.";
  } else if (region_space_->IsInUnevacFromSpace(obj)) {
    LOG(INFO) << "holder is in the unevac from-space.";
    if (region_space_bitmap_->Test(obj)) {
      LOG(INFO) << "holder is marked in the region space bitmap.";
    } else {
      LOG(INFO) << "holder is not marked in the region space bitmap.";
    }
  } else {
    // In a non-moving space.
    if (immune_spaces_.ContainsObject(obj)) {
      LOG(INFO) << "holder is in an immune image or the zygote space.";
      accounting::ContinuousSpaceBitmap* cc_bitmap =
          cc_heap_bitmap_->GetContinuousSpaceBitmap(obj);
      CHECK(cc_bitmap != nullptr)
          << "An immune space object must have a bitmap.";
      if (cc_bitmap->Test(obj)) {
        LOG(INFO) << "holder is marked in the bit map.";
      } else {
        LOG(INFO) << "holder is NOT marked in the bit map.";
      }
    } else {
      LOG(INFO) << "holder is in a non-immune, non-moving (or main) space.";
      accounting::ContinuousSpaceBitmap* mark_bitmap =
          heap_mark_bitmap_->GetContinuousSpaceBitmap(obj);
      accounting::LargeObjectBitmap* los_bitmap =
          heap_mark_bitmap_->GetLargeObjectBitmap(obj);
      CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
      bool is_los = mark_bitmap == nullptr;
      if (!is_los && mark_bitmap->Test(obj)) {
        LOG(INFO) << "holder is marked in the mark bit map.";
      } else if (is_los && los_bitmap->Test(obj)) {
        LOG(INFO) << "holder is marked in the los bit map.";
      } else {
        // If ref is on the allocation stack, then it is considered
        // mark/alive (but not necessarily on the live stack.)
        if (IsOnAllocStack(obj)) {
          LOG(INFO) << "holder is on the alloc stack.";
        } else {
          LOG(INFO) << "holder is not marked or on the alloc stack.";
        }
      }
    }
  }
  LOG(INFO) << "offset=" << offset.SizeValue();
}

void ConcurrentCopying::AssertToSpaceInvariantInNonMovingSpace(mirror::Object* obj,
                                                               mirror::Object* ref) {
  // In a non-moving spaces. Check that the ref is marked.
  if (immune_spaces_.ContainsObject(ref)) {
    accounting::ContinuousSpaceBitmap* cc_bitmap =
        cc_heap_bitmap_->GetContinuousSpaceBitmap(ref);
    CHECK(cc_bitmap != nullptr)
        << "An immune space ref must have a bitmap. " << ref;
    if (kUseBakerReadBarrier) {
      CHECK(cc_bitmap->Test(ref))
          << "Unmarked immune space ref. obj=" << obj << " rb_ptr="
          << obj->GetReadBarrierPointer() << " ref=" << ref;
    } else {
      CHECK(cc_bitmap->Test(ref))
          << "Unmarked immune space ref. obj=" << obj << " ref=" << ref;
    }
  } else {
    accounting::ContinuousSpaceBitmap* mark_bitmap =
        heap_mark_bitmap_->GetContinuousSpaceBitmap(ref);
    accounting::LargeObjectBitmap* los_bitmap =
        heap_mark_bitmap_->GetLargeObjectBitmap(ref);
    CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
    bool is_los = mark_bitmap == nullptr;
    if ((!is_los && mark_bitmap->Test(ref)) ||
        (is_los && los_bitmap->Test(ref))) {
      // OK.
    } else {
      // If ref is on the allocation stack, then it may not be
      // marked live, but considered marked/alive (but not
      // necessarily on the live stack).
      CHECK(IsOnAllocStack(ref)) << "Unmarked ref that's not on the allocation stack. "
                                 << "obj=" << obj << " ref=" << ref;
    }
  }
}

// Used to scan ref fields of an object.
class ConcurrentCopying::RefFieldsVisitor {
 public:
  explicit RefFieldsVisitor(ConcurrentCopying* collector)
      : collector_(collector) {}

  void operator()(mirror::Object* obj, MemberOffset offset, bool /* is_static */)
      const ALWAYS_INLINE SHARED_REQUIRES(Locks::mutator_lock_)
      SHARED_REQUIRES(Locks::heap_bitmap_lock_) {
    collector_->Process(obj, offset);
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE {
    CHECK(klass->IsTypeOfReferenceClass());
    collector_->DelayReferenceReferent(klass, ref);
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      ALWAYS_INLINE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      ALWAYS_INLINE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    collector_->MarkRoot(root);
  }

 private:
  ConcurrentCopying* const collector_;
};

// Scan ref fields of an object.
inline void ConcurrentCopying::Scan(mirror::Object* to_ref) {
  DCHECK(!region_space_->IsInFromSpace(to_ref));
  RefFieldsVisitor visitor(this);
  // Disable the read barrier for a performance reason.
  to_ref->VisitReferences</*kVisitNativeRoots*/true, kDefaultVerifyFlags, kWithoutReadBarrier>(
      visitor, visitor);
}

// Process a field.
inline void ConcurrentCopying::Process(mirror::Object* obj, MemberOffset offset) {
  mirror::Object* ref = obj->GetFieldObject<
      mirror::Object, kVerifyNone, kWithoutReadBarrier, false>(offset);
  mirror::Object* to_ref = Mark(ref);
  if (to_ref == ref) {
    return;
  }
  // This may fail if the mutator writes to the field at the same time. But it's ok.
  mirror::Object* expected_ref = ref;
  mirror::Object* new_ref = to_ref;
  do {
    if (expected_ref !=
        obj->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier, false>(offset)) {
      // It was updated by the mutator.
      break;
    }
  } while (!obj->CasFieldWeakRelaxedObjectWithoutWriteBarrier<
      false, false, kVerifyNone>(offset, expected_ref, new_ref));
}

// Process some roots.
inline void ConcurrentCopying::VisitRoots(
    mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    mirror::Object** root = roots[i];
    mirror::Object* ref = *root;
    mirror::Object* to_ref = Mark(ref);
    if (to_ref == ref) {
      continue;
    }
    Atomic<mirror::Object*>* addr = reinterpret_cast<Atomic<mirror::Object*>*>(root);
    mirror::Object* expected_ref = ref;
    mirror::Object* new_ref = to_ref;
    do {
      if (expected_ref != addr->LoadRelaxed()) {
        // It was updated by the mutator.
        break;
      }
    } while (!addr->CompareExchangeWeakRelaxed(expected_ref, new_ref));
  }
}

inline void ConcurrentCopying::MarkRoot(mirror::CompressedReference<mirror::Object>* root) {
  DCHECK(!root->IsNull());
  mirror::Object* const ref = root->AsMirrorPtr();
  mirror::Object* to_ref = Mark(ref);
  if (to_ref != ref) {
    auto* addr = reinterpret_cast<Atomic<mirror::CompressedReference<mirror::Object>>*>(root);
    auto expected_ref = mirror::CompressedReference<mirror::Object>::FromMirrorPtr(ref);
    auto new_ref = mirror::CompressedReference<mirror::Object>::FromMirrorPtr(to_ref);
    // If the cas fails, then it was updated by the mutator.
    do {
      if (ref != addr->LoadRelaxed().AsMirrorPtr()) {
        // It was updated by the mutator.
        break;
      }
    } while (!addr->CompareExchangeWeakRelaxed(expected_ref, new_ref));
  }
}

inline void ConcurrentCopying::VisitRoots(
    mirror::CompressedReference<mirror::Object>** roots, size_t count,
    const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    mirror::CompressedReference<mirror::Object>* const root = roots[i];
    if (!root->IsNull()) {
      MarkRoot(root);
    }
  }
}

// Fill the given memory block with a dummy object. Used to fill in a
// copy of objects that was lost in race.
void ConcurrentCopying::FillWithDummyObject(mirror::Object* dummy_obj, size_t byte_size) {
  CHECK_ALIGNED(byte_size, kObjectAlignment);
  memset(dummy_obj, 0, byte_size);
  mirror::Class* int_array_class = mirror::IntArray::GetArrayClass();
  CHECK(int_array_class != nullptr);
  AssertToSpaceInvariant(nullptr, MemberOffset(0), int_array_class);
  size_t component_size = int_array_class->GetComponentSize();
  CHECK_EQ(component_size, sizeof(int32_t));
  size_t data_offset = mirror::Array::DataOffset(component_size).SizeValue();
  if (data_offset > byte_size) {
    // An int array is too big. Use java.lang.Object.
    mirror::Class* java_lang_Object = WellKnownClasses::ToClass(WellKnownClasses::java_lang_Object);
    AssertToSpaceInvariant(nullptr, MemberOffset(0), java_lang_Object);
    CHECK_EQ(byte_size, java_lang_Object->GetObjectSize());
    dummy_obj->SetClass(java_lang_Object);
    CHECK_EQ(byte_size, dummy_obj->SizeOf());
  } else {
    // Use an int array.
    dummy_obj->SetClass(int_array_class);
    CHECK(dummy_obj->IsArrayInstance());
    int32_t length = (byte_size - data_offset) / component_size;
    dummy_obj->AsArray()->SetLength(length);
    CHECK_EQ(dummy_obj->AsArray()->GetLength(), length)
        << "byte_size=" << byte_size << " length=" << length
        << " component_size=" << component_size << " data_offset=" << data_offset;
    CHECK_EQ(byte_size, dummy_obj->SizeOf())
        << "byte_size=" << byte_size << " length=" << length
        << " component_size=" << component_size << " data_offset=" << data_offset;
  }
}

// Reuse the memory blocks that were copy of objects that were lost in race.
mirror::Object* ConcurrentCopying::AllocateInSkippedBlock(size_t alloc_size) {
  // Try to reuse the blocks that were unused due to CAS failures.
  CHECK_ALIGNED(alloc_size, space::RegionSpace::kAlignment);
  Thread* self = Thread::Current();
  size_t min_object_size = RoundUp(sizeof(mirror::Object), space::RegionSpace::kAlignment);
  MutexLock mu(self, skipped_blocks_lock_);
  auto it = skipped_blocks_map_.lower_bound(alloc_size);
  if (it == skipped_blocks_map_.end()) {
    // Not found.
    return nullptr;
  }
  {
    size_t byte_size = it->first;
    CHECK_GE(byte_size, alloc_size);
    if (byte_size > alloc_size && byte_size - alloc_size < min_object_size) {
      // If remainder would be too small for a dummy object, retry with a larger request size.
      it = skipped_blocks_map_.lower_bound(alloc_size + min_object_size);
      if (it == skipped_blocks_map_.end()) {
        // Not found.
        return nullptr;
      }
      CHECK_ALIGNED(it->first - alloc_size, space::RegionSpace::kAlignment);
      CHECK_GE(it->first - alloc_size, min_object_size)
          << "byte_size=" << byte_size << " it->first=" << it->first << " alloc_size=" << alloc_size;
    }
  }
  // Found a block.
  CHECK(it != skipped_blocks_map_.end());
  size_t byte_size = it->first;
  uint8_t* addr = it->second;
  CHECK_GE(byte_size, alloc_size);
  CHECK(region_space_->IsInToSpace(reinterpret_cast<mirror::Object*>(addr)));
  CHECK_ALIGNED(byte_size, space::RegionSpace::kAlignment);
  if (kVerboseMode) {
    LOG(INFO) << "Reusing skipped bytes : " << reinterpret_cast<void*>(addr) << ", " << byte_size;
  }
  skipped_blocks_map_.erase(it);
  memset(addr, 0, byte_size);
  if (byte_size > alloc_size) {
    // Return the remainder to the map.
    CHECK_ALIGNED(byte_size - alloc_size, space::RegionSpace::kAlignment);
    CHECK_GE(byte_size - alloc_size, min_object_size);
    FillWithDummyObject(reinterpret_cast<mirror::Object*>(addr + alloc_size),
                        byte_size - alloc_size);
    CHECK(region_space_->IsInToSpace(reinterpret_cast<mirror::Object*>(addr + alloc_size)));
    skipped_blocks_map_.insert(std::make_pair(byte_size - alloc_size, addr + alloc_size));
  }
  return reinterpret_cast<mirror::Object*>(addr);
}

mirror::Object* ConcurrentCopying::Copy(mirror::Object* from_ref) {
  DCHECK(region_space_->IsInFromSpace(from_ref));
  // No read barrier to avoid nested RB that might violate the to-space
  // invariant. Note that from_ref is a from space ref so the SizeOf()
  // call will access the from-space meta objects, but it's ok and necessary.
  size_t obj_size = from_ref->SizeOf<kDefaultVerifyFlags, kWithoutReadBarrier>();
  size_t region_space_alloc_size = RoundUp(obj_size, space::RegionSpace::kAlignment);
  size_t region_space_bytes_allocated = 0U;
  size_t non_moving_space_bytes_allocated = 0U;
  size_t bytes_allocated = 0U;
  size_t dummy;
  mirror::Object* to_ref = region_space_->AllocNonvirtual<true>(
      region_space_alloc_size, &region_space_bytes_allocated, nullptr, &dummy);
  bytes_allocated = region_space_bytes_allocated;
  if (to_ref != nullptr) {
    DCHECK_EQ(region_space_alloc_size, region_space_bytes_allocated);
  }
  bool fall_back_to_non_moving = false;
  if (UNLIKELY(to_ref == nullptr)) {
    // Failed to allocate in the region space. Try the skipped blocks.
    to_ref = AllocateInSkippedBlock(region_space_alloc_size);
    if (to_ref != nullptr) {
      // Succeeded to allocate in a skipped block.
      if (heap_->use_tlab_) {
        // This is necessary for the tlab case as it's not accounted in the space.
        region_space_->RecordAlloc(to_ref);
      }
      bytes_allocated = region_space_alloc_size;
    } else {
      // Fall back to the non-moving space.
      fall_back_to_non_moving = true;
      if (kVerboseMode) {
        LOG(INFO) << "Out of memory in the to-space. Fall back to non-moving. skipped_bytes="
                  << to_space_bytes_skipped_.LoadSequentiallyConsistent()
                  << " skipped_objects=" << to_space_objects_skipped_.LoadSequentiallyConsistent();
      }
      fall_back_to_non_moving = true;
      to_ref = heap_->non_moving_space_->Alloc(Thread::Current(), obj_size,
                                               &non_moving_space_bytes_allocated, nullptr, &dummy);
      CHECK(to_ref != nullptr) << "Fall-back non-moving space allocation failed";
      bytes_allocated = non_moving_space_bytes_allocated;
      // Mark it in the mark bitmap.
      accounting::ContinuousSpaceBitmap* mark_bitmap =
          heap_mark_bitmap_->GetContinuousSpaceBitmap(to_ref);
      CHECK(mark_bitmap != nullptr);
      CHECK(!mark_bitmap->AtomicTestAndSet(to_ref));
    }
  }
  DCHECK(to_ref != nullptr);

  // Attempt to install the forward pointer. This is in a loop as the
  // lock word atomic write can fail.
  while (true) {
    // Copy the object. TODO: copy only the lockword in the second iteration and on?
    memcpy(to_ref, from_ref, obj_size);

    LockWord old_lock_word = to_ref->GetLockWord(false);

    if (old_lock_word.GetState() == LockWord::kForwardingAddress) {
      // Lost the race. Another thread (either GC or mutator) stored
      // the forwarding pointer first. Make the lost copy (to_ref)
      // look like a valid but dead (dummy) object and keep it for
      // future reuse.
      FillWithDummyObject(to_ref, bytes_allocated);
      if (!fall_back_to_non_moving) {
        DCHECK(region_space_->IsInToSpace(to_ref));
        if (bytes_allocated > space::RegionSpace::kRegionSize) {
          // Free the large alloc.
          region_space_->FreeLarge(to_ref, bytes_allocated);
        } else {
          // Record the lost copy for later reuse.
          heap_->num_bytes_allocated_.FetchAndAddSequentiallyConsistent(bytes_allocated);
          to_space_bytes_skipped_.FetchAndAddSequentiallyConsistent(bytes_allocated);
          to_space_objects_skipped_.FetchAndAddSequentiallyConsistent(1);
          MutexLock mu(Thread::Current(), skipped_blocks_lock_);
          skipped_blocks_map_.insert(std::make_pair(bytes_allocated,
                                                    reinterpret_cast<uint8_t*>(to_ref)));
        }
      } else {
        DCHECK(heap_->non_moving_space_->HasAddress(to_ref));
        DCHECK_EQ(bytes_allocated, non_moving_space_bytes_allocated);
        // Free the non-moving-space chunk.
        accounting::ContinuousSpaceBitmap* mark_bitmap =
            heap_mark_bitmap_->GetContinuousSpaceBitmap(to_ref);
        CHECK(mark_bitmap != nullptr);
        CHECK(mark_bitmap->Clear(to_ref));
        heap_->non_moving_space_->Free(Thread::Current(), to_ref);
      }

      // Get the winner's forward ptr.
      mirror::Object* lost_fwd_ptr = to_ref;
      to_ref = reinterpret_cast<mirror::Object*>(old_lock_word.ForwardingAddress());
      CHECK(to_ref != nullptr);
      CHECK_NE(to_ref, lost_fwd_ptr);
      CHECK(region_space_->IsInToSpace(to_ref) || heap_->non_moving_space_->HasAddress(to_ref));
      CHECK_NE(to_ref->GetLockWord(false).GetState(), LockWord::kForwardingAddress);
      return to_ref;
    }

    // Set the gray ptr.
    if (kUseBakerReadBarrier) {
      to_ref->SetReadBarrierPointer(ReadBarrier::GrayPtr());
    }

    LockWord new_lock_word = LockWord::FromForwardingAddress(reinterpret_cast<size_t>(to_ref));

    // Try to atomically write the fwd ptr.
    bool success = from_ref->CasLockWordWeakSequentiallyConsistent(old_lock_word, new_lock_word);
    if (LIKELY(success)) {
      // The CAS succeeded.
      objects_moved_.FetchAndAddSequentiallyConsistent(1);
      bytes_moved_.FetchAndAddSequentiallyConsistent(region_space_alloc_size);
      if (LIKELY(!fall_back_to_non_moving)) {
        DCHECK(region_space_->IsInToSpace(to_ref));
      } else {
        DCHECK(heap_->non_moving_space_->HasAddress(to_ref));
        DCHECK_EQ(bytes_allocated, non_moving_space_bytes_allocated);
      }
      if (kUseBakerReadBarrier) {
        DCHECK(to_ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr());
      }
      DCHECK(GetFwdPtr(from_ref) == to_ref);
      CHECK_NE(to_ref->GetLockWord(false).GetState(), LockWord::kForwardingAddress);
      PushOntoMarkStack(to_ref);
      return to_ref;
    } else {
      // The CAS failed. It may have lost the race or may have failed
      // due to monitor/hashcode ops. Either way, retry.
    }
  }
}

mirror::Object* ConcurrentCopying::IsMarked(mirror::Object* from_ref) {
  DCHECK(from_ref != nullptr);
  space::RegionSpace::RegionType rtype = region_space_->GetRegionType(from_ref);
  if (rtype == space::RegionSpace::RegionType::kRegionTypeToSpace) {
    // It's already marked.
    return from_ref;
  }
  mirror::Object* to_ref;
  if (rtype == space::RegionSpace::RegionType::kRegionTypeFromSpace) {
    to_ref = GetFwdPtr(from_ref);
    DCHECK(to_ref == nullptr || region_space_->IsInToSpace(to_ref) ||
           heap_->non_moving_space_->HasAddress(to_ref))
        << "from_ref=" << from_ref << " to_ref=" << to_ref;
  } else if (rtype == space::RegionSpace::RegionType::kRegionTypeUnevacFromSpace) {
    if (region_space_bitmap_->Test(from_ref)) {
      to_ref = from_ref;
    } else {
      to_ref = nullptr;
    }
  } else {
    // from_ref is in a non-moving space.
    if (immune_spaces_.ContainsObject(from_ref)) {
      accounting::ContinuousSpaceBitmap* cc_bitmap =
          cc_heap_bitmap_->GetContinuousSpaceBitmap(from_ref);
      DCHECK(cc_bitmap != nullptr)
          << "An immune space object must have a bitmap";
      if (kIsDebugBuild) {
        DCHECK(heap_mark_bitmap_->GetContinuousSpaceBitmap(from_ref)->Test(from_ref))
            << "Immune space object must be already marked";
      }
      if (cc_bitmap->Test(from_ref)) {
        // Already marked.
        to_ref = from_ref;
      } else {
        // Newly marked.
        to_ref = nullptr;
      }
    } else {
      // Non-immune non-moving space. Use the mark bitmap.
      accounting::ContinuousSpaceBitmap* mark_bitmap =
          heap_mark_bitmap_->GetContinuousSpaceBitmap(from_ref);
      accounting::LargeObjectBitmap* los_bitmap =
          heap_mark_bitmap_->GetLargeObjectBitmap(from_ref);
      CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
      bool is_los = mark_bitmap == nullptr;
      if (!is_los && mark_bitmap->Test(from_ref)) {
        // Already marked.
        to_ref = from_ref;
      } else if (is_los && los_bitmap->Test(from_ref)) {
        // Already marked in LOS.
        to_ref = from_ref;
      } else {
        // Not marked.
        if (IsOnAllocStack(from_ref)) {
          // If on the allocation stack, it's considered marked.
          to_ref = from_ref;
        } else {
          // Not marked.
          to_ref = nullptr;
        }
      }
    }
  }
  return to_ref;
}

bool ConcurrentCopying::IsOnAllocStack(mirror::Object* ref) {
  QuasiAtomic::ThreadFenceAcquire();
  accounting::ObjectStack* alloc_stack = GetAllocationStack();
  return alloc_stack->Contains(ref);
}

mirror::Object* ConcurrentCopying::MarkNonMoving(mirror::Object* ref) {
  // ref is in a non-moving space (from_ref == to_ref).
  DCHECK(!region_space_->HasAddress(ref)) << ref;
  if (immune_spaces_.ContainsObject(ref)) {
    accounting::ContinuousSpaceBitmap* cc_bitmap =
        cc_heap_bitmap_->GetContinuousSpaceBitmap(ref);
    DCHECK(cc_bitmap != nullptr)
        << "An immune space object must have a bitmap";
    if (kIsDebugBuild) {
      DCHECK(heap_mark_bitmap_->GetContinuousSpaceBitmap(ref)->Test(ref))
          << "Immune space object must be already marked";
    }
    // This may or may not succeed, which is ok.
    if (kUseBakerReadBarrier) {
      ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
    }
    if (cc_bitmap->AtomicTestAndSet(ref)) {
      // Already marked.
    } else {
      // Newly marked.
      if (kUseBakerReadBarrier) {
        DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::GrayPtr());
      }
      PushOntoMarkStack(ref);
    }
  } else {
    // Use the mark bitmap.
    accounting::ContinuousSpaceBitmap* mark_bitmap =
        heap_mark_bitmap_->GetContinuousSpaceBitmap(ref);
    accounting::LargeObjectBitmap* los_bitmap =
        heap_mark_bitmap_->GetLargeObjectBitmap(ref);
    CHECK(los_bitmap != nullptr) << "LOS bitmap covers the entire address range";
    bool is_los = mark_bitmap == nullptr;
    if (!is_los && mark_bitmap->Test(ref)) {
      // Already marked.
      if (kUseBakerReadBarrier) {
        DCHECK(ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr() ||
               ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr());
      }
    } else if (is_los && los_bitmap->Test(ref)) {
      // Already marked in LOS.
      if (kUseBakerReadBarrier) {
        DCHECK(ref->GetReadBarrierPointer() == ReadBarrier::GrayPtr() ||
               ref->GetReadBarrierPointer() == ReadBarrier::BlackPtr());
      }
    } else {
      // Not marked.
      if (IsOnAllocStack(ref)) {
        // If it's on the allocation stack, it's considered marked. Keep it white.
        // Objects on the allocation stack need not be marked.
        if (!is_los) {
          DCHECK(!mark_bitmap->Test(ref));
        } else {
          DCHECK(!los_bitmap->Test(ref));
        }
        if (kUseBakerReadBarrier) {
          DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::WhitePtr());
        }
      } else {
        // Not marked or on the allocation stack. Try to mark it.
        // This may or may not succeed, which is ok.
        if (kUseBakerReadBarrier) {
          ref->AtomicSetReadBarrierPointer(ReadBarrier::WhitePtr(), ReadBarrier::GrayPtr());
        }
        if (!is_los && mark_bitmap->AtomicTestAndSet(ref)) {
          // Already marked.
        } else if (is_los && los_bitmap->AtomicTestAndSet(ref)) {
          // Already marked in LOS.
        } else {
          // Newly marked.
          if (kUseBakerReadBarrier) {
            DCHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::GrayPtr());
          }
          PushOntoMarkStack(ref);
        }
      }
    }
  }
  return ref;
}

void ConcurrentCopying::FinishPhase() {
  Thread* const self = Thread::Current();
  {
    MutexLock mu(self, mark_stack_lock_);
    CHECK_EQ(pooled_mark_stacks_.size(), kMarkStackPoolSize);
  }
  region_space_ = nullptr;
  {
    MutexLock mu(Thread::Current(), skipped_blocks_lock_);
    skipped_blocks_map_.clear();
  }
  ReaderMutexLock mu(self, *Locks::mutator_lock_);
  WriterMutexLock mu2(self, *Locks::heap_bitmap_lock_);
  heap_->ClearMarkedObjects();
}

bool ConcurrentCopying::IsMarkedHeapReference(mirror::HeapReference<mirror::Object>* field) {
  mirror::Object* from_ref = field->AsMirrorPtr();
  mirror::Object* to_ref = IsMarked(from_ref);
  if (to_ref == nullptr) {
    return false;
  }
  if (from_ref != to_ref) {
    QuasiAtomic::ThreadFenceRelease();
    field->Assign(to_ref);
    QuasiAtomic::ThreadFenceSequentiallyConsistent();
  }
  return true;
}

mirror::Object* ConcurrentCopying::MarkObject(mirror::Object* from_ref) {
  return Mark(from_ref);
}

void ConcurrentCopying::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* reference) {
  heap_->GetReferenceProcessor()->DelayReferenceReferent(klass, reference, this);
}

void ConcurrentCopying::ProcessReferences(Thread* self) {
  TimingLogger::ScopedTiming split("ProcessReferences", GetTimings());
  // We don't really need to lock the heap bitmap lock as we use CAS to mark in bitmaps.
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->GetReferenceProcessor()->ProcessReferences(
      true /*concurrent*/, GetTimings(), GetCurrentIteration()->GetClearSoftReferences(), this);
}

void ConcurrentCopying::RevokeAllThreadLocalBuffers() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  region_space_->RevokeAllThreadLocalBuffers();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
