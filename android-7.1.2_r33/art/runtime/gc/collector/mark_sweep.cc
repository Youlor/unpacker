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

#include "mark_sweep.h"

#include <atomic>
#include <functional>
#include <numeric>
#include <climits>
#include <vector>

#include "base/bounded_fifo.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap-inl.h"
#include "gc/accounting/mod_union_table.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/reference_processor.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "mark_sweep-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {
namespace gc {
namespace collector {

// Performance options.
static constexpr bool kUseRecursiveMark = false;
static constexpr bool kUseMarkStackPrefetch = true;
static constexpr size_t kSweepArrayChunkFreeSize = 1024;
static constexpr bool kPreCleanCards = true;

// Parallelism options.
static constexpr bool kParallelCardScan = true;
static constexpr bool kParallelRecursiveMark = true;
// Don't attempt to parallelize mark stack processing unless the mark stack is at least n
// elements. This is temporary until we reduce the overhead caused by allocating tasks, etc.. Not
// having this can add overhead in ProcessReferences since we may end up doing many calls of
// ProcessMarkStack with very small mark stacks.
static constexpr size_t kMinimumParallelMarkStackSize = 128;
static constexpr bool kParallelProcessMarkStack = true;

// Profiling and information flags.
static constexpr bool kProfileLargeObjects = false;
static constexpr bool kMeasureOverhead = false;
static constexpr bool kCountTasks = false;
static constexpr bool kCountMarkedObjects = false;

// Turn off kCheckLocks when profiling the GC since it slows the GC down by up to 40%.
static constexpr bool kCheckLocks = kDebugLocking;
static constexpr bool kVerifyRootsMarked = kIsDebugBuild;

// If true, revoke the rosalloc thread-local buffers at the
// checkpoint, as opposed to during the pause.
static constexpr bool kRevokeRosAllocThreadLocalBuffersAtCheckpoint = true;

void MarkSweep::BindBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
  // Mark all of the spaces we never collect as immune.
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyNeverCollect) {
      immune_spaces_.AddSpace(space);
    }
  }
}

MarkSweep::MarkSweep(Heap* heap, bool is_concurrent, const std::string& name_prefix)
    : GarbageCollector(heap,
                       name_prefix +
                       (is_concurrent ? "concurrent mark sweep": "mark sweep")),
      current_space_bitmap_(nullptr),
      mark_bitmap_(nullptr),
      mark_stack_(nullptr),
      gc_barrier_(new Barrier(0)),
      mark_stack_lock_("mark sweep mark stack lock", kMarkSweepMarkStackLock),
      is_concurrent_(is_concurrent),
      live_stack_freeze_size_(0) {
  std::string error_msg;
  MemMap* mem_map = MemMap::MapAnonymous(
      "mark sweep sweep array free buffer", nullptr,
      RoundUp(kSweepArrayChunkFreeSize * sizeof(mirror::Object*), kPageSize),
      PROT_READ | PROT_WRITE, false, false, &error_msg);
  CHECK(mem_map != nullptr) << "Couldn't allocate sweep array free buffer: " << error_msg;
  sweep_array_free_buffer_mem_map_.reset(mem_map);
}

void MarkSweep::InitializePhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  mark_stack_ = heap_->GetMarkStack();
  DCHECK(mark_stack_ != nullptr);
  immune_spaces_.Reset();
  no_reference_class_count_.StoreRelaxed(0);
  normal_count_.StoreRelaxed(0);
  class_count_.StoreRelaxed(0);
  object_array_count_.StoreRelaxed(0);
  other_count_.StoreRelaxed(0);
  reference_count_.StoreRelaxed(0);
  large_object_test_.StoreRelaxed(0);
  large_object_mark_.StoreRelaxed(0);
  overhead_time_ .StoreRelaxed(0);
  work_chunks_created_.StoreRelaxed(0);
  work_chunks_deleted_.StoreRelaxed(0);
  mark_null_count_.StoreRelaxed(0);
  mark_immune_count_.StoreRelaxed(0);
  mark_fastpath_count_.StoreRelaxed(0);
  mark_slowpath_count_.StoreRelaxed(0);
  {
    // TODO: I don't think we should need heap bitmap lock to Get the mark bitmap.
    ReaderMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    mark_bitmap_ = heap_->GetMarkBitmap();
  }
  if (!GetCurrentIteration()->GetClearSoftReferences()) {
    // Always clear soft references if a non-sticky collection.
    GetCurrentIteration()->SetClearSoftReferences(GetGcType() != collector::kGcTypeSticky);
  }
}

void MarkSweep::RunPhases() {
  Thread* self = Thread::Current();
  InitializePhase();
  Locks::mutator_lock_->AssertNotHeld(self);
  if (IsConcurrent()) {
    GetHeap()->PreGcVerification(this);
    {
      ReaderMutexLock mu(self, *Locks::mutator_lock_);
      MarkingPhase();
    }
    ScopedPause pause(this);
    GetHeap()->PrePauseRosAllocVerification(this);
    PausePhase();
    RevokeAllThreadLocalBuffers();
  } else {
    ScopedPause pause(this);
    GetHeap()->PreGcVerificationPaused(this);
    MarkingPhase();
    GetHeap()->PrePauseRosAllocVerification(this);
    PausePhase();
    RevokeAllThreadLocalBuffers();
  }
  {
    // Sweeping always done concurrently, even for non concurrent mark sweep.
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    ReclaimPhase();
  }
  GetHeap()->PostGcVerification(this);
  FinishPhase();
}

void MarkSweep::ProcessReferences(Thread* self) {
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  GetHeap()->GetReferenceProcessor()->ProcessReferences(
      true,
      GetTimings(),
      GetCurrentIteration()->GetClearSoftReferences(),
      this);
}

void MarkSweep::PausePhase() {
  TimingLogger::ScopedTiming t("(Paused)PausePhase", GetTimings());
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertExclusiveHeld(self);
  if (IsConcurrent()) {
    // Handle the dirty objects if we are a concurrent GC.
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Re-mark root set.
    ReMarkRoots();
    // Scan dirty objects, this is only required if we are not doing concurrent GC.
    RecursiveMarkDirtyObjects(true, accounting::CardTable::kCardDirty);
  }
  {
    TimingLogger::ScopedTiming t2("SwapStacks", GetTimings());
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap_->SwapStacks();
    live_stack_freeze_size_ = heap_->GetLiveStack()->Size();
    // Need to revoke all the thread local allocation stacks since we just swapped the allocation
    // stacks and don't want anybody to allocate into the live stack.
    RevokeAllThreadLocalAllocationStacks(self);
  }
  heap_->PreSweepingGcVerification(this);
  // Disallow new system weaks to prevent a race which occurs when someone adds a new system
  // weak before we sweep them. Since this new system weak may not be marked, the GC may
  // incorrectly sweep it. This also fixes a race where interning may attempt to return a strong
  // reference to a string that is about to be swept.
  Runtime::Current()->DisallowNewSystemWeaks();
  // Enable the reference processing slow path, needs to be done with mutators paused since there
  // is no lock in the GetReferent fast path.
  GetHeap()->GetReferenceProcessor()->EnableSlowPath();
}

void MarkSweep::PreCleanCards() {
  // Don't do this for non concurrent GCs since they don't have any dirty cards.
  if (kPreCleanCards && IsConcurrent()) {
    TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
    Thread* self = Thread::Current();
    CHECK(!Locks::mutator_lock_->IsExclusiveHeld(self));
    // Process dirty cards and add dirty cards to mod union tables, also ages cards.
    heap_->ProcessCards(GetTimings(), false, true, false);
    // The checkpoint root marking is required to avoid a race condition which occurs if the
    // following happens during a reference write:
    // 1. mutator dirties the card (write barrier)
    // 2. GC ages the card (the above ProcessCards call)
    // 3. GC scans the object (the RecursiveMarkDirtyObjects call below)
    // 4. mutator writes the value (corresponding to the write barrier in 1.)
    // This causes the GC to age the card but not necessarily mark the reference which the mutator
    // wrote into the object stored in the card.
    // Having the checkpoint fixes this issue since it ensures that the card mark and the
    // reference write are visible to the GC before the card is scanned (this is due to locks being
    // acquired / released in the checkpoint code).
    // The other roots are also marked to help reduce the pause.
    MarkRootsCheckpoint(self, false);
    MarkNonThreadRoots();
    MarkConcurrentRoots(
        static_cast<VisitRootFlags>(kVisitRootFlagClearRootLog | kVisitRootFlagNewRoots));
    // Process the newly aged cards.
    RecursiveMarkDirtyObjects(false, accounting::CardTable::kCardDirty - 1);
    // TODO: Empty allocation stack to reduce the number of objects we need to test / mark as live
    // in the next GC.
  }
}

void MarkSweep::RevokeAllThreadLocalAllocationStacks(Thread* self) {
  if (kUseThreadLocalAllocationStack) {
    TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
    Locks::mutator_lock_->AssertExclusiveHeld(self);
    heap_->RevokeAllThreadLocalAllocationStacks(self);
  }
}

void MarkSweep::MarkingPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Thread* self = Thread::Current();
  BindBitmaps();
  FindDefaultSpaceBitmap();
  // Process dirty cards and add dirty cards to mod union tables.
  // If the GC type is non sticky, then we just clear the cards instead of ageing them.
  heap_->ProcessCards(GetTimings(), false, true, GetGcType() != kGcTypeSticky);
  WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
  MarkRoots(self);
  MarkReachableObjects();
  // Pre-clean dirtied cards to reduce pauses.
  PreCleanCards();
}

class MarkSweep::ScanObjectVisitor {
 public:
  explicit ScanObjectVisitor(MarkSweep* const mark_sweep) ALWAYS_INLINE
      : mark_sweep_(mark_sweep) {}

  void operator()(mirror::Object* obj) const
      ALWAYS_INLINE
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->ScanObject(obj);
  }

 private:
  MarkSweep* const mark_sweep_;
};

void MarkSweep::UpdateAndMarkModUnion() {
  for (const auto& space : immune_spaces_.GetSpaces()) {
    const char* name = space->IsZygoteSpace()
        ? "UpdateAndMarkZygoteModUnionTable"
        : "UpdateAndMarkImageModUnionTable";
    DCHECK(space->IsZygoteSpace() || space->IsImageSpace()) << *space;
    TimingLogger::ScopedTiming t(name, GetTimings());
    accounting::ModUnionTable* mod_union_table = heap_->FindModUnionTableFromSpace(space);
    if (mod_union_table != nullptr) {
      mod_union_table->UpdateAndMarkReferences(this);
    } else {
      // No mod-union table, scan all the live bits. This can only occur for app images.
      space->GetLiveBitmap()->VisitMarkedRange(reinterpret_cast<uintptr_t>(space->Begin()),
                                               reinterpret_cast<uintptr_t>(space->End()),
                                               ScanObjectVisitor(this));
    }
  }
}

void MarkSweep::MarkReachableObjects() {
  UpdateAndMarkModUnion();
  // Recursively mark all the non-image bits set in the mark bitmap.
  RecursiveMark();
}

void MarkSweep::ReclaimPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Thread* const self = Thread::Current();
  // Process the references concurrently.
  ProcessReferences(self);
  SweepSystemWeaks(self);
  Runtime* const runtime = Runtime::Current();
  runtime->AllowNewSystemWeaks();
  // Clean up class loaders after system weaks are swept since that is how we know if class
  // unloading occurred.
  runtime->GetClassLinker()->CleanupClassLoaders();
  {
    WriterMutexLock mu(self, *Locks::heap_bitmap_lock_);
    GetHeap()->RecordFreeRevoke();
    // Reclaim unmarked objects.
    Sweep(false);
    // Swap the live and mark bitmaps for each space which we modified space. This is an
    // optimization that enables us to not clear live bits inside of the sweep. Only swaps unbound
    // bitmaps.
    SwapBitmaps();
    // Unbind the live and mark bitmaps.
    GetHeap()->UnBindBitmaps();
  }
}

void MarkSweep::FindDefaultSpaceBitmap() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    accounting::ContinuousSpaceBitmap* bitmap = space->GetMarkBitmap();
    // We want to have the main space instead of non moving if possible.
    if (bitmap != nullptr &&
        space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) {
      current_space_bitmap_ = bitmap;
      // If we are not the non moving space exit the loop early since this will be good enough.
      if (space != heap_->GetNonMovingSpace()) {
        break;
      }
    }
  }
  CHECK(current_space_bitmap_ != nullptr) << "Could not find a default mark bitmap\n"
      << heap_->DumpSpaces();
}

void MarkSweep::ExpandMarkStack() {
  ResizeMarkStack(mark_stack_->Capacity() * 2);
}

void MarkSweep::ResizeMarkStack(size_t new_size) {
  // Rare case, no need to have Thread::Current be a parameter.
  if (UNLIKELY(mark_stack_->Size() < mark_stack_->Capacity())) {
    // Someone else acquired the lock and expanded the mark stack before us.
    return;
  }
  std::vector<StackReference<mirror::Object>> temp(mark_stack_->Begin(), mark_stack_->End());
  CHECK_LE(mark_stack_->Size(), new_size);
  mark_stack_->Resize(new_size);
  for (auto& obj : temp) {
    mark_stack_->PushBack(obj.AsMirrorPtr());
  }
}

mirror::Object* MarkSweep::MarkObject(mirror::Object* obj) {
  MarkObject(obj, nullptr, MemberOffset(0));
  return obj;
}

inline void MarkSweep::MarkObjectNonNullParallel(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  if (MarkObjectParallel(obj)) {
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
      ExpandMarkStack();
    }
    // The object must be pushed on to the mark stack.
    mark_stack_->PushBack(obj);
  }
}

bool MarkSweep::IsMarkedHeapReference(mirror::HeapReference<mirror::Object>* ref) {
  return IsMarked(ref->AsMirrorPtr());
}

class MarkSweep::MarkObjectSlowPath {
 public:
  explicit MarkObjectSlowPath(MarkSweep* mark_sweep,
                              mirror::Object* holder = nullptr,
                              MemberOffset offset = MemberOffset(0))
      : mark_sweep_(mark_sweep),
        holder_(holder),
        offset_(offset) {}

  void operator()(const mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    if (kProfileLargeObjects) {
      // TODO: Differentiate between marking and testing somehow.
      ++mark_sweep_->large_object_test_;
      ++mark_sweep_->large_object_mark_;
    }
    space::LargeObjectSpace* large_object_space = mark_sweep_->GetHeap()->GetLargeObjectsSpace();
    if (UNLIKELY(obj == nullptr || !IsAligned<kPageSize>(obj) ||
                 (kIsDebugBuild && large_object_space != nullptr &&
                     !large_object_space->Contains(obj)))) {
      LOG(INTERNAL_FATAL) << "Tried to mark " << obj << " not contained by any spaces";
      if (holder_ != nullptr) {
        size_t holder_size = holder_->SizeOf();
        ArtField* field = holder_->FindFieldByOffset(offset_);
        LOG(INTERNAL_FATAL) << "Field info: "
                            << " holder=" << holder_
                            << " holder is "
                            << (mark_sweep_->GetHeap()->IsLiveObjectLocked(holder_)
                                ? "alive" : "dead")
                            << " holder_size=" << holder_size
                            << " holder_type=" << PrettyTypeOf(holder_)
                            << " offset=" << offset_.Uint32Value()
                            << " field=" << (field != nullptr ? field->GetName() : "nullptr")
                            << " field_type="
                            << (field != nullptr ? field->GetTypeDescriptor() : "")
                            << " first_ref_field_offset="
                            << (holder_->IsClass()
                                ? holder_->AsClass()->GetFirstReferenceStaticFieldOffset(
                                    sizeof(void*))
                                : holder_->GetClass()->GetFirstReferenceInstanceFieldOffset())
                            << " num_of_ref_fields="
                            << (holder_->IsClass()
                                ? holder_->AsClass()->NumReferenceStaticFields()
                                : holder_->GetClass()->NumReferenceInstanceFields())
                            << "\n";
        // Print the memory content of the holder.
        for (size_t i = 0; i < holder_size / sizeof(uint32_t); ++i) {
          uint32_t* p = reinterpret_cast<uint32_t*>(holder_);
          LOG(INTERNAL_FATAL) << &p[i] << ": " << "holder+" << (i * sizeof(uint32_t)) << " = "
                              << std::hex << p[i];
        }
      }
      PrintFileToLog("/proc/self/maps", LogSeverity::INTERNAL_FATAL);
      MemMap::DumpMaps(LOG(INTERNAL_FATAL), true);
      {
        LOG(INTERNAL_FATAL) << "Attempting see if it's a bad root";
        Thread* self = Thread::Current();
        if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
          mark_sweep_->VerifyRoots();
        } else {
          const bool heap_bitmap_exclusive_locked =
              Locks::heap_bitmap_lock_->IsExclusiveHeld(self);
          if (heap_bitmap_exclusive_locked) {
            Locks::heap_bitmap_lock_->ExclusiveUnlock(self);
          }
          {
            ScopedThreadSuspension(self, kSuspended);
            ScopedSuspendAll ssa(__FUNCTION__);
            mark_sweep_->VerifyRoots();
          }
          if (heap_bitmap_exclusive_locked) {
            Locks::heap_bitmap_lock_->ExclusiveLock(self);
          }
        }
      }
      LOG(FATAL) << "Can't mark invalid object";
    }
  }

 private:
  MarkSweep* const mark_sweep_;
  mirror::Object* const holder_;
  MemberOffset offset_;
};

inline void MarkSweep::MarkObjectNonNull(mirror::Object* obj,
                                         mirror::Object* holder,
                                         MemberOffset offset) {
  DCHECK(obj != nullptr);
  if (kUseBakerOrBrooksReadBarrier) {
    // Verify all the objects have the correct pointer installed.
    obj->AssertReadBarrierPointer();
  }
  if (immune_spaces_.IsInImmuneRegion(obj)) {
    if (kCountMarkedObjects) {
      ++mark_immune_count_;
    }
    DCHECK(mark_bitmap_->Test(obj));
  } else if (LIKELY(current_space_bitmap_->HasAddress(obj))) {
    if (kCountMarkedObjects) {
      ++mark_fastpath_count_;
    }
    if (UNLIKELY(!current_space_bitmap_->Set(obj))) {
      PushOnMarkStack(obj);  // This object was not previously marked.
    }
  } else {
    if (kCountMarkedObjects) {
      ++mark_slowpath_count_;
    }
    MarkObjectSlowPath visitor(this, holder, offset);
    // TODO: We already know that the object is not in the current_space_bitmap_ but MarkBitmap::Set
    // will check again.
    if (!mark_bitmap_->Set(obj, visitor)) {
      PushOnMarkStack(obj);  // Was not already marked, push.
    }
  }
}

inline void MarkSweep::PushOnMarkStack(mirror::Object* obj) {
  if (UNLIKELY(mark_stack_->Size() >= mark_stack_->Capacity())) {
    // Lock is not needed but is here anyways to please annotalysis.
    MutexLock mu(Thread::Current(), mark_stack_lock_);
    ExpandMarkStack();
  }
  // The object must be pushed on to the mark stack.
  mark_stack_->PushBack(obj);
}

inline bool MarkSweep::MarkObjectParallel(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  if (kUseBakerOrBrooksReadBarrier) {
    // Verify all the objects have the correct pointer installed.
    obj->AssertReadBarrierPointer();
  }
  if (immune_spaces_.IsInImmuneRegion(obj)) {
    DCHECK(IsMarked(obj) != nullptr);
    return false;
  }
  // Try to take advantage of locality of references within a space, failing this find the space
  // the hard way.
  accounting::ContinuousSpaceBitmap* object_bitmap = current_space_bitmap_;
  if (LIKELY(object_bitmap->HasAddress(obj))) {
    return !object_bitmap->AtomicTestAndSet(obj);
  }
  MarkObjectSlowPath visitor(this);
  return !mark_bitmap_->AtomicTestAndSet(obj, visitor);
}

void MarkSweep::MarkHeapReference(mirror::HeapReference<mirror::Object>* ref) {
  MarkObject(ref->AsMirrorPtr(), nullptr, MemberOffset(0));
}

// Used to mark objects when processing the mark stack. If an object is null, it is not marked.
inline void MarkSweep::MarkObject(mirror::Object* obj,
                                  mirror::Object* holder,
                                  MemberOffset offset) {
  if (obj != nullptr) {
    MarkObjectNonNull(obj, holder, offset);
  } else if (kCountMarkedObjects) {
    ++mark_null_count_;
  }
}

class MarkSweep::VerifyRootMarkedVisitor : public SingleRootVisitor {
 public:
  explicit VerifyRootMarkedVisitor(MarkSweep* collector) : collector_(collector) { }

  void VisitRoot(mirror::Object* root, const RootInfo& info) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    CHECK(collector_->IsMarked(root) != nullptr) << info.ToString();
  }

 private:
  MarkSweep* const collector_;
};

void MarkSweep::VisitRoots(mirror::Object*** roots,
                           size_t count,
                           const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    MarkObjectNonNull(*roots[i]);
  }
}

void MarkSweep::VisitRoots(mirror::CompressedReference<mirror::Object>** roots,
                           size_t count,
                           const RootInfo& info ATTRIBUTE_UNUSED) {
  for (size_t i = 0; i < count; ++i) {
    MarkObjectNonNull(roots[i]->AsMirrorPtr());
  }
}

class MarkSweep::VerifyRootVisitor : public SingleRootVisitor {
 public:
  void VisitRoot(mirror::Object* root, const RootInfo& info) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    // See if the root is on any space bitmap.
    auto* heap = Runtime::Current()->GetHeap();
    if (heap->GetLiveBitmap()->GetContinuousSpaceBitmap(root) == nullptr) {
      space::LargeObjectSpace* large_object_space = heap->GetLargeObjectsSpace();
      if (large_object_space != nullptr && !large_object_space->Contains(root)) {
        LOG(INTERNAL_FATAL) << "Found invalid root: " << root << " " << info;
      }
    }
  }
};

void MarkSweep::VerifyRoots() {
  VerifyRootVisitor visitor;
  Runtime::Current()->GetThreadList()->VisitRoots(&visitor);
}

void MarkSweep::MarkRoots(Thread* self) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // If we exclusively hold the mutator lock, all threads must be suspended.
    Runtime::Current()->VisitRoots(this);
    RevokeAllThreadLocalAllocationStacks(self);
  } else {
    MarkRootsCheckpoint(self, kRevokeRosAllocThreadLocalBuffersAtCheckpoint);
    // At this point the live stack should no longer have any mutators which push into it.
    MarkNonThreadRoots();
    MarkConcurrentRoots(
        static_cast<VisitRootFlags>(kVisitRootFlagAllRoots | kVisitRootFlagStartLoggingNewRoots));
  }
}

void MarkSweep::MarkNonThreadRoots() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Runtime::Current()->VisitNonThreadRoots(this);
}

void MarkSweep::MarkConcurrentRoots(VisitRootFlags flags) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Visit all runtime roots and clear dirty flags.
  Runtime::Current()->VisitConcurrentRoots(this, flags);
}

class MarkSweep::DelayReferenceReferentVisitor {
 public:
  explicit DelayReferenceReferentVisitor(MarkSweep* collector) : collector_(collector) {}

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    collector_->DelayReferenceReferent(klass, ref);
  }

 private:
  MarkSweep* const collector_;
};

template <bool kUseFinger = false>
class MarkSweep::MarkStackTask : public Task {
 public:
  MarkStackTask(ThreadPool* thread_pool,
                MarkSweep* mark_sweep,
                size_t mark_stack_size,
                StackReference<mirror::Object>* mark_stack)
      : mark_sweep_(mark_sweep),
        thread_pool_(thread_pool),
        mark_stack_pos_(mark_stack_size) {
    // We may have to copy part of an existing mark stack when another mark stack overflows.
    if (mark_stack_size != 0) {
      DCHECK(mark_stack != nullptr);
      // TODO: Check performance?
      std::copy(mark_stack, mark_stack + mark_stack_size, mark_stack_);
    }
    if (kCountTasks) {
      ++mark_sweep_->work_chunks_created_;
    }
  }

  static const size_t kMaxSize = 1 * KB;

 protected:
  class MarkObjectParallelVisitor {
   public:
    ALWAYS_INLINE MarkObjectParallelVisitor(MarkStackTask<kUseFinger>* chunk_task,
                                            MarkSweep* mark_sweep)
        : chunk_task_(chunk_task), mark_sweep_(mark_sweep) {}

    ALWAYS_INLINE void operator()(mirror::Object* obj,
                    MemberOffset offset,
                    bool is_static ATTRIBUTE_UNUSED) const
        SHARED_REQUIRES(Locks::mutator_lock_) {
      Mark(obj->GetFieldObject<mirror::Object>(offset));
    }

    void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
        SHARED_REQUIRES(Locks::mutator_lock_) {
      if (!root->IsNull()) {
        VisitRoot(root);
      }
    }

    void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
        SHARED_REQUIRES(Locks::mutator_lock_) {
      if (kCheckLocks) {
        Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
        Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
      }
      Mark(root->AsMirrorPtr());
    }

   private:
    ALWAYS_INLINE void Mark(mirror::Object* ref) const SHARED_REQUIRES(Locks::mutator_lock_) {
      if (ref != nullptr && mark_sweep_->MarkObjectParallel(ref)) {
        if (kUseFinger) {
          std::atomic_thread_fence(std::memory_order_seq_cst);
          if (reinterpret_cast<uintptr_t>(ref) >=
              static_cast<uintptr_t>(mark_sweep_->atomic_finger_.LoadRelaxed())) {
            return;
          }
        }
        chunk_task_->MarkStackPush(ref);
      }
    }

    MarkStackTask<kUseFinger>* const chunk_task_;
    MarkSweep* const mark_sweep_;
  };

  class ScanObjectParallelVisitor {
   public:
    ALWAYS_INLINE explicit ScanObjectParallelVisitor(MarkStackTask<kUseFinger>* chunk_task)
        : chunk_task_(chunk_task) {}

    // No thread safety analysis since multiple threads will use this visitor.
    void operator()(mirror::Object* obj) const
        REQUIRES(Locks::heap_bitmap_lock_)
        SHARED_REQUIRES(Locks::mutator_lock_) {
      MarkSweep* const mark_sweep = chunk_task_->mark_sweep_;
      MarkObjectParallelVisitor mark_visitor(chunk_task_, mark_sweep);
      DelayReferenceReferentVisitor ref_visitor(mark_sweep);
      mark_sweep->ScanObjectVisit(obj, mark_visitor, ref_visitor);
    }

   private:
    MarkStackTask<kUseFinger>* const chunk_task_;
  };

  virtual ~MarkStackTask() {
    // Make sure that we have cleared our mark stack.
    DCHECK_EQ(mark_stack_pos_, 0U);
    if (kCountTasks) {
      ++mark_sweep_->work_chunks_deleted_;
    }
  }

  MarkSweep* const mark_sweep_;
  ThreadPool* const thread_pool_;
  // Thread local mark stack for this task.
  StackReference<mirror::Object> mark_stack_[kMaxSize];
  // Mark stack position.
  size_t mark_stack_pos_;

  ALWAYS_INLINE void MarkStackPush(mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (UNLIKELY(mark_stack_pos_ == kMaxSize)) {
      // Mark stack overflow, give 1/2 the stack to the thread pool as a new work task.
      mark_stack_pos_ /= 2;
      auto* task = new MarkStackTask(thread_pool_,
                                     mark_sweep_,
                                     kMaxSize - mark_stack_pos_,
                                     mark_stack_ + mark_stack_pos_);
      thread_pool_->AddTask(Thread::Current(), task);
    }
    DCHECK(obj != nullptr);
    DCHECK_LT(mark_stack_pos_, kMaxSize);
    mark_stack_[mark_stack_pos_++].Assign(obj);
  }

  virtual void Finalize() {
    delete this;
  }

  // Scans all of the objects
  virtual void Run(Thread* self ATTRIBUTE_UNUSED)
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ScanObjectParallelVisitor visitor(this);
    // TODO: Tune this.
    static const size_t kFifoSize = 4;
    BoundedFifoPowerOfTwo<mirror::Object*, kFifoSize> prefetch_fifo;
    for (;;) {
      mirror::Object* obj = nullptr;
      if (kUseMarkStackPrefetch) {
        while (mark_stack_pos_ != 0 && prefetch_fifo.size() < kFifoSize) {
          mirror::Object* const mark_stack_obj = mark_stack_[--mark_stack_pos_].AsMirrorPtr();
          DCHECK(mark_stack_obj != nullptr);
          __builtin_prefetch(mark_stack_obj);
          prefetch_fifo.push_back(mark_stack_obj);
        }
        if (UNLIKELY(prefetch_fifo.empty())) {
          break;
        }
        obj = prefetch_fifo.front();
        prefetch_fifo.pop_front();
      } else {
        if (UNLIKELY(mark_stack_pos_ == 0)) {
          break;
        }
        obj = mark_stack_[--mark_stack_pos_].AsMirrorPtr();
      }
      DCHECK(obj != nullptr);
      visitor(obj);
    }
  }
};

class MarkSweep::CardScanTask : public MarkStackTask<false> {
 public:
  CardScanTask(ThreadPool* thread_pool,
               MarkSweep* mark_sweep,
               accounting::ContinuousSpaceBitmap* bitmap,
               uint8_t* begin,
               uint8_t* end,
               uint8_t minimum_age,
               size_t mark_stack_size,
               StackReference<mirror::Object>* mark_stack_obj,
               bool clear_card)
      : MarkStackTask<false>(thread_pool, mark_sweep, mark_stack_size, mark_stack_obj),
        bitmap_(bitmap),
        begin_(begin),
        end_(end),
        minimum_age_(minimum_age),
        clear_card_(clear_card) {}

 protected:
  accounting::ContinuousSpaceBitmap* const bitmap_;
  uint8_t* const begin_;
  uint8_t* const end_;
  const uint8_t minimum_age_;
  const bool clear_card_;

  virtual void Finalize() {
    delete this;
  }

  virtual void Run(Thread* self) NO_THREAD_SAFETY_ANALYSIS {
    ScanObjectParallelVisitor visitor(this);
    accounting::CardTable* card_table = mark_sweep_->GetHeap()->GetCardTable();
    size_t cards_scanned = clear_card_
        ? card_table->Scan<true>(bitmap_, begin_, end_, visitor, minimum_age_)
        : card_table->Scan<false>(bitmap_, begin_, end_, visitor, minimum_age_);
    VLOG(heap) << "Parallel scanning cards " << reinterpret_cast<void*>(begin_) << " - "
        << reinterpret_cast<void*>(end_) << " = " << cards_scanned;
    // Finish by emptying our local mark stack.
    MarkStackTask::Run(self);
  }
};

size_t MarkSweep::GetThreadCount(bool paused) const {
  // Use less threads if we are in a background state (non jank perceptible) since we want to leave
  // more CPU time for the foreground apps.
  if (heap_->GetThreadPool() == nullptr || !Runtime::Current()->InJankPerceptibleProcessState()) {
    return 1;
  }
  return (paused ? heap_->GetParallelGCThreadCount() : heap_->GetConcGCThreadCount()) + 1;
}

void MarkSweep::ScanGrayObjects(bool paused, uint8_t minimum_age) {
  accounting::CardTable* card_table = GetHeap()->GetCardTable();
  ThreadPool* thread_pool = GetHeap()->GetThreadPool();
  size_t thread_count = GetThreadCount(paused);
  // The parallel version with only one thread is faster for card scanning, TODO: fix.
  if (kParallelCardScan && thread_count > 1) {
    Thread* self = Thread::Current();
    // Can't have a different split for each space since multiple spaces can have their cards being
    // scanned at the same time.
    TimingLogger::ScopedTiming t(paused ? "(Paused)ScanGrayObjects" : __FUNCTION__,
        GetTimings());
    // Try to take some of the mark stack since we can pass this off to the worker tasks.
    StackReference<mirror::Object>* mark_stack_begin = mark_stack_->Begin();
    StackReference<mirror::Object>* mark_stack_end = mark_stack_->End();
    const size_t mark_stack_size = mark_stack_end - mark_stack_begin;
    // Estimated number of work tasks we will create.
    const size_t mark_stack_tasks = GetHeap()->GetContinuousSpaces().size() * thread_count;
    DCHECK_NE(mark_stack_tasks, 0U);
    const size_t mark_stack_delta = std::min(CardScanTask::kMaxSize / 2,
                                             mark_stack_size / mark_stack_tasks + 1);
    for (const auto& space : GetHeap()->GetContinuousSpaces()) {
      if (space->GetMarkBitmap() == nullptr) {
        continue;
      }
      uint8_t* card_begin = space->Begin();
      uint8_t* card_end = space->End();
      // Align up the end address. For example, the image space's end
      // may not be card-size-aligned.
      card_end = AlignUp(card_end, accounting::CardTable::kCardSize);
      DCHECK_ALIGNED(card_begin, accounting::CardTable::kCardSize);
      DCHECK_ALIGNED(card_end, accounting::CardTable::kCardSize);
      // Calculate how many bytes of heap we will scan,
      const size_t address_range = card_end - card_begin;
      // Calculate how much address range each task gets.
      const size_t card_delta = RoundUp(address_range / thread_count + 1,
                                        accounting::CardTable::kCardSize);
      // If paused and the space is neither zygote nor image space, we could clear the dirty
      // cards to avoid accumulating them to increase card scanning load in the following GC
      // cycles. We need to keep dirty cards of image space and zygote space in order to track
      // references to the other spaces.
      bool clear_card = paused && !space->IsZygoteSpace() && !space->IsImageSpace();
      // Create the worker tasks for this space.
      while (card_begin != card_end) {
        // Add a range of cards.
        size_t addr_remaining = card_end - card_begin;
        size_t card_increment = std::min(card_delta, addr_remaining);
        // Take from the back of the mark stack.
        size_t mark_stack_remaining = mark_stack_end - mark_stack_begin;
        size_t mark_stack_increment = std::min(mark_stack_delta, mark_stack_remaining);
        mark_stack_end -= mark_stack_increment;
        mark_stack_->PopBackCount(static_cast<int32_t>(mark_stack_increment));
        DCHECK_EQ(mark_stack_end, mark_stack_->End());
        // Add the new task to the thread pool.
        auto* task = new CardScanTask(thread_pool,
                                      this,
                                      space->GetMarkBitmap(),
                                      card_begin,
                                      card_begin + card_increment,
                                      minimum_age,
                                      mark_stack_increment,
                                      mark_stack_end,
                                      clear_card);
        thread_pool->AddTask(self, task);
        card_begin += card_increment;
      }
    }

    // Note: the card scan below may dirty new cards (and scan them)
    // as a side effect when a Reference object is encountered and
    // queued during the marking. See b/11465268.
    thread_pool->SetMaxActiveWorkers(thread_count - 1);
    thread_pool->StartWorkers(self);
    thread_pool->Wait(self, true, true);
    thread_pool->StopWorkers(self);
  } else {
    for (const auto& space : GetHeap()->GetContinuousSpaces()) {
      if (space->GetMarkBitmap() != nullptr) {
        // Image spaces are handled properly since live == marked for them.
        const char* name = nullptr;
        switch (space->GetGcRetentionPolicy()) {
        case space::kGcRetentionPolicyNeverCollect:
          name = paused ? "(Paused)ScanGrayImageSpaceObjects" : "ScanGrayImageSpaceObjects";
          break;
        case space::kGcRetentionPolicyFullCollect:
          name = paused ? "(Paused)ScanGrayZygoteSpaceObjects" : "ScanGrayZygoteSpaceObjects";
          break;
        case space::kGcRetentionPolicyAlwaysCollect:
          name = paused ? "(Paused)ScanGrayAllocSpaceObjects" : "ScanGrayAllocSpaceObjects";
          break;
        default:
          LOG(FATAL) << "Unreachable";
          UNREACHABLE();
        }
        TimingLogger::ScopedTiming t(name, GetTimings());
        ScanObjectVisitor visitor(this);
        bool clear_card = paused && !space->IsZygoteSpace() && !space->IsImageSpace();
        if (clear_card) {
          card_table->Scan<true>(space->GetMarkBitmap(),
                                 space->Begin(),
                                 space->End(),
                                 visitor,
                                 minimum_age);
        } else {
          card_table->Scan<false>(space->GetMarkBitmap(),
                                  space->Begin(),
                                  space->End(),
                                  visitor,
                                  minimum_age);
        }
      }
    }
  }
}

class MarkSweep::RecursiveMarkTask : public MarkStackTask<false> {
 public:
  RecursiveMarkTask(ThreadPool* thread_pool,
                    MarkSweep* mark_sweep,
                    accounting::ContinuousSpaceBitmap* bitmap,
                    uintptr_t begin,
                    uintptr_t end)
      : MarkStackTask<false>(thread_pool, mark_sweep, 0, nullptr),
        bitmap_(bitmap),
        begin_(begin),
        end_(end) {}

 protected:
  accounting::ContinuousSpaceBitmap* const bitmap_;
  const uintptr_t begin_;
  const uintptr_t end_;

  virtual void Finalize() {
    delete this;
  }

  // Scans all of the objects
  virtual void Run(Thread* self) NO_THREAD_SAFETY_ANALYSIS {
    ScanObjectParallelVisitor visitor(this);
    bitmap_->VisitMarkedRange(begin_, end_, visitor);
    // Finish by emptying our local mark stack.
    MarkStackTask::Run(self);
  }
};

// Populates the mark stack based on the set of marked objects and
// recursively marks until the mark stack is emptied.
void MarkSweep::RecursiveMark() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // RecursiveMark will build the lists of known instances of the Reference classes. See
  // DelayReferenceReferent for details.
  if (kUseRecursiveMark) {
    const bool partial = GetGcType() == kGcTypePartial;
    ScanObjectVisitor scan_visitor(this);
    auto* self = Thread::Current();
    ThreadPool* thread_pool = heap_->GetThreadPool();
    size_t thread_count = GetThreadCount(false);
    const bool parallel = kParallelRecursiveMark && thread_count > 1;
    mark_stack_->Reset();
    for (const auto& space : GetHeap()->GetContinuousSpaces()) {
      if ((space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect) ||
          (!partial && space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect)) {
        current_space_bitmap_ = space->GetMarkBitmap();
        if (current_space_bitmap_ == nullptr) {
          continue;
        }
        if (parallel) {
          // We will use the mark stack the future.
          // CHECK(mark_stack_->IsEmpty());
          // This function does not handle heap end increasing, so we must use the space end.
          uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
          uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
          atomic_finger_.StoreRelaxed(AtomicInteger::MaxValue());

          // Create a few worker tasks.
          const size_t n = thread_count * 2;
          while (begin != end) {
            uintptr_t start = begin;
            uintptr_t delta = (end - begin) / n;
            delta = RoundUp(delta, KB);
            if (delta < 16 * KB) delta = end - begin;
            begin += delta;
            auto* task = new RecursiveMarkTask(thread_pool,
                                               this,
                                               current_space_bitmap_,
                                               start,
                                               begin);
            thread_pool->AddTask(self, task);
          }
          thread_pool->SetMaxActiveWorkers(thread_count - 1);
          thread_pool->StartWorkers(self);
          thread_pool->Wait(self, true, true);
          thread_pool->StopWorkers(self);
        } else {
          // This function does not handle heap end increasing, so we must use the space end.
          uintptr_t begin = reinterpret_cast<uintptr_t>(space->Begin());
          uintptr_t end = reinterpret_cast<uintptr_t>(space->End());
          current_space_bitmap_->VisitMarkedRange(begin, end, scan_visitor);
        }
      }
    }
  }
  ProcessMarkStack(false);
}

void MarkSweep::RecursiveMarkDirtyObjects(bool paused, uint8_t minimum_age) {
  ScanGrayObjects(paused, minimum_age);
  ProcessMarkStack(paused);
}

void MarkSweep::ReMarkRoots() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  Runtime::Current()->VisitRoots(this, static_cast<VisitRootFlags>(
      kVisitRootFlagNewRoots | kVisitRootFlagStopLoggingNewRoots | kVisitRootFlagClearRootLog));
  if (kVerifyRootsMarked) {
    TimingLogger::ScopedTiming t2("(Paused)VerifyRoots", GetTimings());
    VerifyRootMarkedVisitor visitor(this);
    Runtime::Current()->VisitRoots(&visitor);
  }
}

void MarkSweep::SweepSystemWeaks(Thread* self) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
  Runtime::Current()->SweepSystemWeaks(this);
}

class MarkSweep::VerifySystemWeakVisitor : public IsMarkedVisitor {
 public:
  explicit VerifySystemWeakVisitor(MarkSweep* mark_sweep) : mark_sweep_(mark_sweep) {}

  virtual mirror::Object* IsMarked(mirror::Object* obj)
      OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    mark_sweep_->VerifyIsLive(obj);
    return obj;
  }

  MarkSweep* const mark_sweep_;
};

void MarkSweep::VerifyIsLive(const mirror::Object* obj) {
  if (!heap_->GetLiveBitmap()->Test(obj)) {
    // TODO: Consider live stack? Has this code bitrotted?
    CHECK(!heap_->allocation_stack_->Contains(obj))
        << "Found dead object " << obj << "\n" << heap_->DumpSpaces();
  }
}

void MarkSweep::VerifySystemWeaks() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Verify system weaks, uses a special object visitor which returns the input object.
  VerifySystemWeakVisitor visitor(this);
  Runtime::Current()->SweepSystemWeaks(&visitor);
}

class MarkSweep::CheckpointMarkThreadRoots : public Closure, public RootVisitor {
 public:
  CheckpointMarkThreadRoots(MarkSweep* mark_sweep,
                            bool revoke_ros_alloc_thread_local_buffers_at_checkpoint)
      : mark_sweep_(mark_sweep),
        revoke_ros_alloc_thread_local_buffers_at_checkpoint_(
            revoke_ros_alloc_thread_local_buffers_at_checkpoint) {
  }

  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_) {
    for (size_t i = 0; i < count; ++i) {
      mark_sweep_->MarkObjectNonNullParallel(*roots[i]);
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots,
                  size_t count,
                  const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_)
      REQUIRES(Locks::heap_bitmap_lock_) {
    for (size_t i = 0; i < count; ++i) {
      mark_sweep_->MarkObjectNonNullParallel(roots[i]->AsMirrorPtr());
    }
  }

  virtual void Run(Thread* thread) OVERRIDE NO_THREAD_SAFETY_ANALYSIS {
    ScopedTrace trace("Marking thread roots");
    // Note: self is not necessarily equal to thread since thread may be suspended.
    Thread* const self = Thread::Current();
    CHECK(thread == self || thread->IsSuspended() || thread->GetState() == kWaitingPerformingGc)
        << thread->GetState() << " thread " << thread << " self " << self;
    thread->VisitRoots(this);
    if (revoke_ros_alloc_thread_local_buffers_at_checkpoint_) {
      ScopedTrace trace2("RevokeRosAllocThreadLocalBuffers");
      mark_sweep_->GetHeap()->RevokeRosAllocThreadLocalBuffers(thread);
    }
    // If thread is a running mutator, then act on behalf of the garbage collector.
    // See the code in ThreadList::RunCheckpoint.
    mark_sweep_->GetBarrier().Pass(self);
  }

 private:
  MarkSweep* const mark_sweep_;
  const bool revoke_ros_alloc_thread_local_buffers_at_checkpoint_;
};

void MarkSweep::MarkRootsCheckpoint(Thread* self,
                                    bool revoke_ros_alloc_thread_local_buffers_at_checkpoint) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  CheckpointMarkThreadRoots check_point(this, revoke_ros_alloc_thread_local_buffers_at_checkpoint);
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  // Request the check point is run on all threads returning a count of the threads that must
  // run through the barrier including self.
  size_t barrier_count = thread_list->RunCheckpoint(&check_point);
  // Release locks then wait for all mutator threads to pass the barrier.
  // If there are no threads to wait which implys that all the checkpoint functions are finished,
  // then no need to release locks.
  if (barrier_count == 0) {
    return;
  }
  Locks::heap_bitmap_lock_->ExclusiveUnlock(self);
  Locks::mutator_lock_->SharedUnlock(self);
  {
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    gc_barrier_->Increment(self, barrier_count);
  }
  Locks::mutator_lock_->SharedLock(self);
  Locks::heap_bitmap_lock_->ExclusiveLock(self);
}

void MarkSweep::SweepArray(accounting::ObjectStack* allocations, bool swap_bitmaps) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  Thread* self = Thread::Current();
  mirror::Object** chunk_free_buffer = reinterpret_cast<mirror::Object**>(
      sweep_array_free_buffer_mem_map_->BaseBegin());
  size_t chunk_free_pos = 0;
  ObjectBytePair freed;
  ObjectBytePair freed_los;
  // How many objects are left in the array, modified after each space is swept.
  StackReference<mirror::Object>* objects = allocations->Begin();
  size_t count = allocations->Size();
  // Change the order to ensure that the non-moving space last swept as an optimization.
  std::vector<space::ContinuousSpace*> sweep_spaces;
  space::ContinuousSpace* non_moving_space = nullptr;
  for (space::ContinuousSpace* space : heap_->GetContinuousSpaces()) {
    if (space->IsAllocSpace() &&
        !immune_spaces_.ContainsSpace(space) &&
        space->GetLiveBitmap() != nullptr) {
      if (space == heap_->GetNonMovingSpace()) {
        non_moving_space = space;
      } else {
        sweep_spaces.push_back(space);
      }
    }
  }
  // Unlikely to sweep a significant amount of non_movable objects, so we do these after the after
  // the other alloc spaces as an optimization.
  if (non_moving_space != nullptr) {
    sweep_spaces.push_back(non_moving_space);
  }
  // Start by sweeping the continuous spaces.
  for (space::ContinuousSpace* space : sweep_spaces) {
    space::AllocSpace* alloc_space = space->AsAllocSpace();
    accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
    if (swap_bitmaps) {
      std::swap(live_bitmap, mark_bitmap);
    }
    StackReference<mirror::Object>* out = objects;
    for (size_t i = 0; i < count; ++i) {
      mirror::Object* const obj = objects[i].AsMirrorPtr();
      if (kUseThreadLocalAllocationStack && obj == nullptr) {
        continue;
      }
      if (space->HasAddress(obj)) {
        // This object is in the space, remove it from the array and add it to the sweep buffer
        // if needed.
        if (!mark_bitmap->Test(obj)) {
          if (chunk_free_pos >= kSweepArrayChunkFreeSize) {
            TimingLogger::ScopedTiming t2("FreeList", GetTimings());
            freed.objects += chunk_free_pos;
            freed.bytes += alloc_space->FreeList(self, chunk_free_pos, chunk_free_buffer);
            chunk_free_pos = 0;
          }
          chunk_free_buffer[chunk_free_pos++] = obj;
        }
      } else {
        (out++)->Assign(obj);
      }
    }
    if (chunk_free_pos > 0) {
      TimingLogger::ScopedTiming t2("FreeList", GetTimings());
      freed.objects += chunk_free_pos;
      freed.bytes += alloc_space->FreeList(self, chunk_free_pos, chunk_free_buffer);
      chunk_free_pos = 0;
    }
    // All of the references which space contained are no longer in the allocation stack, update
    // the count.
    count = out - objects;
  }
  // Handle the large object space.
  space::LargeObjectSpace* large_object_space = GetHeap()->GetLargeObjectsSpace();
  if (large_object_space != nullptr) {
    accounting::LargeObjectBitmap* large_live_objects = large_object_space->GetLiveBitmap();
    accounting::LargeObjectBitmap* large_mark_objects = large_object_space->GetMarkBitmap();
    if (swap_bitmaps) {
      std::swap(large_live_objects, large_mark_objects);
    }
    for (size_t i = 0; i < count; ++i) {
      mirror::Object* const obj = objects[i].AsMirrorPtr();
      // Handle large objects.
      if (kUseThreadLocalAllocationStack && obj == nullptr) {
        continue;
      }
      if (!large_mark_objects->Test(obj)) {
        ++freed_los.objects;
        freed_los.bytes += large_object_space->Free(self, obj);
      }
    }
  }
  {
    TimingLogger::ScopedTiming t2("RecordFree", GetTimings());
    RecordFree(freed);
    RecordFreeLOS(freed_los);
    t2.NewTiming("ResetStack");
    allocations->Reset();
  }
  sweep_array_free_buffer_mem_map_->MadviseDontNeedAndZero();
}

void MarkSweep::Sweep(bool swap_bitmaps) {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Ensure that nobody inserted items in the live stack after we swapped the stacks.
  CHECK_GE(live_stack_freeze_size_, GetHeap()->GetLiveStack()->Size());
  {
    TimingLogger::ScopedTiming t2("MarkAllocStackAsLive", GetTimings());
    // Mark everything allocated since the last as GC live so that we can sweep concurrently,
    // knowing that new allocations won't be marked as live.
    accounting::ObjectStack* live_stack = heap_->GetLiveStack();
    heap_->MarkAllocStackAsLive(live_stack);
    live_stack->Reset();
    DCHECK(mark_stack_->IsEmpty());
  }
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsContinuousMemMapAllocSpace()) {
      space::ContinuousMemMapAllocSpace* alloc_space = space->AsContinuousMemMapAllocSpace();
      TimingLogger::ScopedTiming split(
          alloc_space->IsZygoteSpace() ? "SweepZygoteSpace" : "SweepMallocSpace",
          GetTimings());
      RecordFree(alloc_space->Sweep(swap_bitmaps));
    }
  }
  SweepLargeObjects(swap_bitmaps);
}

void MarkSweep::SweepLargeObjects(bool swap_bitmaps) {
  space::LargeObjectSpace* los = heap_->GetLargeObjectsSpace();
  if (los != nullptr) {
    TimingLogger::ScopedTiming split(__FUNCTION__, GetTimings());
    RecordFreeLOS(los->Sweep(swap_bitmaps));
  }
}

// Process the "referent" field in a java.lang.ref.Reference.  If the referent has not yet been
// marked, put it on the appropriate list in the heap for later processing.
void MarkSweep::DelayReferenceReferent(mirror::Class* klass, mirror::Reference* ref) {
  heap_->GetReferenceProcessor()->DelayReferenceReferent(klass, ref, this);
}

class MarkVisitor {
 public:
  ALWAYS_INLINE explicit MarkVisitor(MarkSweep* const mark_sweep) : mark_sweep_(mark_sweep) {}

  ALWAYS_INLINE void operator()(mirror::Object* obj,
                                MemberOffset offset,
                                bool is_static ATTRIBUTE_UNUSED) const
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->MarkObject(obj->GetFieldObject<mirror::Object>(offset), obj, offset);
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (kCheckLocks) {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      Locks::heap_bitmap_lock_->AssertExclusiveHeld(Thread::Current());
    }
    mark_sweep_->MarkObject(root->AsMirrorPtr());
  }

 private:
  MarkSweep* const mark_sweep_;
};

// Scans an object reference.  Determines the type of the reference
// and dispatches to a specialized scanning routine.
void MarkSweep::ScanObject(mirror::Object* obj) {
  MarkVisitor mark_visitor(this);
  DelayReferenceReferentVisitor ref_visitor(this);
  ScanObjectVisit(obj, mark_visitor, ref_visitor);
}

void MarkSweep::ProcessMarkStackParallel(size_t thread_count) {
  Thread* self = Thread::Current();
  ThreadPool* thread_pool = GetHeap()->GetThreadPool();
  const size_t chunk_size = std::min(mark_stack_->Size() / thread_count + 1,
                                     static_cast<size_t>(MarkStackTask<false>::kMaxSize));
  CHECK_GT(chunk_size, 0U);
  // Split the current mark stack up into work tasks.
  for (auto* it = mark_stack_->Begin(), *end = mark_stack_->End(); it < end; ) {
    const size_t delta = std::min(static_cast<size_t>(end - it), chunk_size);
    thread_pool->AddTask(self, new MarkStackTask<false>(thread_pool, this, delta, it));
    it += delta;
  }
  thread_pool->SetMaxActiveWorkers(thread_count - 1);
  thread_pool->StartWorkers(self);
  thread_pool->Wait(self, true, true);
  thread_pool->StopWorkers(self);
  mark_stack_->Reset();
  CHECK_EQ(work_chunks_created_.LoadSequentiallyConsistent(),
           work_chunks_deleted_.LoadSequentiallyConsistent())
      << " some of the work chunks were leaked";
}

// Scan anything that's on the mark stack.
void MarkSweep::ProcessMarkStack(bool paused) {
  TimingLogger::ScopedTiming t(paused ? "(Paused)ProcessMarkStack" : __FUNCTION__, GetTimings());
  size_t thread_count = GetThreadCount(paused);
  if (kParallelProcessMarkStack && thread_count > 1 &&
      mark_stack_->Size() >= kMinimumParallelMarkStackSize) {
    ProcessMarkStackParallel(thread_count);
  } else {
    // TODO: Tune this.
    static const size_t kFifoSize = 4;
    BoundedFifoPowerOfTwo<mirror::Object*, kFifoSize> prefetch_fifo;
    for (;;) {
      mirror::Object* obj = nullptr;
      if (kUseMarkStackPrefetch) {
        while (!mark_stack_->IsEmpty() && prefetch_fifo.size() < kFifoSize) {
          mirror::Object* mark_stack_obj = mark_stack_->PopBack();
          DCHECK(mark_stack_obj != nullptr);
          __builtin_prefetch(mark_stack_obj);
          prefetch_fifo.push_back(mark_stack_obj);
        }
        if (prefetch_fifo.empty()) {
          break;
        }
        obj = prefetch_fifo.front();
        prefetch_fifo.pop_front();
      } else {
        if (mark_stack_->IsEmpty()) {
          break;
        }
        obj = mark_stack_->PopBack();
      }
      DCHECK(obj != nullptr);
      ScanObject(obj);
    }
  }
}

inline mirror::Object* MarkSweep::IsMarked(mirror::Object* object) {
  if (immune_spaces_.IsInImmuneRegion(object)) {
    return object;
  }
  if (current_space_bitmap_->HasAddress(object)) {
    return current_space_bitmap_->Test(object) ? object : nullptr;
  }
  return mark_bitmap_->Test(object) ? object : nullptr;
}

void MarkSweep::FinishPhase() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  if (kCountScannedTypes) {
    VLOG(gc)
        << "MarkSweep scanned"
        << " no reference objects=" << no_reference_class_count_.LoadRelaxed()
        << " normal objects=" << normal_count_.LoadRelaxed()
        << " classes=" << class_count_.LoadRelaxed()
        << " object arrays=" << object_array_count_.LoadRelaxed()
        << " references=" << reference_count_.LoadRelaxed()
        << " other=" << other_count_.LoadRelaxed();
  }
  if (kCountTasks) {
    VLOG(gc) << "Total number of work chunks allocated: " << work_chunks_created_.LoadRelaxed();
  }
  if (kMeasureOverhead) {
    VLOG(gc) << "Overhead time " << PrettyDuration(overhead_time_.LoadRelaxed());
  }
  if (kProfileLargeObjects) {
    VLOG(gc) << "Large objects tested " << large_object_test_.LoadRelaxed()
        << " marked " << large_object_mark_.LoadRelaxed();
  }
  if (kCountMarkedObjects) {
    VLOG(gc) << "Marked: null=" << mark_null_count_.LoadRelaxed()
        << " immune=" <<  mark_immune_count_.LoadRelaxed()
        << " fastpath=" << mark_fastpath_count_.LoadRelaxed()
        << " slowpath=" << mark_slowpath_count_.LoadRelaxed();
  }
  CHECK(mark_stack_->IsEmpty());  // Ensure that the mark stack is empty.
  mark_stack_->Reset();
  Thread* const self = Thread::Current();
  ReaderMutexLock mu(self, *Locks::mutator_lock_);
  WriterMutexLock mu2(self, *Locks::heap_bitmap_lock_);
  heap_->ClearMarkedObjects();
}

void MarkSweep::RevokeAllThreadLocalBuffers() {
  if (kRevokeRosAllocThreadLocalBuffersAtCheckpoint && IsConcurrent()) {
    // If concurrent, rosalloc thread-local buffers are revoked at the
    // thread checkpoint. Bump pointer space thread-local buffers must
    // not be in use.
    GetHeap()->AssertAllBumpPointerSpaceThreadLocalBuffersAreRevoked();
  } else {
    TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
    GetHeap()->RevokeAllThreadLocalBuffers();
  }
}

}  // namespace collector
}  // namespace gc
}  // namespace art
