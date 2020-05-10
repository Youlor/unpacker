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

#include "thread_list.h"

#include <backtrace/BacktraceMap.h>
#include <dirent.h>
#include <ScopedLocalRef.h>
#include <ScopedUtfChars.h>
#include <sys/types.h>
#include <unistd.h>

#include <sstream>

#include "base/histogram-inl.h"
#include "base/mutex-inl.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "debugger.h"
#include "gc/collector/concurrent_copying.h"
#include "jni_internal.h"
#include "lock_word.h"
#include "monitor.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "trace.h"
#include "well_known_classes.h"

#if ART_USE_FUTEXES
#include "linux/futex.h"
#include "sys/syscall.h"
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif
#endif  // ART_USE_FUTEXES

namespace art {

static constexpr uint64_t kLongThreadSuspendThreshold = MsToNs(5);
static constexpr uint64_t kThreadSuspendTimeoutMs = 30 * 1000;  // 30s.
// Use 0 since we want to yield to prevent blocking for an unpredictable amount of time.
static constexpr useconds_t kThreadSuspendInitialSleepUs = 0;
static constexpr useconds_t kThreadSuspendMaxYieldUs = 3000;
static constexpr useconds_t kThreadSuspendMaxSleepUs = 5000;

// Whether we should try to dump the native stack of unattached threads. See commit ed8b723 for
// some history.
// Turned off again. b/29248079
static constexpr bool kDumpUnattachedThreadNativeStack = false;

ThreadList::ThreadList()
    : suspend_all_count_(0),
      debug_suspend_all_count_(0),
      unregistering_count_(0),
      suspend_all_historam_("suspend all histogram", 16, 64),
      long_suspend_(false) {
  CHECK(Monitor::IsValidLockWord(LockWord::FromThinLockId(kMaxThreadId, 1, 0U)));
}

ThreadList::~ThreadList() {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  // Detach the current thread if necessary. If we failed to start, there might not be any threads.
  // We need to detach the current thread here in case there's another thread waiting to join with
  // us.
  bool contains = false;
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    contains = Contains(self);
  }
  if (contains) {
    Runtime::Current()->DetachCurrentThread();
  }
  WaitForOtherNonDaemonThreadsToExit();
  // Disable GC and wait for GC to complete in case there are still daemon threads doing
  // allocations.
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  heap->DisableGCForShutdown();
  // In case a GC is in progress, wait for it to finish.
  heap->WaitForGcToComplete(gc::kGcCauseBackground, Thread::Current());
  // TODO: there's an unaddressed race here where a thread may attach during shutdown, see
  //       Thread::Init.
  SuspendAllDaemonThreadsForShutdown();
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

bool ThreadList::Contains(pid_t tid) {
  for (const auto& thread : list_) {
    if (thread->GetTid() == tid) {
      return true;
    }
  }
  return false;
}

pid_t ThreadList::GetLockOwner() {
  return Locks::thread_list_lock_->GetExclusiveOwnerTid();
}

void ThreadList::DumpNativeStacks(std::ostream& os) {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(getpid()));
  for (const auto& thread : list_) {
    os << "DUMPING THREAD " << thread->GetTid() << "\n";
    DumpNativeStack(os, thread->GetTid(), map.get(), "\t");
    os << "\n";
  }
}

void ThreadList::DumpForSigQuit(std::ostream& os) {
  {
    ScopedObjectAccess soa(Thread::Current());
    // Only print if we have samples.
    if (suspend_all_historam_.SampleSize() > 0) {
      Histogram<uint64_t>::CumulativeData data;
      suspend_all_historam_.CreateHistogram(&data);
      suspend_all_historam_.PrintConfidenceIntervals(os, 0.99, data);  // Dump time to suspend.
    }
  }
  bool dump_native_stack = Runtime::Current()->GetDumpNativeStackOnSigQuit();
  Dump(os, dump_native_stack);
  DumpUnattachedThreads(os, dump_native_stack);
}

static void DumpUnattachedThread(std::ostream& os, pid_t tid, bool dump_native_stack)
    NO_THREAD_SAFETY_ANALYSIS {
  // TODO: No thread safety analysis as DumpState with a null thread won't access fields, should
  // refactor DumpState to avoid skipping analysis.
  Thread::DumpState(os, nullptr, tid);
  DumpKernelStack(os, tid, "  kernel: ", false);
  if (dump_native_stack && kDumpUnattachedThreadNativeStack) {
    DumpNativeStack(os, tid, nullptr, "  native: ");
  }
  os << "\n";
}

void ThreadList::DumpUnattachedThreads(std::ostream& os, bool dump_native_stack) {
  DIR* d = opendir("/proc/self/task");
  if (!d) {
    return;
  }

  Thread* self = Thread::Current();
  dirent* e;
  while ((e = readdir(d)) != nullptr) {
    char* end;
    pid_t tid = strtol(e->d_name, &end, 10);
    if (!*end) {
      bool contains;
      {
        MutexLock mu(self, *Locks::thread_list_lock_);
        contains = Contains(tid);
      }
      if (!contains) {
        DumpUnattachedThread(os, tid, dump_native_stack);
      }
    }
  }
  closedir(d);
}

// Dump checkpoint timeout in milliseconds. Larger amount on the target, since the device could be
// overloaded with ANR dumps.
static constexpr uint32_t kDumpWaitTimeout = kIsTargetBuild ? 100000 : 20000;

// A closure used by Thread::Dump.
class DumpCheckpoint FINAL : public Closure {
 public:
  DumpCheckpoint(std::ostream* os, bool dump_native_stack)
      : os_(os),
        barrier_(0),
        backtrace_map_(dump_native_stack ? BacktraceMap::Create(getpid()) : nullptr),
        dump_native_stack_(dump_native_stack) {}

  void Run(Thread* thread) OVERRIDE {
    // Note thread and self may not be equal if thread was already suspended at the point of the
    // request.
    Thread* self = Thread::Current();
    std::ostringstream local_os;
    {
      ScopedObjectAccess soa(self);
      thread->Dump(local_os, dump_native_stack_, backtrace_map_.get());
    }
    local_os << "\n";
    {
      // Use the logging lock to ensure serialization when writing to the common ostream.
      MutexLock mu(self, *Locks::logging_lock_);
      *os_ << local_os.str();
    }
    barrier_.Pass(self);
  }

  void WaitForThreadsToRunThroughCheckpoint(size_t threads_running_checkpoint) {
    Thread* self = Thread::Current();
    ScopedThreadStateChange tsc(self, kWaitingForCheckPointsToRun);
    bool timed_out = barrier_.Increment(self, threads_running_checkpoint, kDumpWaitTimeout);
    if (timed_out) {
      // Avoid a recursive abort.
      LOG((kIsDebugBuild && (gAborting == 0)) ? FATAL : ERROR)
          << "Unexpected time out during dump checkpoint.";
    }
  }

 private:
  // The common stream that will accumulate all the dumps.
  std::ostream* const os_;
  // The barrier to be passed through and for the requestor to wait upon.
  Barrier barrier_;
  // A backtrace map, so that all threads use a shared info and don't reacquire/parse separately.
  std::unique_ptr<BacktraceMap> backtrace_map_;
  // Whether we should dump the native stack.
  const bool dump_native_stack_;
};

void ThreadList::Dump(std::ostream& os, bool dump_native_stack) {
  {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    os << "DALVIK THREADS (" << list_.size() << "):\n";
  }
  DumpCheckpoint checkpoint(&os, dump_native_stack);
  size_t threads_running_checkpoint;
  {
    // Use SOA to prevent deadlocks if multiple threads are calling Dump() at the same time.
    ScopedObjectAccess soa(Thread::Current());
    threads_running_checkpoint = RunCheckpoint(&checkpoint);
  }
  if (threads_running_checkpoint != 0) {
    checkpoint.WaitForThreadsToRunThroughCheckpoint(threads_running_checkpoint);
  }
}

void ThreadList::AssertThreadsAreSuspended(Thread* self, Thread* ignore1, Thread* ignore2) {
  MutexLock mu(self, *Locks::thread_list_lock_);
  MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
  for (const auto& thread : list_) {
    if (thread != ignore1 && thread != ignore2) {
      CHECK(thread->IsSuspended())
            << "\nUnsuspended thread: <<" << *thread << "\n"
            << "self: <<" << *Thread::Current();
    }
  }
}

#if HAVE_TIMED_RWLOCK
// Attempt to rectify locks so that we dump thread list with required locks before exiting.
NO_RETURN static void UnsafeLogFatalForThreadSuspendAllTimeout() {
  Runtime* runtime = Runtime::Current();
  std::ostringstream ss;
  ss << "Thread suspend timeout\n";
  Locks::mutator_lock_->Dump(ss);
  ss << "\n";
  runtime->GetThreadList()->Dump(ss);
  LOG(FATAL) << ss.str();
  exit(0);
}
#endif

// Unlike suspending all threads where we can wait to acquire the mutator_lock_, suspending an
// individual thread requires polling. delay_us is the requested sleep wait. If delay_us is 0 then
// we use sched_yield instead of calling usleep.
static void ThreadSuspendSleep(useconds_t delay_us) {
  if (delay_us == 0) {
    sched_yield();
  } else {
    usleep(delay_us);
  }
}

size_t ThreadList::RunCheckpoint(Closure* checkpoint_function) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);

  std::vector<Thread*> suspended_count_modified_threads;
  size_t count = 0;
  {
    // Call a checkpoint function for each thread, threads which are suspend get their checkpoint
    // manually called.
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    count = list_.size();
    for (const auto& thread : list_) {
      if (thread != self) {
        while (true) {
          if (thread->RequestCheckpoint(checkpoint_function)) {
            // This thread will run its checkpoint some time in the near future.
            break;
          } else {
            // We are probably suspended, try to make sure that we stay suspended.
            // The thread switched back to runnable.
            if (thread->GetState() == kRunnable) {
              // Spurious fail, try again.
              continue;
            }
            thread->ModifySuspendCount(self, +1, nullptr, false);
            suspended_count_modified_threads.push_back(thread);
            break;
          }
        }
      }
    }
  }

  // Run the checkpoint on ourself while we wait for threads to suspend.
  checkpoint_function->Run(self);

  // Run the checkpoint on the suspended threads.
  for (const auto& thread : suspended_count_modified_threads) {
    if (!thread->IsSuspended()) {
      if (ATRACE_ENABLED()) {
        std::ostringstream oss;
        thread->ShortDump(oss);
        ATRACE_BEGIN((std::string("Waiting for suspension of thread ") + oss.str()).c_str());
      }
      // Busy wait until the thread is suspended.
      const uint64_t start_time = NanoTime();
      do {
        ThreadSuspendSleep(kThreadSuspendInitialSleepUs);
      } while (!thread->IsSuspended());
      const uint64_t total_delay = NanoTime() - start_time;
      // Shouldn't need to wait for longer than 1000 microseconds.
      constexpr uint64_t kLongWaitThreshold = MsToNs(1);
      ATRACE_END();
      if (UNLIKELY(total_delay > kLongWaitThreshold)) {
        LOG(WARNING) << "Long wait of " << PrettyDuration(total_delay) << " for "
            << *thread << " suspension!";
      }
    }
    // We know for sure that the thread is suspended at this point.
    checkpoint_function->Run(thread);
    {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      thread->ModifySuspendCount(self, -1, nullptr, false);
    }
  }

  {
    // Imitate ResumeAll, threads may be waiting on Thread::resume_cond_ since we raised their
    // suspend count. Now the suspend_count_ is lowered so we must do the broadcast.
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  return count;
}

// Request that a checkpoint function be run on all active (non-suspended)
// threads.  Returns the number of successful requests.
size_t ThreadList::RunCheckpointOnRunnableThreads(Closure* checkpoint_function) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  CHECK_NE(self->GetState(), kRunnable);

  size_t count = 0;
  {
    // Call a checkpoint function for each non-suspended thread.
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : list_) {
      if (thread != self) {
        if (thread->RequestCheckpoint(checkpoint_function)) {
          // This thread will run its checkpoint some time in the near future.
          count++;
        }
      }
    }
  }

  // Return the number of threads that will run the checkpoint function.
  return count;
}

// A checkpoint/suspend-all hybrid to switch thread roots from
// from-space to to-space refs. Used to synchronize threads at a point
// to mark the initiation of marking while maintaining the to-space
// invariant.
size_t ThreadList::FlipThreadRoots(Closure* thread_flip_visitor,
                                   Closure* flip_callback,
                                   gc::collector::GarbageCollector* collector) {
  TimingLogger::ScopedTiming split("ThreadListFlip", collector->GetTimings());
  const uint64_t start_time = NanoTime();
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  CHECK_NE(self->GetState(), kRunnable);

  SuspendAllInternal(self, self, nullptr);

  // Run the flip callback for the collector.
  Locks::mutator_lock_->ExclusiveLock(self);
  flip_callback->Run(self);
  Locks::mutator_lock_->ExclusiveUnlock(self);
  collector->RegisterPause(NanoTime() - start_time);

  // Resume runnable threads.
  std::vector<Thread*> runnable_threads;
  std::vector<Thread*> other_threads;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    --suspend_all_count_;
    for (const auto& thread : list_) {
      if (thread == self) {
        continue;
      }
      // Set the flip function for both runnable and suspended threads
      // because Thread::DumpState/DumpJavaStack() (invoked by a
      // checkpoint) may cause the flip function to be run for a
      // runnable/suspended thread before a runnable threads runs it
      // for itself or we run it for a suspended thread below.
      thread->SetFlipFunction(thread_flip_visitor);
      if (thread->IsSuspendedAtSuspendCheck()) {
        // The thread will resume right after the broadcast.
        thread->ModifySuspendCount(self, -1, nullptr, false);
        runnable_threads.push_back(thread);
      } else {
        other_threads.push_back(thread);
      }
    }
    Thread::resume_cond_->Broadcast(self);
  }

  // Run the closure on the other threads and let them resume.
  {
    ReaderMutexLock mu(self, *Locks::mutator_lock_);
    for (const auto& thread : other_threads) {
      Closure* flip_func = thread->GetFlipFunction();
      if (flip_func != nullptr) {
        flip_func->Run(thread);
      }
    }
    // Run it for self.
    thread_flip_visitor->Run(self);
  }

  // Resume other threads.
  {
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : other_threads) {
      thread->ModifySuspendCount(self, -1, nullptr, false);
    }
    Thread::resume_cond_->Broadcast(self);
  }

  return runnable_threads.size() + other_threads.size() + 1;  // +1 for self.
}

void ThreadList::SuspendAll(const char* cause, bool long_suspend) {
  Thread* self = Thread::Current();

  if (self != nullptr) {
    VLOG(threads) << *self << " SuspendAll for " << cause << " starting...";
  } else {
    VLOG(threads) << "Thread[null] SuspendAll for " << cause << " starting...";
  }
  {
    ScopedTrace trace("Suspending mutator threads");
    const uint64_t start_time = NanoTime();

    SuspendAllInternal(self, self);
    // All threads are known to have suspended (but a thread may still own the mutator lock)
    // Make sure this thread grabs exclusive access to the mutator lock and its protected data.
#if HAVE_TIMED_RWLOCK
    while (true) {
      if (Locks::mutator_lock_->ExclusiveLockWithTimeout(self, kThreadSuspendTimeoutMs, 0)) {
        break;
      } else if (!long_suspend_) {
        // Reading long_suspend without the mutator lock is slightly racy, in some rare cases, this
        // could result in a thread suspend timeout.
        // Timeout if we wait more than kThreadSuspendTimeoutMs seconds.
        UnsafeLogFatalForThreadSuspendAllTimeout();
      }
    }
#else
    Locks::mutator_lock_->ExclusiveLock(self);
#endif

    long_suspend_ = long_suspend;

    const uint64_t end_time = NanoTime();
    const uint64_t suspend_time = end_time - start_time;
    suspend_all_historam_.AdjustAndAddValue(suspend_time);
    if (suspend_time > kLongThreadSuspendThreshold) {
      LOG(WARNING) << "Suspending all threads took: " << PrettyDuration(suspend_time);
    }

    if (kDebugLocking) {
      // Debug check that all threads are suspended.
      AssertThreadsAreSuspended(self, self);
    }
  }
  ATRACE_BEGIN((std::string("Mutator threads suspended for ") + cause).c_str());

  if (self != nullptr) {
    VLOG(threads) << *self << " SuspendAll complete";
  } else {
    VLOG(threads) << "Thread[null] SuspendAll complete";
  }
}

// Ensures all threads running Java suspend and that those not running Java don't start.
// Debugger thread might be set to kRunnable for a short period of time after the
// SuspendAllInternal. This is safe because it will be set back to suspended state before
// the SuspendAll returns.
void ThreadList::SuspendAllInternal(Thread* self,
                                    Thread* ignore1,
                                    Thread* ignore2,
                                    bool debug_suspend) {
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  if (kDebugLocking && self != nullptr) {
    CHECK_NE(self->GetState(), kRunnable);
  }

  // First request that all threads suspend, then wait for them to suspend before
  // returning. This suspension scheme also relies on other behaviour:
  // 1. Threads cannot be deleted while they are suspended or have a suspend-
  //    request flag set - (see Unregister() below).
  // 2. When threads are created, they are created in a suspended state (actually
  //    kNative) and will never begin executing Java code without first checking
  //    the suspend-request flag.

  // The atomic counter for number of threads that need to pass the barrier.
  AtomicInteger pending_threads;
  uint32_t num_ignored = 0;
  if (ignore1 != nullptr) {
    ++num_ignored;
  }
  if (ignore2 != nullptr && ignore1 != ignore2) {
    ++num_ignored;
  }
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    ++suspend_all_count_;
    if (debug_suspend)
      ++debug_suspend_all_count_;
    pending_threads.StoreRelaxed(list_.size() - num_ignored);
    // Increment everybody's suspend count (except those that should be ignored).
    for (const auto& thread : list_) {
      if (thread == ignore1 || thread == ignore2) {
        continue;
      }
      VLOG(threads) << "requesting thread suspend: " << *thread;
      while (true) {
        if (LIKELY(thread->ModifySuspendCount(self, +1, &pending_threads, debug_suspend))) {
          break;
        } else {
          // Failure means the list of active_suspend_barriers is full, we should release the
          // thread_suspend_count_lock_ (to avoid deadlock) and wait till the target thread has
          // executed Thread::PassActiveSuspendBarriers(). Note that we could not simply wait for
          // the thread to change to a suspended state, because it might need to run checkpoint
          // function before the state change, which also needs thread_suspend_count_lock_.

          // This is very unlikely to happen since more than kMaxSuspendBarriers threads need to
          // execute SuspendAllInternal() simultaneously, and target thread stays in kRunnable
          // in the mean time.
          Locks::thread_suspend_count_lock_->ExclusiveUnlock(self);
          NanoSleep(100000);
          Locks::thread_suspend_count_lock_->ExclusiveLock(self);
        }
      }

      // Must install the pending_threads counter first, then check thread->IsSuspend() and clear
      // the counter. Otherwise there's a race with Thread::TransitionFromRunnableToSuspended()
      // that can lead a thread to miss a call to PassActiveSuspendBarriers().
      if (thread->IsSuspended()) {
        // Only clear the counter for the current thread.
        thread->ClearSuspendBarrier(&pending_threads);
        pending_threads.FetchAndSubSequentiallyConsistent(1);
      }
    }
  }

  // Wait for the barrier to be passed by all runnable threads. This wait
  // is done with a timeout so that we can detect problems.
#if ART_USE_FUTEXES
  timespec wait_timeout;
  InitTimeSpec(true, CLOCK_MONOTONIC, 10000, 0, &wait_timeout);
#endif
  while (true) {
    int32_t cur_val = pending_threads.LoadRelaxed();
    if (LIKELY(cur_val > 0)) {
#if ART_USE_FUTEXES
      if (futex(pending_threads.Address(), FUTEX_WAIT, cur_val, &wait_timeout, nullptr, 0) != 0) {
        // EAGAIN and EINTR both indicate a spurious failure, try again from the beginning.
        if ((errno != EAGAIN) && (errno != EINTR)) {
          if (errno == ETIMEDOUT) {
            LOG(kIsDebugBuild ? FATAL : ERROR) << "Unexpected time out during suspend all.";
          } else {
            PLOG(FATAL) << "futex wait failed for SuspendAllInternal()";
          }
        }
      } else {
        cur_val = pending_threads.LoadRelaxed();
        CHECK_EQ(cur_val, 0);
        break;
      }
#else
      // Spin wait. This is likely to be slow, but on most architecture ART_USE_FUTEXES is set.
#endif
    } else {
      CHECK_EQ(cur_val, 0);
      break;
    }
  }
}

void ThreadList::ResumeAll() {
  Thread* self = Thread::Current();

  if (self != nullptr) {
    VLOG(threads) << *self << " ResumeAll starting";
  } else {
    VLOG(threads) << "Thread[null] ResumeAll starting";
  }

  ATRACE_END();

  ScopedTrace trace("Resuming mutator threads");

  if (kDebugLocking) {
    // Debug check that all threads are suspended.
    AssertThreadsAreSuspended(self, self);
  }

  long_suspend_ = false;

  Locks::mutator_lock_->ExclusiveUnlock(self);
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    --suspend_all_count_;
    // Decrement the suspend counts for all threads.
    for (const auto& thread : list_) {
      if (thread == self) {
        continue;
      }
      thread->ModifySuspendCount(self, -1, nullptr, false);
    }

    // Broadcast a notification to all suspended threads, some or all of
    // which may choose to wake up.  No need to wait for them.
    if (self != nullptr) {
      VLOG(threads) << *self << " ResumeAll waking others";
    } else {
      VLOG(threads) << "Thread[null] ResumeAll waking others";
    }
    Thread::resume_cond_->Broadcast(self);
  }

  if (self != nullptr) {
    VLOG(threads) << *self << " ResumeAll complete";
  } else {
    VLOG(threads) << "Thread[null] ResumeAll complete";
  }
}

void ThreadList::Resume(Thread* thread, bool for_debugger) {
  // This assumes there was an ATRACE_BEGIN when we suspended the thread.
  ATRACE_END();

  Thread* self = Thread::Current();
  DCHECK_NE(thread, self);
  VLOG(threads) << "Resume(" << reinterpret_cast<void*>(thread) << ") starting..."
      << (for_debugger ? " (debugger)" : "");

  {
    // To check Contains.
    MutexLock mu(self, *Locks::thread_list_lock_);
    // To check IsSuspended.
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    DCHECK(thread->IsSuspended());
    if (!Contains(thread)) {
      // We only expect threads within the thread-list to have been suspended otherwise we can't
      // stop such threads from delete-ing themselves.
      LOG(ERROR) << "Resume(" << reinterpret_cast<void*>(thread)
          << ") thread not within thread list";
      return;
    }
    thread->ModifySuspendCount(self, -1, nullptr, for_debugger);
  }

  {
    VLOG(threads) << "Resume(" << reinterpret_cast<void*>(thread) << ") waking others";
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  VLOG(threads) << "Resume(" << reinterpret_cast<void*>(thread) << ") complete";
}

static void ThreadSuspendByPeerWarning(Thread* self,
                                       LogSeverity severity,
                                       const char* message,
                                       jobject peer) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedLocalRef<jstring>
      scoped_name_string(env, static_cast<jstring>(env->GetObjectField(
          peer, WellKnownClasses::java_lang_Thread_name)));
  ScopedUtfChars scoped_name_chars(env, scoped_name_string.get());
  if (scoped_name_chars.c_str() == nullptr) {
      LOG(severity) << message << ": " << peer;
      env->ExceptionClear();
  } else {
      LOG(severity) << message << ": " << peer << ":" << scoped_name_chars.c_str();
  }
}

Thread* ThreadList::SuspendThreadByPeer(jobject peer,
                                        bool request_suspension,
                                        bool debug_suspension,
                                        bool* timed_out) {
  const uint64_t start_time = NanoTime();
  useconds_t sleep_us = kThreadSuspendInitialSleepUs;
  *timed_out = false;
  Thread* const self = Thread::Current();
  Thread* suspended_thread = nullptr;
  VLOG(threads) << "SuspendThreadByPeer starting";
  while (true) {
    Thread* thread;
    {
      // Note: this will transition to runnable and potentially suspend. We ensure only one thread
      // is requesting another suspend, to avoid deadlock, by requiring this function be called
      // holding Locks::thread_list_suspend_thread_lock_. Its important this thread suspend rather
      // than request thread suspension, to avoid potential cycles in threads requesting each other
      // suspend.
      ScopedObjectAccess soa(self);
      MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
      thread = Thread::FromManagedThread(soa, peer);
      if (thread == nullptr) {
        if (suspended_thread != nullptr) {
          MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
          // If we incremented the suspend count but the thread reset its peer, we need to
          // re-decrement it since it is shutting down and may deadlock the runtime in
          // ThreadList::WaitForOtherNonDaemonThreadsToExit.
          suspended_thread->ModifySuspendCount(soa.Self(), -1, nullptr, debug_suspension);
        }
        ThreadSuspendByPeerWarning(self, WARNING, "No such thread for suspend", peer);
        return nullptr;
      }
      if (!Contains(thread)) {
        CHECK(suspended_thread == nullptr);
        VLOG(threads) << "SuspendThreadByPeer failed for unattached thread: "
            << reinterpret_cast<void*>(thread);
        return nullptr;
      }
      VLOG(threads) << "SuspendThreadByPeer found thread: " << *thread;
      {
        MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
        if (request_suspension) {
          if (self->GetSuspendCount() > 0) {
            // We hold the suspend count lock but another thread is trying to suspend us. Its not
            // safe to try to suspend another thread in case we get a cycle. Start the loop again
            // which will allow this thread to be suspended.
            continue;
          }
          CHECK(suspended_thread == nullptr);
          suspended_thread = thread;
          suspended_thread->ModifySuspendCount(self, +1, nullptr, debug_suspension);
          request_suspension = false;
        } else {
          // If the caller isn't requesting suspension, a suspension should have already occurred.
          CHECK_GT(thread->GetSuspendCount(), 0);
        }
        // IsSuspended on the current thread will fail as the current thread is changed into
        // Runnable above. As the suspend count is now raised if this is the current thread
        // it will self suspend on transition to Runnable, making it hard to work with. It's simpler
        // to just explicitly handle the current thread in the callers to this code.
        CHECK_NE(thread, self) << "Attempt to suspend the current thread for the debugger";
        // If thread is suspended (perhaps it was already not Runnable but didn't have a suspend
        // count, or else we've waited and it has self suspended) or is the current thread, we're
        // done.
        if (thread->IsSuspended()) {
          VLOG(threads) << "SuspendThreadByPeer thread suspended: " << *thread;
          if (ATRACE_ENABLED()) {
            std::string name;
            thread->GetThreadName(name);
            ATRACE_BEGIN(StringPrintf("SuspendThreadByPeer suspended %s for peer=%p", name.c_str(),
                                      peer).c_str());
          }
          return thread;
        }
        const uint64_t total_delay = NanoTime() - start_time;
        if (total_delay >= MsToNs(kThreadSuspendTimeoutMs)) {
          ThreadSuspendByPeerWarning(self, FATAL, "Thread suspension timed out", peer);
          if (suspended_thread != nullptr) {
            CHECK_EQ(suspended_thread, thread);
            suspended_thread->ModifySuspendCount(soa.Self(), -1, nullptr, debug_suspension);
          }
          *timed_out = true;
          return nullptr;
        } else if (sleep_us == 0 &&
            total_delay > static_cast<uint64_t>(kThreadSuspendMaxYieldUs) * 1000) {
          // We have spun for kThreadSuspendMaxYieldUs time, switch to sleeps to prevent
          // excessive CPU usage.
          sleep_us = kThreadSuspendMaxYieldUs / 2;
        }
      }
      // Release locks and come out of runnable state.
    }
    VLOG(threads) << "SuspendThreadByPeer waiting to allow thread chance to suspend";
    ThreadSuspendSleep(sleep_us);
    // This may stay at 0 if sleep_us == 0, but this is WAI since we want to avoid using usleep at
    // all if possible. This shouldn't be an issue since time to suspend should always be small.
    sleep_us = std::min(sleep_us * 2, kThreadSuspendMaxSleepUs);
  }
}

static void ThreadSuspendByThreadIdWarning(LogSeverity severity,
                                           const char* message,
                                           uint32_t thread_id) {
  LOG(severity) << StringPrintf("%s: %d", message, thread_id);
}

Thread* ThreadList::SuspendThreadByThreadId(uint32_t thread_id,
                                            bool debug_suspension,
                                            bool* timed_out) {
  const uint64_t start_time = NanoTime();
  useconds_t sleep_us = kThreadSuspendInitialSleepUs;
  *timed_out = false;
  Thread* suspended_thread = nullptr;
  Thread* const self = Thread::Current();
  CHECK_NE(thread_id, kInvalidThreadId);
  VLOG(threads) << "SuspendThreadByThreadId starting";
  while (true) {
    {
      // Note: this will transition to runnable and potentially suspend. We ensure only one thread
      // is requesting another suspend, to avoid deadlock, by requiring this function be called
      // holding Locks::thread_list_suspend_thread_lock_. Its important this thread suspend rather
      // than request thread suspension, to avoid potential cycles in threads requesting each other
      // suspend.
      ScopedObjectAccess soa(self);
      MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
      Thread* thread = nullptr;
      for (const auto& it : list_) {
        if (it->GetThreadId() == thread_id) {
          thread = it;
          break;
        }
      }
      if (thread == nullptr) {
        CHECK(suspended_thread == nullptr) << "Suspended thread " << suspended_thread
            << " no longer in thread list";
        // There's a race in inflating a lock and the owner giving up ownership and then dying.
        ThreadSuspendByThreadIdWarning(WARNING, "No such thread id for suspend", thread_id);
        return nullptr;
      }
      VLOG(threads) << "SuspendThreadByThreadId found thread: " << *thread;
      DCHECK(Contains(thread));
      {
        MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
        if (suspended_thread == nullptr) {
          if (self->GetSuspendCount() > 0) {
            // We hold the suspend count lock but another thread is trying to suspend us. Its not
            // safe to try to suspend another thread in case we get a cycle. Start the loop again
            // which will allow this thread to be suspended.
            continue;
          }
          thread->ModifySuspendCount(self, +1, nullptr, debug_suspension);
          suspended_thread = thread;
        } else {
          CHECK_EQ(suspended_thread, thread);
          // If the caller isn't requesting suspension, a suspension should have already occurred.
          CHECK_GT(thread->GetSuspendCount(), 0);
        }
        // IsSuspended on the current thread will fail as the current thread is changed into
        // Runnable above. As the suspend count is now raised if this is the current thread
        // it will self suspend on transition to Runnable, making it hard to work with. It's simpler
        // to just explicitly handle the current thread in the callers to this code.
        CHECK_NE(thread, self) << "Attempt to suspend the current thread for the debugger";
        // If thread is suspended (perhaps it was already not Runnable but didn't have a suspend
        // count, or else we've waited and it has self suspended) or is the current thread, we're
        // done.
        if (thread->IsSuspended()) {
          if (ATRACE_ENABLED()) {
            std::string name;
            thread->GetThreadName(name);
            ATRACE_BEGIN(StringPrintf("SuspendThreadByThreadId suspended %s id=%d",
                                      name.c_str(), thread_id).c_str());
          }
          VLOG(threads) << "SuspendThreadByThreadId thread suspended: " << *thread;
          return thread;
        }
        const uint64_t total_delay = NanoTime() - start_time;
        if (total_delay >= MsToNs(kThreadSuspendTimeoutMs)) {
          ThreadSuspendByThreadIdWarning(WARNING, "Thread suspension timed out", thread_id);
          if (suspended_thread != nullptr) {
            thread->ModifySuspendCount(soa.Self(), -1, nullptr, debug_suspension);
          }
          *timed_out = true;
          return nullptr;
        } else if (sleep_us == 0 &&
            total_delay > static_cast<uint64_t>(kThreadSuspendMaxYieldUs) * 1000) {
          // We have spun for kThreadSuspendMaxYieldUs time, switch to sleeps to prevent
          // excessive CPU usage.
          sleep_us = kThreadSuspendMaxYieldUs / 2;
        }
      }
      // Release locks and come out of runnable state.
    }
    VLOG(threads) << "SuspendThreadByThreadId waiting to allow thread chance to suspend";
    ThreadSuspendSleep(sleep_us);
    sleep_us = std::min(sleep_us * 2, kThreadSuspendMaxSleepUs);
  }
}

Thread* ThreadList::FindThreadByThreadId(uint32_t thread_id) {
  for (const auto& thread : list_) {
    if (thread->GetThreadId() == thread_id) {
      return thread;
    }
  }
  return nullptr;
}

void ThreadList::SuspendAllForDebugger() {
  Thread* self = Thread::Current();
  Thread* debug_thread = Dbg::GetDebugThread();

  VLOG(threads) << *self << " SuspendAllForDebugger starting...";

  SuspendAllInternal(self, self, debug_thread, true);
  // Block on the mutator lock until all Runnable threads release their share of access then
  // immediately unlock again.
#if HAVE_TIMED_RWLOCK
  // Timeout if we wait more than 30 seconds.
  if (!Locks::mutator_lock_->ExclusiveLockWithTimeout(self, 30 * 1000, 0)) {
    UnsafeLogFatalForThreadSuspendAllTimeout();
  } else {
    Locks::mutator_lock_->ExclusiveUnlock(self);
  }
#else
  Locks::mutator_lock_->ExclusiveLock(self);
  Locks::mutator_lock_->ExclusiveUnlock(self);
#endif
  // Disabled for the following race condition:
  // Thread 1 calls SuspendAllForDebugger, gets preempted after pulsing the mutator lock.
  // Thread 2 calls SuspendAll and SetStateUnsafe (perhaps from Dbg::Disconnected).
  // Thread 1 fails assertion that all threads are suspended due to thread 2 being in a runnable
  // state (from SetStateUnsafe).
  // AssertThreadsAreSuspended(self, self, debug_thread);

  VLOG(threads) << *self << " SuspendAllForDebugger complete";
}

void ThreadList::SuspendSelfForDebugger() {
  Thread* const self = Thread::Current();
  self->SetReadyForDebugInvoke(true);

  // The debugger thread must not suspend itself due to debugger activity!
  Thread* debug_thread = Dbg::GetDebugThread();
  CHECK(self != debug_thread);
  CHECK_NE(self->GetState(), kRunnable);
  Locks::mutator_lock_->AssertNotHeld(self);

  // The debugger may have detached while we were executing an invoke request. In that case, we
  // must not suspend ourself.
  DebugInvokeReq* pReq = self->GetInvokeReq();
  const bool skip_thread_suspension = (pReq != nullptr && !Dbg::IsDebuggerActive());
  if (!skip_thread_suspension) {
    // Collisions with other suspends aren't really interesting. We want
    // to ensure that we're the only one fiddling with the suspend count
    // though.
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    self->ModifySuspendCount(self, +1, nullptr, true);
    CHECK_GT(self->GetSuspendCount(), 0);

    VLOG(threads) << *self << " self-suspending (debugger)";
  } else {
    // We must no longer be subject to debugger suspension.
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    CHECK_EQ(self->GetDebugSuspendCount(), 0) << "Debugger detached without resuming us";

    VLOG(threads) << *self << " not self-suspending because debugger detached during invoke";
  }

  // If the debugger requested an invoke, we need to send the reply and clear the request.
  if (pReq != nullptr) {
    Dbg::FinishInvokeMethod(pReq);
    self->ClearDebugInvokeReq();
    pReq = nullptr;  // object has been deleted, clear it for safety.
  }

  // Tell JDWP that we've completed suspension. The JDWP thread can't
  // tell us to resume before we're fully asleep because we hold the
  // suspend count lock.
  Dbg::ClearWaitForEventThread();

  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    while (self->GetSuspendCount() != 0) {
      Thread::resume_cond_->Wait(self);
      if (self->GetSuspendCount() != 0) {
        // The condition was signaled but we're still suspended. This
        // can happen when we suspend then resume all threads to
        // update instrumentation or compute monitor info. This can
        // also happen if the debugger lets go while a SIGQUIT thread
        // dump event is pending (assuming SignalCatcher was resumed for
        // just long enough to try to grab the thread-suspend lock).
        VLOG(jdwp) << *self << " still suspended after undo "
                   << "(suspend count=" << self->GetSuspendCount() << ", "
                   << "debug suspend count=" << self->GetDebugSuspendCount() << ")";
      }
    }
    CHECK_EQ(self->GetSuspendCount(), 0);
  }

  self->SetReadyForDebugInvoke(false);
  VLOG(threads) << *self << " self-reviving (debugger)";
}

void ThreadList::ResumeAllForDebugger() {
  Thread* self = Thread::Current();
  Thread* debug_thread = Dbg::GetDebugThread();

  VLOG(threads) << *self << " ResumeAllForDebugger starting...";

  // Threads can't resume if we exclusively hold the mutator lock.
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);

  {
    MutexLock thread_list_mu(self, *Locks::thread_list_lock_);
    {
      MutexLock suspend_count_mu(self, *Locks::thread_suspend_count_lock_);
      // Update global suspend all state for attaching threads.
      DCHECK_GE(suspend_all_count_, debug_suspend_all_count_);
      if (debug_suspend_all_count_ > 0) {
        --suspend_all_count_;
        --debug_suspend_all_count_;
      } else {
        // We've been asked to resume all threads without being asked to
        // suspend them all before. That may happen if a debugger tries
        // to resume some suspended threads (with suspend count == 1)
        // at once with a VirtualMachine.Resume command. Let's print a
        // warning.
        LOG(WARNING) << "Debugger attempted to resume all threads without "
                     << "having suspended them all before.";
      }
      // Decrement everybody's suspend count (except our own).
      for (const auto& thread : list_) {
        if (thread == self || thread == debug_thread) {
          continue;
        }
        if (thread->GetDebugSuspendCount() == 0) {
          // This thread may have been individually resumed with ThreadReference.Resume.
          continue;
        }
        VLOG(threads) << "requesting thread resume: " << *thread;
        thread->ModifySuspendCount(self, -1, nullptr, true);
      }
    }
  }

  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  VLOG(threads) << *self << " ResumeAllForDebugger complete";
}

void ThreadList::UndoDebuggerSuspensions() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " UndoDebuggerSuspensions starting";

  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    suspend_all_count_ -= debug_suspend_all_count_;
    debug_suspend_all_count_ = 0;
    // Update running threads.
    for (const auto& thread : list_) {
      if (thread == self || thread->GetDebugSuspendCount() == 0) {
        continue;
      }
      thread->ModifySuspendCount(self, -thread->GetDebugSuspendCount(), nullptr, true);
    }
  }

  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  VLOG(threads) << "UndoDebuggerSuspensions(" << *self << ") complete";
}

void ThreadList::WaitForOtherNonDaemonThreadsToExit() {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  while (true) {
    {
      // No more threads can be born after we start to shutdown.
      MutexLock mu(self, *Locks::runtime_shutdown_lock_);
      CHECK(Runtime::Current()->IsShuttingDownLocked());
      CHECK_EQ(Runtime::Current()->NumberOfThreadsBeingBorn(), 0U);
    }
    MutexLock mu(self, *Locks::thread_list_lock_);
    // Also wait for any threads that are unregistering to finish. This is required so that no
    // threads access the thread list after it is deleted. TODO: This may not work for user daemon
    // threads since they could unregister at the wrong time.
    bool done = unregistering_count_ == 0;
    if (done) {
      for (const auto& thread : list_) {
        if (thread != self && !thread->IsDaemon()) {
          done = false;
          break;
        }
      }
    }
    if (done) {
      break;
    }
    // Wait for another thread to exit before re-checking.
    Locks::thread_exit_cond_->Wait(self);
  }
}

void ThreadList::SuspendAllDaemonThreadsForShutdown() {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::thread_list_lock_);
  size_t daemons_left = 0;
  {  // Tell all the daemons it's time to suspend.
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : list_) {
      // This is only run after all non-daemon threads have exited, so the remainder should all be
      // daemons.
      CHECK(thread->IsDaemon()) << *thread;
      if (thread != self) {
        thread->ModifySuspendCount(self, +1, nullptr, false);
        ++daemons_left;
      }
      // We are shutting down the runtime, set the JNI functions of all the JNIEnvs to be
      // the sleep forever one.
      thread->GetJniEnv()->SetFunctionsToRuntimeShutdownFunctions();
    }
  }
  // If we have any daemons left, wait 200ms to ensure they are not stuck in a place where they
  // are about to access runtime state and are not in a runnable state. Examples: Monitor code
  // or waking up from a condition variable. TODO: Try and see if there is a better way to wait
  // for daemon threads to be in a blocked state.
  if (daemons_left > 0) {
    static constexpr size_t kDaemonSleepTime = 200 * 1000;
    usleep(kDaemonSleepTime);
  }
  // Give the threads a chance to suspend, complaining if they're slow.
  bool have_complained = false;
  static constexpr size_t kTimeoutMicroseconds = 2000 * 1000;
  static constexpr size_t kSleepMicroseconds = 1000;
  for (size_t i = 0; i < kTimeoutMicroseconds / kSleepMicroseconds; ++i) {
    bool all_suspended = true;
    for (const auto& thread : list_) {
      if (thread != self && thread->GetState() == kRunnable) {
        if (!have_complained) {
          LOG(WARNING) << "daemon thread not yet suspended: " << *thread;
          have_complained = true;
        }
        all_suspended = false;
      }
    }
    if (all_suspended) {
      return;
    }
    usleep(kSleepMicroseconds);
  }
  LOG(WARNING) << "timed out suspending all daemon threads";
}

void ThreadList::Register(Thread* self) {
  DCHECK_EQ(self, Thread::Current());

  if (VLOG_IS_ON(threads)) {
    std::ostringstream oss;
    self->ShortDump(oss);  // We don't hold the mutator_lock_ yet and so cannot call Dump.
    LOG(INFO) << "ThreadList::Register() " << *self  << "\n" << oss.str();
  }

  // Atomically add self to the thread list and make its thread_suspend_count_ reflect ongoing
  // SuspendAll requests.
  MutexLock mu(self, *Locks::thread_list_lock_);
  MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
  CHECK_GE(suspend_all_count_, debug_suspend_all_count_);
  // Modify suspend count in increments of 1 to maintain invariants in ModifySuspendCount. While
  // this isn't particularly efficient the suspend counts are most commonly 0 or 1.
  for (int delta = debug_suspend_all_count_; delta > 0; delta--) {
    self->ModifySuspendCount(self, +1, nullptr, true);
  }
  for (int delta = suspend_all_count_ - debug_suspend_all_count_; delta > 0; delta--) {
    self->ModifySuspendCount(self, +1, nullptr, false);
  }
  CHECK(!Contains(self));
  list_.push_back(self);
  if (kUseReadBarrier) {
    // Initialize according to the state of the CC collector.
    bool is_gc_marking =
        Runtime::Current()->GetHeap()->ConcurrentCopyingCollector()->IsMarking();
    self->SetIsGcMarking(is_gc_marking);
    bool weak_ref_access_enabled =
        Runtime::Current()->GetHeap()->ConcurrentCopyingCollector()->IsWeakRefAccessEnabled();
    self->SetWeakRefAccessEnabled(weak_ref_access_enabled);
  }
}

void ThreadList::Unregister(Thread* self) {
  DCHECK_EQ(self, Thread::Current());
  CHECK_NE(self->GetState(), kRunnable);
  Locks::mutator_lock_->AssertNotHeld(self);

  VLOG(threads) << "ThreadList::Unregister() " << *self;

  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    ++unregistering_count_;
  }

  // Any time-consuming destruction, plus anything that can call back into managed code or
  // suspend and so on, must happen at this point, and not in ~Thread. The self->Destroy is what
  // causes the threads to join. It is important to do this after incrementing unregistering_count_
  // since we want the runtime to wait for the daemon threads to exit before deleting the thread
  // list.
  self->Destroy();

  // If tracing, remember thread id and name before thread exits.
  Trace::StoreExitingThreadInfo(self);

  uint32_t thin_lock_id = self->GetThreadId();
  while (true) {
    // Remove and delete the Thread* while holding the thread_list_lock_ and
    // thread_suspend_count_lock_ so that the unregistering thread cannot be suspended.
    // Note: deliberately not using MutexLock that could hold a stale self pointer.
    MutexLock mu(self, *Locks::thread_list_lock_);
    if (!Contains(self)) {
      std::string thread_name;
      self->GetThreadName(thread_name);
      std::ostringstream os;
      DumpNativeStack(os, GetTid(), nullptr, "  native: ", nullptr);
      LOG(ERROR) << "Request to unregister unattached thread " << thread_name << "\n" << os.str();
      break;
    } else {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      if (!self->IsSuspended()) {
        list_.remove(self);
        break;
      }
    }
    // We failed to remove the thread due to a suspend request, loop and try again.
  }
  delete self;

  // Release the thread ID after the thread is finished and deleted to avoid cases where we can
  // temporarily have multiple threads with the same thread id. When this occurs, it causes
  // problems in FindThreadByThreadId / SuspendThreadByThreadId.
  ReleaseThreadId(nullptr, thin_lock_id);

  // Clear the TLS data, so that the underlying native thread is recognizably detached.
  // (It may wish to reattach later.)
#ifdef __ANDROID__
  __get_tls()[TLS_SLOT_ART_THREAD_SELF] = nullptr;
#else
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, nullptr), "detach self");
#endif

  // Signal that a thread just detached.
  MutexLock mu(nullptr, *Locks::thread_list_lock_);
  --unregistering_count_;
  Locks::thread_exit_cond_->Broadcast(nullptr);
}

void ThreadList::ForEach(void (*callback)(Thread*, void*), void* context) {
  for (const auto& thread : list_) {
    callback(thread, context);
  }
}

void ThreadList::VisitRoots(RootVisitor* visitor) const {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    thread->VisitRoots(visitor);
  }
}

uint32_t ThreadList::AllocThreadId(Thread* self) {
  MutexLock mu(self, *Locks::allocated_thread_ids_lock_);
  for (size_t i = 0; i < allocated_ids_.size(); ++i) {
    if (!allocated_ids_[i]) {
      allocated_ids_.set(i);
      return i + 1;  // Zero is reserved to mean "invalid".
    }
  }
  LOG(FATAL) << "Out of internal thread ids";
  return 0;
}

void ThreadList::ReleaseThreadId(Thread* self, uint32_t id) {
  MutexLock mu(self, *Locks::allocated_thread_ids_lock_);
  --id;  // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}

ScopedSuspendAll::ScopedSuspendAll(const char* cause, bool long_suspend) {
  Runtime::Current()->GetThreadList()->SuspendAll(cause, long_suspend);
}

ScopedSuspendAll::~ScopedSuspendAll() {
  Runtime::Current()->GetThreadList()->ResumeAll();
}

}  // namespace art
