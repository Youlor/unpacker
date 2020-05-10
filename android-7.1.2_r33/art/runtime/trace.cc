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

#include "trace.h"

#include <sys/uio.h>
#include <unistd.h>

#include "art_method-inl.h"
#include "base/casts.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/scoped_gc_critical_section.h"
#include "instrumentation.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"
#include "entrypoints/quick/quick_entrypoints.h"

namespace art {

static constexpr size_t TraceActionBits = MinimumBitsToStore(
    static_cast<size_t>(kTraceMethodActionMask));
static constexpr uint8_t kOpNewMethod = 1U;
static constexpr uint8_t kOpNewThread = 2U;

class BuildStackTraceVisitor : public StackVisitor {
 public:
  explicit BuildStackTraceVisitor(Thread* thread)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        method_trace_(Trace::AllocStackTrace()) {}

  bool VisitFrame() SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    // Ignore runtime frames (in particular callee save).
    if (!m->IsRuntimeMethod()) {
      method_trace_->push_back(m);
    }
    return true;
  }

  // Returns a stack trace where the topmost frame corresponds with the first element of the vector.
  std::vector<ArtMethod*>* GetStackTrace() const {
    return method_trace_;
  }

 private:
  std::vector<ArtMethod*>* const method_trace_;

  DISALLOW_COPY_AND_ASSIGN(BuildStackTraceVisitor);
};

static const char     kTraceTokenChar             = '*';
static const uint16_t kTraceHeaderLength          = 32;
static const uint32_t kTraceMagicValue            = 0x574f4c53;
static const uint16_t kTraceVersionSingleClock    = 2;
static const uint16_t kTraceVersionDualClock      = 3;
static const uint16_t kTraceRecordSizeSingleClock = 10;  // using v2
static const uint16_t kTraceRecordSizeDualClock   = 14;  // using v3 with two timestamps

TraceClockSource Trace::default_clock_source_ = kDefaultTraceClockSource;

Trace* volatile Trace::the_trace_ = nullptr;
pthread_t Trace::sampling_pthread_ = 0U;
std::unique_ptr<std::vector<ArtMethod*>> Trace::temp_stack_trace_;

// The key identifying the tracer to update instrumentation.
static constexpr const char* kTracerInstrumentationKey = "Tracer";

static TraceAction DecodeTraceAction(uint32_t tmid) {
  return static_cast<TraceAction>(tmid & kTraceMethodActionMask);
}

ArtMethod* Trace::DecodeTraceMethod(uint32_t tmid) {
  MutexLock mu(Thread::Current(), *unique_methods_lock_);
  return unique_methods_[tmid >> TraceActionBits];
}

uint32_t Trace::EncodeTraceMethod(ArtMethod* method) {
  MutexLock mu(Thread::Current(), *unique_methods_lock_);
  uint32_t idx;
  auto it = art_method_id_map_.find(method);
  if (it != art_method_id_map_.end()) {
    idx = it->second;
  } else {
    unique_methods_.push_back(method);
    idx = unique_methods_.size() - 1;
    art_method_id_map_.emplace(method, idx);
  }
  DCHECK_LT(idx, unique_methods_.size());
  DCHECK_EQ(unique_methods_[idx], method);
  return idx;
}

uint32_t Trace::EncodeTraceMethodAndAction(ArtMethod* method, TraceAction action) {
  uint32_t tmid = (EncodeTraceMethod(method) << TraceActionBits) | action;
  DCHECK_EQ(method, DecodeTraceMethod(tmid));
  return tmid;
}

std::vector<ArtMethod*>* Trace::AllocStackTrace() {
  return (temp_stack_trace_.get() != nullptr)  ? temp_stack_trace_.release() :
      new std::vector<ArtMethod*>();
}

void Trace::FreeStackTrace(std::vector<ArtMethod*>* stack_trace) {
  stack_trace->clear();
  temp_stack_trace_.reset(stack_trace);
}

void Trace::SetDefaultClockSource(TraceClockSource clock_source) {
#if defined(__linux__)
  default_clock_source_ = clock_source;
#else
  if (clock_source != TraceClockSource::kWall) {
    LOG(WARNING) << "Ignoring tracing request to use CPU time.";
  }
#endif
}

static uint16_t GetTraceVersion(TraceClockSource clock_source) {
  return (clock_source == TraceClockSource::kDual) ? kTraceVersionDualClock
                                                    : kTraceVersionSingleClock;
}

static uint16_t GetRecordSize(TraceClockSource clock_source) {
  return (clock_source == TraceClockSource::kDual) ? kTraceRecordSizeDualClock
                                                    : kTraceRecordSizeSingleClock;
}

bool Trace::UseThreadCpuClock() {
  return (clock_source_ == TraceClockSource::kThreadCpu) ||
      (clock_source_ == TraceClockSource::kDual);
}

bool Trace::UseWallClock() {
  return (clock_source_ == TraceClockSource::kWall) ||
      (clock_source_ == TraceClockSource::kDual);
}

void Trace::MeasureClockOverhead() {
  if (UseThreadCpuClock()) {
    Thread::Current()->GetCpuMicroTime();
  }
  if (UseWallClock()) {
    MicroTime();
  }
}

// Compute an average time taken to measure clocks.
uint32_t Trace::GetClockOverheadNanoSeconds() {
  Thread* self = Thread::Current();
  uint64_t start = self->GetCpuMicroTime();

  for (int i = 4000; i > 0; i--) {
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
  }

  uint64_t elapsed_us = self->GetCpuMicroTime() - start;
  return static_cast<uint32_t>(elapsed_us / 32);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append2LE(uint8_t* buf, uint16_t val) {
  *buf++ = static_cast<uint8_t>(val);
  *buf++ = static_cast<uint8_t>(val >> 8);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append4LE(uint8_t* buf, uint32_t val) {
  *buf++ = static_cast<uint8_t>(val);
  *buf++ = static_cast<uint8_t>(val >> 8);
  *buf++ = static_cast<uint8_t>(val >> 16);
  *buf++ = static_cast<uint8_t>(val >> 24);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append8LE(uint8_t* buf, uint64_t val) {
  *buf++ = static_cast<uint8_t>(val);
  *buf++ = static_cast<uint8_t>(val >> 8);
  *buf++ = static_cast<uint8_t>(val >> 16);
  *buf++ = static_cast<uint8_t>(val >> 24);
  *buf++ = static_cast<uint8_t>(val >> 32);
  *buf++ = static_cast<uint8_t>(val >> 40);
  *buf++ = static_cast<uint8_t>(val >> 48);
  *buf++ = static_cast<uint8_t>(val >> 56);
}

static void GetSample(Thread* thread, void* arg) SHARED_REQUIRES(Locks::mutator_lock_) {
  BuildStackTraceVisitor build_trace_visitor(thread);
  build_trace_visitor.WalkStack();
  std::vector<ArtMethod*>* stack_trace = build_trace_visitor.GetStackTrace();
  Trace* the_trace = reinterpret_cast<Trace*>(arg);
  the_trace->CompareAndUpdateStackTrace(thread, stack_trace);
}

static void ClearThreadStackTraceAndClockBase(Thread* thread, void* arg ATTRIBUTE_UNUSED) {
  thread->SetTraceClockBase(0);
  std::vector<ArtMethod*>* stack_trace = thread->GetStackTraceSample();
  thread->SetStackTraceSample(nullptr);
  delete stack_trace;
}

void Trace::CompareAndUpdateStackTrace(Thread* thread,
                                       std::vector<ArtMethod*>* stack_trace) {
  CHECK_EQ(pthread_self(), sampling_pthread_);
  std::vector<ArtMethod*>* old_stack_trace = thread->GetStackTraceSample();
  // Update the thread's stack trace sample.
  thread->SetStackTraceSample(stack_trace);
  // Read timer clocks to use for all events in this trace.
  uint32_t thread_clock_diff = 0;
  uint32_t wall_clock_diff = 0;
  ReadClocks(thread, &thread_clock_diff, &wall_clock_diff);
  if (old_stack_trace == nullptr) {
    // If there's no previous stack trace sample for this thread, log an entry event for all
    // methods in the trace.
    for (auto rit = stack_trace->rbegin(); rit != stack_trace->rend(); ++rit) {
      LogMethodTraceEvent(thread, *rit, instrumentation::Instrumentation::kMethodEntered,
                          thread_clock_diff, wall_clock_diff);
    }
  } else {
    // If there's a previous stack trace for this thread, diff the traces and emit entry and exit
    // events accordingly.
    auto old_rit = old_stack_trace->rbegin();
    auto rit = stack_trace->rbegin();
    // Iterate bottom-up over both traces until there's a difference between them.
    while (old_rit != old_stack_trace->rend() && rit != stack_trace->rend() && *old_rit == *rit) {
      old_rit++;
      rit++;
    }
    // Iterate top-down over the old trace until the point where they differ, emitting exit events.
    for (auto old_it = old_stack_trace->begin(); old_it != old_rit.base(); ++old_it) {
      LogMethodTraceEvent(thread, *old_it, instrumentation::Instrumentation::kMethodExited,
                          thread_clock_diff, wall_clock_diff);
    }
    // Iterate bottom-up over the new trace from the point where they differ, emitting entry events.
    for (; rit != stack_trace->rend(); ++rit) {
      LogMethodTraceEvent(thread, *rit, instrumentation::Instrumentation::kMethodEntered,
                          thread_clock_diff, wall_clock_diff);
    }
    FreeStackTrace(old_stack_trace);
  }
}

void* Trace::RunSamplingThread(void* arg) {
  Runtime* runtime = Runtime::Current();
  intptr_t interval_us = reinterpret_cast<intptr_t>(arg);
  CHECK_GE(interval_us, 0);
  CHECK(runtime->AttachCurrentThread("Sampling Profiler", true, runtime->GetSystemThreadGroup(),
                                     !runtime->IsAotCompiler()));

  while (true) {
    usleep(interval_us);
    ScopedTrace trace("Profile sampling");
    Thread* self = Thread::Current();
    Trace* the_trace;
    {
      MutexLock mu(self, *Locks::trace_lock_);
      the_trace = the_trace_;
      if (the_trace == nullptr) {
        break;
      }
    }
    {
      ScopedSuspendAll ssa(__FUNCTION__);
      MutexLock mu(self, *Locks::thread_list_lock_);
      runtime->GetThreadList()->ForEach(GetSample, the_trace);
    }
  }

  runtime->DetachCurrentThread();
  return nullptr;
}

void Trace::Start(const char* trace_filename, int trace_fd, size_t buffer_size, int flags,
                  TraceOutputMode output_mode, TraceMode trace_mode, int interval_us) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != nullptr) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
      return;
    }
  }

  // Check interval if sampling is enabled
  if (trace_mode == TraceMode::kSampling && interval_us <= 0) {
    LOG(ERROR) << "Invalid sampling interval: " << interval_us;
    ScopedObjectAccess soa(self);
    ThrowRuntimeException("Invalid sampling interval: %d", interval_us);
    return;
  }

  // Open trace file if not going directly to ddms.
  std::unique_ptr<File> trace_file;
  if (output_mode != TraceOutputMode::kDDMS) {
    if (trace_fd < 0) {
      trace_file.reset(OS::CreateEmptyFileWriteOnly(trace_filename));
    } else {
      trace_file.reset(new File(trace_fd, "tracefile"));
      trace_file->DisableAutoClose();
    }
    if (trace_file.get() == nullptr) {
      PLOG(ERROR) << "Unable to open trace file '" << trace_filename << "'";
      ScopedObjectAccess soa(self);
      ThrowRuntimeException("Unable to open trace file '%s'", trace_filename);
      return;
    }
  }

  Runtime* runtime = Runtime::Current();

  // Enable count of allocs if specified in the flags.
  bool enable_stats = false;

  // Create Trace object.
  {
    // Required since EnableMethodTracing calls ConfigureStubs which visits class linker classes.
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != nullptr) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
    } else {
      enable_stats = (flags && kTraceCountAllocs) != 0;
      the_trace_ = new Trace(trace_file.release(), trace_filename, buffer_size, flags, output_mode,
                             trace_mode);
      if (trace_mode == TraceMode::kSampling) {
        CHECK_PTHREAD_CALL(pthread_create, (&sampling_pthread_, nullptr, &RunSamplingThread,
                                            reinterpret_cast<void*>(interval_us)),
                                            "Sampling profiler thread");
        the_trace_->interval_us_ = interval_us;
      } else {
        runtime->GetInstrumentation()->AddListener(the_trace_,
                                                   instrumentation::Instrumentation::kMethodEntered |
                                                   instrumentation::Instrumentation::kMethodExited |
                                                   instrumentation::Instrumentation::kMethodUnwind);
        // TODO: In full-PIC mode, we don't need to fully deopt.
        runtime->GetInstrumentation()->EnableMethodTracing(kTracerInstrumentationKey);
      }
    }
  }

  // Can't call this when holding the mutator lock.
  if (enable_stats) {
    runtime->SetStatsEnabled(true);
  }
}

void Trace::StopTracing(bool finish_tracing, bool flush_file) {
  bool stop_alloc_counting = false;
  Runtime* const runtime = Runtime::Current();
  Trace* the_trace = nullptr;
  Thread* const self = Thread::Current();
  pthread_t sampling_pthread = 0U;
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ == nullptr) {
      LOG(ERROR) << "Trace stop requested, but no trace currently running";
    } else {
      the_trace = the_trace_;
      the_trace_ = nullptr;
      sampling_pthread = sampling_pthread_;
    }
  }
  // Make sure that we join before we delete the trace since we don't want to have
  // the sampling thread access a stale pointer. This finishes since the sampling thread exits when
  // the_trace_ is null.
  if (sampling_pthread != 0U) {
    CHECK_PTHREAD_CALL(pthread_join, (sampling_pthread, nullptr), "sampling thread shutdown");
    sampling_pthread_ = 0U;
  }

  {
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);
    if (the_trace != nullptr) {
      stop_alloc_counting = (the_trace->flags_ & Trace::kTraceCountAllocs) != 0;
      if (finish_tracing) {
        the_trace->FinishTracing();
      }

      if (the_trace->trace_mode_ == TraceMode::kSampling) {
        MutexLock mu(self, *Locks::thread_list_lock_);
        runtime->GetThreadList()->ForEach(ClearThreadStackTraceAndClockBase, nullptr);
      } else {
        runtime->GetInstrumentation()->DisableMethodTracing(kTracerInstrumentationKey);
        runtime->GetInstrumentation()->RemoveListener(
            the_trace, instrumentation::Instrumentation::kMethodEntered |
            instrumentation::Instrumentation::kMethodExited |
            instrumentation::Instrumentation::kMethodUnwind);
      }
      if (the_trace->trace_file_.get() != nullptr) {
        // Do not try to erase, so flush and close explicitly.
        if (flush_file) {
          if (the_trace->trace_file_->Flush() != 0) {
            PLOG(WARNING) << "Could not flush trace file.";
          }
        } else {
          the_trace->trace_file_->MarkUnchecked();  // Do not trigger guard.
        }
        if (the_trace->trace_file_->Close() != 0) {
          PLOG(ERROR) << "Could not close trace file.";
        }
      }
      delete the_trace;
    }
  }
  if (stop_alloc_counting) {
    // Can be racy since SetStatsEnabled is not guarded by any locks.
    runtime->SetStatsEnabled(false);
  }
}

void Trace::Abort() {
  // Do not write anything anymore.
  StopTracing(false, false);
}

void Trace::Stop() {
  // Finish writing.
  StopTracing(true, true);
}

void Trace::Shutdown() {
  if (GetMethodTracingMode() != kTracingInactive) {
    Stop();
  }
}

void Trace::Pause() {
  bool stop_alloc_counting = false;
  Runtime* runtime = Runtime::Current();
  Trace* the_trace = nullptr;

  Thread* const self = Thread::Current();
  pthread_t sampling_pthread = 0U;
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ == nullptr) {
      LOG(ERROR) << "Trace pause requested, but no trace currently running";
      return;
    } else {
      the_trace = the_trace_;
      sampling_pthread = sampling_pthread_;
    }
  }

  if (sampling_pthread != 0U) {
    {
      MutexLock mu(self, *Locks::trace_lock_);
      the_trace_ = nullptr;
    }
    CHECK_PTHREAD_CALL(pthread_join, (sampling_pthread, nullptr), "sampling thread shutdown");
    sampling_pthread_ = 0U;
    {
      MutexLock mu(self, *Locks::trace_lock_);
      the_trace_ = the_trace;
    }
  }

  if (the_trace != nullptr) {
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);
    stop_alloc_counting = (the_trace->flags_ & Trace::kTraceCountAllocs) != 0;

    if (the_trace->trace_mode_ == TraceMode::kSampling) {
      MutexLock mu(self, *Locks::thread_list_lock_);
      runtime->GetThreadList()->ForEach(ClearThreadStackTraceAndClockBase, nullptr);
    } else {
      runtime->GetInstrumentation()->DisableMethodTracing(kTracerInstrumentationKey);
      runtime->GetInstrumentation()->RemoveListener(
          the_trace,
          instrumentation::Instrumentation::kMethodEntered |
          instrumentation::Instrumentation::kMethodExited |
          instrumentation::Instrumentation::kMethodUnwind);
    }
  }

  if (stop_alloc_counting) {
    // Can be racy since SetStatsEnabled is not guarded by any locks.
    Runtime::Current()->SetStatsEnabled(false);
  }
}

void Trace::Resume() {
  Thread* self = Thread::Current();
  Trace* the_trace;
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ == nullptr) {
      LOG(ERROR) << "No trace to resume (or sampling mode), ignoring this request";
      return;
    }
    the_trace = the_trace_;
  }

  Runtime* runtime = Runtime::Current();

  // Enable count of allocs if specified in the flags.
  bool enable_stats = (the_trace->flags_ && kTraceCountAllocs) != 0;

  {
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);

    // Reenable.
    if (the_trace->trace_mode_ == TraceMode::kSampling) {
      CHECK_PTHREAD_CALL(pthread_create, (&sampling_pthread_, nullptr, &RunSamplingThread,
          reinterpret_cast<void*>(the_trace->interval_us_)), "Sampling profiler thread");
    } else {
      runtime->GetInstrumentation()->AddListener(the_trace,
                                                 instrumentation::Instrumentation::kMethodEntered |
                                                 instrumentation::Instrumentation::kMethodExited |
                                                 instrumentation::Instrumentation::kMethodUnwind);
      // TODO: In full-PIC mode, we don't need to fully deopt.
      runtime->GetInstrumentation()->EnableMethodTracing(kTracerInstrumentationKey);
    }
  }

  // Can't call this when holding the mutator lock.
  if (enable_stats) {
    runtime->SetStatsEnabled(true);
  }
}

TracingMode Trace::GetMethodTracingMode() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  if (the_trace_ == nullptr) {
    return kTracingInactive;
  } else {
    switch (the_trace_->trace_mode_) {
      case TraceMode::kSampling:
        return kSampleProfilingActive;
      case TraceMode::kMethodTracing:
        return kMethodTracingActive;
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }
}

static constexpr size_t kMinBufSize = 18U;  // Trace header is up to 18B.

Trace::Trace(File* trace_file, const char* trace_name, size_t buffer_size, int flags,
             TraceOutputMode output_mode, TraceMode trace_mode)
    : trace_file_(trace_file),
      buf_(new uint8_t[std::max(kMinBufSize, buffer_size)]()),
      flags_(flags), trace_output_mode_(output_mode), trace_mode_(trace_mode),
      clock_source_(default_clock_source_),
      buffer_size_(std::max(kMinBufSize, buffer_size)),
      start_time_(MicroTime()), clock_overhead_ns_(GetClockOverheadNanoSeconds()), cur_offset_(0),
      overflow_(false), interval_us_(0), streaming_lock_(nullptr),
      unique_methods_lock_(new Mutex("unique methods lock", kTracingUniqueMethodsLock)) {
  uint16_t trace_version = GetTraceVersion(clock_source_);
  if (output_mode == TraceOutputMode::kStreaming) {
    trace_version |= 0xF0U;
  }
  // Set up the beginning of the trace.
  memset(buf_.get(), 0, kTraceHeaderLength);
  Append4LE(buf_.get(), kTraceMagicValue);
  Append2LE(buf_.get() + 4, trace_version);
  Append2LE(buf_.get() + 6, kTraceHeaderLength);
  Append8LE(buf_.get() + 8, start_time_);
  if (trace_version >= kTraceVersionDualClock) {
    uint16_t record_size = GetRecordSize(clock_source_);
    Append2LE(buf_.get() + 16, record_size);
  }
  static_assert(18 <= kMinBufSize, "Minimum buffer size not large enough for trace header");

  // Update current offset.
  cur_offset_.StoreRelaxed(kTraceHeaderLength);

  if (output_mode == TraceOutputMode::kStreaming) {
    streaming_file_name_ = trace_name;
    streaming_lock_ = new Mutex("tracing lock", LockLevel::kTracingStreamingLock);
    seen_threads_.reset(new ThreadIDBitSet());
  }
}

Trace::~Trace() {
  delete streaming_lock_;
  delete unique_methods_lock_;
}

static uint64_t ReadBytes(uint8_t* buf, size_t bytes) {
  uint64_t ret = 0;
  for (size_t i = 0; i < bytes; ++i) {
    ret |= static_cast<uint64_t>(buf[i]) << (i * 8);
  }
  return ret;
}

void Trace::DumpBuf(uint8_t* buf, size_t buf_size, TraceClockSource clock_source) {
  uint8_t* ptr = buf + kTraceHeaderLength;
  uint8_t* end = buf + buf_size;

  while (ptr < end) {
    uint32_t tmid = ReadBytes(ptr + 2, sizeof(tmid));
    ArtMethod* method = DecodeTraceMethod(tmid);
    TraceAction action = DecodeTraceAction(tmid);
    LOG(INFO) << PrettyMethod(method) << " " << static_cast<int>(action);
    ptr += GetRecordSize(clock_source);
  }
}

static void GetVisitedMethodsFromBitSets(
    const std::map<const DexFile*, DexIndexBitSet*>& seen_methods,
    std::set<ArtMethod*>* visited_methods) SHARED_REQUIRES(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Thread* const self = Thread::Current();
  for (auto& e : seen_methods) {
    DexIndexBitSet* bit_set = e.second;
    // TODO: Visit trace methods as roots.
    mirror::DexCache* dex_cache = class_linker->FindDexCache(self, *e.first, false);
    for (uint32_t i = 0; i < bit_set->size(); ++i) {
      if ((*bit_set)[i]) {
        visited_methods->insert(dex_cache->GetResolvedMethod(i, sizeof(void*)));
      }
    }
  }
}

void Trace::FinishTracing() {
  size_t final_offset = 0;

  std::set<ArtMethod*> visited_methods;
  if (trace_output_mode_ == TraceOutputMode::kStreaming) {
    // Write the secondary file with all the method names.
    GetVisitedMethodsFromBitSets(seen_methods_, &visited_methods);

    // Clean up.
    STLDeleteValues(&seen_methods_);
  } else {
    final_offset = cur_offset_.LoadRelaxed();
    GetVisitedMethods(final_offset, &visited_methods);
  }

  // Compute elapsed time.
  uint64_t elapsed = MicroTime() - start_time_;

  std::ostringstream os;

  os << StringPrintf("%cversion\n", kTraceTokenChar);
  os << StringPrintf("%d\n", GetTraceVersion(clock_source_));
  os << StringPrintf("data-file-overflow=%s\n", overflow_ ? "true" : "false");
  if (UseThreadCpuClock()) {
    if (UseWallClock()) {
      os << StringPrintf("clock=dual\n");
    } else {
      os << StringPrintf("clock=thread-cpu\n");
    }
  } else {
    os << StringPrintf("clock=wall\n");
  }
  os << StringPrintf("elapsed-time-usec=%" PRIu64 "\n", elapsed);
  if (trace_output_mode_ != TraceOutputMode::kStreaming) {
    size_t num_records = (final_offset - kTraceHeaderLength) / GetRecordSize(clock_source_);
    os << StringPrintf("num-method-calls=%zd\n", num_records);
  }
  os << StringPrintf("clock-call-overhead-nsec=%d\n", clock_overhead_ns_);
  os << StringPrintf("vm=art\n");
  os << StringPrintf("pid=%d\n", getpid());
  if ((flags_ & kTraceCountAllocs) != 0) {
    os << StringPrintf("alloc-count=%d\n", Runtime::Current()->GetStat(KIND_ALLOCATED_OBJECTS));
    os << StringPrintf("alloc-size=%d\n", Runtime::Current()->GetStat(KIND_ALLOCATED_BYTES));
    os << StringPrintf("gc-count=%d\n", Runtime::Current()->GetStat(KIND_GC_INVOCATIONS));
  }
  os << StringPrintf("%cthreads\n", kTraceTokenChar);
  DumpThreadList(os);
  os << StringPrintf("%cmethods\n", kTraceTokenChar);
  DumpMethodList(os, visited_methods);
  os << StringPrintf("%cend\n", kTraceTokenChar);
  std::string header(os.str());

  if (trace_output_mode_ == TraceOutputMode::kStreaming) {
    File file;
    if (!file.Open(streaming_file_name_ + ".sec", O_CREAT | O_WRONLY)) {
      LOG(WARNING) << "Could not open secondary trace file!";
      return;
    }
    if (!file.WriteFully(header.c_str(), header.length())) {
      file.Erase();
      std::string detail(StringPrintf("Trace data write failed: %s", strerror(errno)));
      PLOG(ERROR) << detail;
      ThrowRuntimeException("%s", detail.c_str());
    }
    if (file.FlushCloseOrErase() != 0) {
      PLOG(ERROR) << "Could not write secondary file";
    }
  } else {
    if (trace_file_.get() == nullptr) {
      iovec iov[2];
      iov[0].iov_base = reinterpret_cast<void*>(const_cast<char*>(header.c_str()));
      iov[0].iov_len = header.length();
      iov[1].iov_base = buf_.get();
      iov[1].iov_len = final_offset;
      Dbg::DdmSendChunkV(CHUNK_TYPE("MPSE"), iov, 2);
      const bool kDumpTraceInfo = false;
      if (kDumpTraceInfo) {
        LOG(INFO) << "Trace sent:\n" << header;
        DumpBuf(buf_.get(), final_offset, clock_source_);
      }
    } else {
      if (!trace_file_->WriteFully(header.c_str(), header.length()) ||
          !trace_file_->WriteFully(buf_.get(), final_offset)) {
        std::string detail(StringPrintf("Trace data write failed: %s", strerror(errno)));
        PLOG(ERROR) << detail;
        ThrowRuntimeException("%s", detail.c_str());
      }
    }
  }
}

void Trace::DexPcMoved(Thread* thread ATTRIBUTE_UNUSED,
                       mirror::Object* this_object ATTRIBUTE_UNUSED,
                       ArtMethod* method,
                       uint32_t new_dex_pc) {
  // We're not recorded to listen to this kind of event, so complain.
  LOG(ERROR) << "Unexpected dex PC event in tracing " << PrettyMethod(method) << " " << new_dex_pc;
}

void Trace::FieldRead(Thread* thread ATTRIBUTE_UNUSED,
                      mirror::Object* this_object ATTRIBUTE_UNUSED,
                      ArtMethod* method,
                      uint32_t dex_pc,
                      ArtField* field ATTRIBUTE_UNUSED)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // We're not recorded to listen to this kind of event, so complain.
  LOG(ERROR) << "Unexpected field read event in tracing " << PrettyMethod(method) << " " << dex_pc;
}

void Trace::FieldWritten(Thread* thread ATTRIBUTE_UNUSED,
                         mirror::Object* this_object ATTRIBUTE_UNUSED,
                         ArtMethod* method,
                         uint32_t dex_pc,
                         ArtField* field ATTRIBUTE_UNUSED,
                         const JValue& field_value ATTRIBUTE_UNUSED)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // We're not recorded to listen to this kind of event, so complain.
  LOG(ERROR) << "Unexpected field write event in tracing " << PrettyMethod(method) << " " << dex_pc;
}

void Trace::MethodEntered(Thread* thread, mirror::Object* this_object ATTRIBUTE_UNUSED,
                          ArtMethod* method, uint32_t dex_pc ATTRIBUTE_UNUSED) {
  uint32_t thread_clock_diff = 0;
  uint32_t wall_clock_diff = 0;
  ReadClocks(thread, &thread_clock_diff, &wall_clock_diff);
  LogMethodTraceEvent(thread, method, instrumentation::Instrumentation::kMethodEntered,
                      thread_clock_diff, wall_clock_diff);
}

void Trace::MethodExited(Thread* thread, mirror::Object* this_object ATTRIBUTE_UNUSED,
                         ArtMethod* method, uint32_t dex_pc ATTRIBUTE_UNUSED,
                         const JValue& return_value ATTRIBUTE_UNUSED) {
  uint32_t thread_clock_diff = 0;
  uint32_t wall_clock_diff = 0;
  ReadClocks(thread, &thread_clock_diff, &wall_clock_diff);
  LogMethodTraceEvent(thread, method, instrumentation::Instrumentation::kMethodExited,
                      thread_clock_diff, wall_clock_diff);
}

void Trace::MethodUnwind(Thread* thread, mirror::Object* this_object ATTRIBUTE_UNUSED,
                         ArtMethod* method, uint32_t dex_pc ATTRIBUTE_UNUSED) {
  uint32_t thread_clock_diff = 0;
  uint32_t wall_clock_diff = 0;
  ReadClocks(thread, &thread_clock_diff, &wall_clock_diff);
  LogMethodTraceEvent(thread, method, instrumentation::Instrumentation::kMethodUnwind,
                      thread_clock_diff, wall_clock_diff);
}

void Trace::ExceptionCaught(Thread* thread ATTRIBUTE_UNUSED,
                            mirror::Throwable* exception_object ATTRIBUTE_UNUSED)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected exception caught event in tracing";
}

void Trace::Branch(Thread* /*thread*/, ArtMethod* method,
                   uint32_t /*dex_pc*/, int32_t /*dex_pc_offset*/)
      SHARED_REQUIRES(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected branch event in tracing" << PrettyMethod(method);
}

void Trace::InvokeVirtualOrInterface(Thread*,
                                     mirror::Object*,
                                     ArtMethod* method,
                                     uint32_t dex_pc,
                                     ArtMethod*) {
  LOG(ERROR) << "Unexpected invoke event in tracing" << PrettyMethod(method)
             << " " << dex_pc;
}

void Trace::ReadClocks(Thread* thread, uint32_t* thread_clock_diff, uint32_t* wall_clock_diff) {
  if (UseThreadCpuClock()) {
    uint64_t clock_base = thread->GetTraceClockBase();
    if (UNLIKELY(clock_base == 0)) {
      // First event, record the base time in the map.
      uint64_t time = thread->GetCpuMicroTime();
      thread->SetTraceClockBase(time);
    } else {
      *thread_clock_diff = thread->GetCpuMicroTime() - clock_base;
    }
  }
  if (UseWallClock()) {
    *wall_clock_diff = MicroTime() - start_time_;
  }
}

bool Trace::RegisterMethod(ArtMethod* method) {
  mirror::DexCache* dex_cache = method->GetDexCache();
  const DexFile* dex_file = dex_cache->GetDexFile();
  auto* resolved_method = dex_cache->GetResolvedMethod(method->GetDexMethodIndex(), sizeof(void*));
  if (resolved_method != method) {
    DCHECK(resolved_method == nullptr);
    dex_cache->SetResolvedMethod(method->GetDexMethodIndex(), method, sizeof(void*));
  }
  if (seen_methods_.find(dex_file) == seen_methods_.end()) {
    seen_methods_.insert(std::make_pair(dex_file, new DexIndexBitSet()));
  }
  DexIndexBitSet* bit_set = seen_methods_.find(dex_file)->second;
  if (!(*bit_set)[method->GetDexMethodIndex()]) {
    bit_set->set(method->GetDexMethodIndex());
    return true;
  }
  return false;
}

bool Trace::RegisterThread(Thread* thread) {
  pid_t tid = thread->GetTid();
  CHECK_LT(0U, static_cast<uint32_t>(tid));
  CHECK_LT(static_cast<uint32_t>(tid), 65536U);

  if (!(*seen_threads_)[tid]) {
    seen_threads_->set(tid);
    return true;
  }
  return false;
}

std::string Trace::GetMethodLine(ArtMethod* method) {
  method = method->GetInterfaceMethodIfProxy(sizeof(void*));
  return StringPrintf("%p\t%s\t%s\t%s\t%s\n",
                      reinterpret_cast<void*>((EncodeTraceMethod(method) << TraceActionBits)),
      PrettyDescriptor(method->GetDeclaringClassDescriptor()).c_str(), method->GetName(),
      method->GetSignature().ToString().c_str(), method->GetDeclaringClassSourceFile());
}

void Trace::WriteToBuf(const uint8_t* src, size_t src_size) {
  int32_t old_offset = cur_offset_.LoadRelaxed();
  int32_t new_offset = old_offset + static_cast<int32_t>(src_size);
  if (dchecked_integral_cast<size_t>(new_offset) > buffer_size_) {
    // Flush buffer.
    if (!trace_file_->WriteFully(buf_.get(), old_offset)) {
      PLOG(WARNING) << "Failed streaming a tracing event.";
    }

    // Check whether the data is too large for the buffer, then write immediately.
    if (src_size >= buffer_size_) {
      if (!trace_file_->WriteFully(src, src_size)) {
        PLOG(WARNING) << "Failed streaming a tracing event.";
      }
      cur_offset_.StoreRelease(0);  // Buffer is empty now.
      return;
    }

    old_offset = 0;
    new_offset = static_cast<int32_t>(src_size);
  }
  cur_offset_.StoreRelease(new_offset);
  // Fill in data.
  memcpy(buf_.get() + old_offset, src, src_size);
}

void Trace::LogMethodTraceEvent(Thread* thread, ArtMethod* method,
                                instrumentation::Instrumentation::InstrumentationEvent event,
                                uint32_t thread_clock_diff, uint32_t wall_clock_diff) {
  // Advance cur_offset_ atomically.
  int32_t new_offset;
  int32_t old_offset = 0;

  // We do a busy loop here trying to acquire the next offset.
  if (trace_output_mode_ != TraceOutputMode::kStreaming) {
    do {
      old_offset = cur_offset_.LoadRelaxed();
      new_offset = old_offset + GetRecordSize(clock_source_);
      if (static_cast<size_t>(new_offset) > buffer_size_) {
        overflow_ = true;
        return;
      }
    } while (!cur_offset_.CompareExchangeWeakSequentiallyConsistent(old_offset, new_offset));
  }

  TraceAction action = kTraceMethodEnter;
  switch (event) {
    case instrumentation::Instrumentation::kMethodEntered:
      action = kTraceMethodEnter;
      break;
    case instrumentation::Instrumentation::kMethodExited:
      action = kTraceMethodExit;
      break;
    case instrumentation::Instrumentation::kMethodUnwind:
      action = kTraceUnroll;
      break;
    default:
      UNIMPLEMENTED(FATAL) << "Unexpected event: " << event;
  }

  uint32_t method_value = EncodeTraceMethodAndAction(method, action);

  // Write data
  uint8_t* ptr;
  static constexpr size_t kPacketSize = 14U;  // The maximum size of data in a packet.
  uint8_t stack_buf[kPacketSize];             // Space to store a packet when in streaming mode.
  if (trace_output_mode_ == TraceOutputMode::kStreaming) {
    ptr = stack_buf;
  } else {
    ptr = buf_.get() + old_offset;
  }

  Append2LE(ptr, thread->GetTid());
  Append4LE(ptr + 2, method_value);
  ptr += 6;

  if (UseThreadCpuClock()) {
    Append4LE(ptr, thread_clock_diff);
    ptr += 4;
  }
  if (UseWallClock()) {
    Append4LE(ptr, wall_clock_diff);
  }
  static_assert(kPacketSize == 2 + 4 + 4 + 4, "Packet size incorrect.");

  if (trace_output_mode_ == TraceOutputMode::kStreaming) {
    MutexLock mu(Thread::Current(), *streaming_lock_);  // To serialize writing.
    if (RegisterMethod(method)) {
      // Write a special block with the name.
      std::string method_line(GetMethodLine(method));
      uint8_t buf2[5];
      Append2LE(buf2, 0);
      buf2[2] = kOpNewMethod;
      Append2LE(buf2 + 3, static_cast<uint16_t>(method_line.length()));
      WriteToBuf(buf2, sizeof(buf2));
      WriteToBuf(reinterpret_cast<const uint8_t*>(method_line.c_str()), method_line.length());
    }
    if (RegisterThread(thread)) {
      // It might be better to postpone this. Threads might not have received names...
      std::string thread_name;
      thread->GetThreadName(thread_name);
      uint8_t buf2[7];
      Append2LE(buf2, 0);
      buf2[2] = kOpNewThread;
      Append2LE(buf2 + 3, static_cast<uint16_t>(thread->GetTid()));
      Append2LE(buf2 + 5, static_cast<uint16_t>(thread_name.length()));
      WriteToBuf(buf2, sizeof(buf2));
      WriteToBuf(reinterpret_cast<const uint8_t*>(thread_name.c_str()), thread_name.length());
    }
    WriteToBuf(stack_buf, sizeof(stack_buf));
  }
}

void Trace::GetVisitedMethods(size_t buf_size,
                              std::set<ArtMethod*>* visited_methods) {
  uint8_t* ptr = buf_.get() + kTraceHeaderLength;
  uint8_t* end = buf_.get() + buf_size;

  while (ptr < end) {
    uint32_t tmid = ReadBytes(ptr + 2, sizeof(tmid));
    ArtMethod* method = DecodeTraceMethod(tmid);
    visited_methods->insert(method);
    ptr += GetRecordSize(clock_source_);
  }
}

void Trace::DumpMethodList(std::ostream& os, const std::set<ArtMethod*>& visited_methods) {
  for (const auto& method : visited_methods) {
    os << GetMethodLine(method);
  }
}

static void DumpThread(Thread* t, void* arg) {
  std::ostream& os = *reinterpret_cast<std::ostream*>(arg);
  std::string name;
  t->GetThreadName(name);
  os << t->GetTid() << "\t" << name << "\n";
}

void Trace::DumpThreadList(std::ostream& os) {
  Thread* self = Thread::Current();
  for (auto it : exited_threads_) {
    os << it.first << "\t" << it.second << "\n";
  }
  Locks::thread_list_lock_->AssertNotHeld(self);
  MutexLock mu(self, *Locks::thread_list_lock_);
  Runtime::Current()->GetThreadList()->ForEach(DumpThread, &os);
}

void Trace::StoreExitingThreadInfo(Thread* thread) {
  MutexLock mu(thread, *Locks::trace_lock_);
  if (the_trace_ != nullptr) {
    std::string name;
    thread->GetThreadName(name);
    // The same thread/tid may be used multiple times. As SafeMap::Put does not allow to override
    // a previous mapping, use SafeMap::Overwrite.
    the_trace_->exited_threads_.Overwrite(thread->GetTid(), name);
  }
}

Trace::TraceOutputMode Trace::GetOutputMode() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace output mode requested, but no trace currently running";
  return the_trace_->trace_output_mode_;
}

Trace::TraceMode Trace::GetMode() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace mode requested, but no trace currently running";
  return the_trace_->trace_mode_;
}

size_t Trace::GetBufferSize() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  CHECK(the_trace_ != nullptr) << "Trace mode requested, but no trace currently running";
  return the_trace_->buffer_size_;
}

bool Trace::IsTracingEnabled() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  return the_trace_ != nullptr;
}

}  // namespace art
