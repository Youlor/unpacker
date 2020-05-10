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

#include <stdio.h>

#include "garbage_collector.h"

#include "base/dumpable.h"
#include "base/histogram-inl.h"
#include "base/logging.h"
#include "base/mutex-inl.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "thread-inl.h"
#include "thread_list.h"
#include "utils.h"

namespace art {
namespace gc {
namespace collector {

Iteration::Iteration()
    : duration_ns_(0), timings_("GC iteration timing logger", true, VLOG_IS_ON(heap)) {
  Reset(kGcCauseBackground, false);  // Reset to some place holder values.
}

void Iteration::Reset(GcCause gc_cause, bool clear_soft_references) {
  timings_.Reset();
  pause_times_.clear();
  duration_ns_ = 0;
  clear_soft_references_ = clear_soft_references;
  gc_cause_ = gc_cause;
  freed_ = ObjectBytePair();
  freed_los_ = ObjectBytePair();
  freed_bytes_revoke_ = 0;
}

uint64_t Iteration::GetEstimatedThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (static_cast<uint64_t>(freed_.bytes) * 1000) / (NsToMs(GetDurationNs()) + 1);
}

GarbageCollector::GarbageCollector(Heap* heap, const std::string& name)
    : heap_(heap),
      name_(name),
      pause_histogram_((name_ + " paused").c_str(), kPauseBucketSize, kPauseBucketCount),
      cumulative_timings_(name),
      pause_histogram_lock_("pause histogram lock", kDefaultMutexLevel, true) {
  ResetCumulativeStatistics();
}

void GarbageCollector::RegisterPause(uint64_t nano_length) {
  GetCurrentIteration()->pause_times_.push_back(nano_length);
}

void GarbageCollector::ResetCumulativeStatistics() {
  cumulative_timings_.Reset();
  total_time_ns_ = 0;
  total_freed_objects_ = 0;
  total_freed_bytes_ = 0;
  MutexLock mu(Thread::Current(), pause_histogram_lock_);
  pause_histogram_.Reset();
}

void GarbageCollector::Run(GcCause gc_cause, bool clear_soft_references) {
  ScopedTrace trace(StringPrintf("%s %s GC", PrettyCause(gc_cause), GetName()));
  Thread* self = Thread::Current();
  uint64_t start_time = NanoTime();
  Iteration* current_iteration = GetCurrentIteration();
  current_iteration->Reset(gc_cause, clear_soft_references);
  RunPhases();  // Run all the GC phases.
  // Add the current timings to the cumulative timings.
  cumulative_timings_.AddLogger(*GetTimings());
  // Update cumulative statistics with how many bytes the GC iteration freed.
  total_freed_objects_ += current_iteration->GetFreedObjects() +
      current_iteration->GetFreedLargeObjects();
  total_freed_bytes_ += current_iteration->GetFreedBytes() +
      current_iteration->GetFreedLargeObjectBytes();
  uint64_t end_time = NanoTime();
  current_iteration->SetDurationNs(end_time - start_time);
  if (Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // The entire GC was paused, clear the fake pauses which might be in the pause times and add
    // the whole GC duration.
    current_iteration->pause_times_.clear();
    RegisterPause(current_iteration->GetDurationNs());
  }
  total_time_ns_ += current_iteration->GetDurationNs();
  for (uint64_t pause_time : current_iteration->GetPauseTimes()) {
    MutexLock mu(self, pause_histogram_lock_);
    pause_histogram_.AdjustAndAddValue(pause_time);
  }
}

void GarbageCollector::SwapBitmaps() {
  TimingLogger::ScopedTiming t(__FUNCTION__, GetTimings());
  // Swap the live and mark bitmaps for each alloc space. This is needed since sweep re-swaps
  // these bitmaps. The bitmap swapping is an optimization so that we do not need to clear the live
  // bits of dead objects in the live bitmap.
  const GcType gc_type = GetGcType();
  for (const auto& space : GetHeap()->GetContinuousSpaces()) {
    // We never allocate into zygote spaces.
    if (space->GetGcRetentionPolicy() == space::kGcRetentionPolicyAlwaysCollect ||
        (gc_type == kGcTypeFull &&
         space->GetGcRetentionPolicy() == space::kGcRetentionPolicyFullCollect)) {
      accounting::ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
      accounting::ContinuousSpaceBitmap* mark_bitmap = space->GetMarkBitmap();
      if (live_bitmap != nullptr && live_bitmap != mark_bitmap) {
        heap_->GetLiveBitmap()->ReplaceBitmap(live_bitmap, mark_bitmap);
        heap_->GetMarkBitmap()->ReplaceBitmap(mark_bitmap, live_bitmap);
        CHECK(space->IsContinuousMemMapAllocSpace());
        space->AsContinuousMemMapAllocSpace()->SwapBitmaps();
      }
    }
  }
  for (const auto& disc_space : GetHeap()->GetDiscontinuousSpaces()) {
    space::LargeObjectSpace* space = disc_space->AsLargeObjectSpace();
    accounting::LargeObjectBitmap* live_set = space->GetLiveBitmap();
    accounting::LargeObjectBitmap* mark_set = space->GetMarkBitmap();
    heap_->GetLiveBitmap()->ReplaceLargeObjectBitmap(live_set, mark_set);
    heap_->GetMarkBitmap()->ReplaceLargeObjectBitmap(mark_set, live_set);
    space->SwapBitmaps();
  }
}

uint64_t GarbageCollector::GetEstimatedMeanThroughput() const {
  // Add 1ms to prevent possible division by 0.
  return (total_freed_bytes_ * 1000) / (NsToMs(GetCumulativeTimings().GetTotalNs()) + 1);
}

void GarbageCollector::ResetMeasurements() {
  {
    MutexLock mu(Thread::Current(), pause_histogram_lock_);
    pause_histogram_.Reset();
  }
  cumulative_timings_.Reset();
  total_time_ns_ = 0;
  total_freed_objects_ = 0;
  total_freed_bytes_ = 0;
}

GarbageCollector::ScopedPause::ScopedPause(GarbageCollector* collector)
    : start_time_(NanoTime()), collector_(collector) {
  Runtime::Current()->GetThreadList()->SuspendAll(__FUNCTION__);
}

GarbageCollector::ScopedPause::~ScopedPause() {
  collector_->RegisterPause(NanoTime() - start_time_);
  Runtime::Current()->GetThreadList()->ResumeAll();
}

// Returns the current GC iteration and assocated info.
Iteration* GarbageCollector::GetCurrentIteration() {
  return heap_->GetCurrentGcIteration();
}
const Iteration* GarbageCollector::GetCurrentIteration() const {
  return heap_->GetCurrentGcIteration();
}

void GarbageCollector::RecordFree(const ObjectBytePair& freed) {
  GetCurrentIteration()->freed_.Add(freed);
  heap_->RecordFree(freed.objects, freed.bytes);
}
void GarbageCollector::RecordFreeLOS(const ObjectBytePair& freed) {
  GetCurrentIteration()->freed_los_.Add(freed);
  heap_->RecordFree(freed.objects, freed.bytes);
}

uint64_t GarbageCollector::GetTotalPausedTimeNs() {
  MutexLock mu(Thread::Current(), pause_histogram_lock_);
  return pause_histogram_.AdjustedSum();
}

void GarbageCollector::DumpPerformanceInfo(std::ostream& os) {
  const CumulativeLogger& logger = GetCumulativeTimings();
  const size_t iterations = logger.GetIterations();
  if (iterations == 0) {
    return;
  }
  os << Dumpable<CumulativeLogger>(logger);
  const uint64_t total_ns = logger.GetTotalNs();
  double seconds = NsToMs(logger.GetTotalNs()) / 1000.0;
  const uint64_t freed_bytes = GetTotalFreedBytes();
  const uint64_t freed_objects = GetTotalFreedObjects();
  {
    MutexLock mu(Thread::Current(), pause_histogram_lock_);
    if (pause_histogram_.SampleSize() > 0) {
      Histogram<uint64_t>::CumulativeData cumulative_data;
      pause_histogram_.CreateHistogram(&cumulative_data);
      pause_histogram_.PrintConfidenceIntervals(os, 0.99, cumulative_data);
    }
  }
  os << GetName() << " total time: " << PrettyDuration(total_ns)
     << " mean time: " << PrettyDuration(total_ns / iterations) << "\n"
     << GetName() << " freed: " << freed_objects
     << " objects with total size " << PrettySize(freed_bytes) << "\n"
     << GetName() << " throughput: " << freed_objects / seconds << "/s / "
     << PrettySize(freed_bytes / seconds) << "/s\n";
}

}  // namespace collector
}  // namespace gc
}  // namespace art
