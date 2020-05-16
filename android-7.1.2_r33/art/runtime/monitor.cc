/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "monitor.h"

#include <vector>

#include "art_method-inl.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "lock_word-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

namespace art {

static constexpr uint64_t kLongWaitMs = 100;

/*
 * Every Object has a monitor associated with it, but not every Object is actually locked.  Even
 * the ones that are locked do not need a full-fledged monitor until a) there is actual contention
 * or b) wait() is called on the Object.
 *
 * For Android, we have implemented a scheme similar to the one described in Bacon et al.'s
 * "Thin locks: featherweight synchronization for Java" (ACM 1998).  Things are even easier for us,
 * though, because we have a full 32 bits to work with.
 *
 * The two states of an Object's lock are referred to as "thin" and "fat".  A lock may transition
 * from the "thin" state to the "fat" state and this transition is referred to as inflation. Once
 * a lock has been inflated it remains in the "fat" state indefinitely.
 *
 * The lock value itself is stored in mirror::Object::monitor_ and the representation is described
 * in the LockWord value type.
 *
 * Monitors provide:
 *  - mutually exclusive access to resources
 *  - a way for multiple threads to wait for notification
 *
 * In effect, they fill the role of both mutexes and condition variables.
 *
 * Only one thread can own the monitor at any time.  There may be several threads waiting on it
 * (the wait call unlocks it).  One or more waiting threads may be getting interrupted or notified
 * at any given time.
 */

uint32_t Monitor::lock_profiling_threshold_ = 0;

void Monitor::Init(uint32_t lock_profiling_threshold) {
  lock_profiling_threshold_ = lock_profiling_threshold;
}

Monitor::Monitor(Thread* self, Thread* owner, mirror::Object* obj, int32_t hash_code)
    : monitor_lock_("a monitor lock", kMonitorLock),
      monitor_contenders_("monitor contenders", monitor_lock_),
      num_waiters_(0),
      owner_(owner),
      lock_count_(0),
      obj_(GcRoot<mirror::Object>(obj)),
      wait_set_(nullptr),
      hash_code_(hash_code),
      locking_method_(nullptr),
      locking_dex_pc_(0),
      monitor_id_(MonitorPool::ComputeMonitorId(this, self)) {
#ifdef __LP64__
  DCHECK(false) << "Should not be reached in 64b";
  next_free_ = nullptr;
#endif
  // We should only inflate a lock if the owner is ourselves or suspended. This avoids a race
  // with the owner unlocking the thin-lock.
  CHECK(owner == nullptr || owner == self || owner->IsSuspended());
  // The identity hash code is set for the life time of the monitor.
}

Monitor::Monitor(Thread* self, Thread* owner, mirror::Object* obj, int32_t hash_code,
                 MonitorId id)
    : monitor_lock_("a monitor lock", kMonitorLock),
      monitor_contenders_("monitor contenders", monitor_lock_),
      num_waiters_(0),
      owner_(owner),
      lock_count_(0),
      obj_(GcRoot<mirror::Object>(obj)),
      wait_set_(nullptr),
      hash_code_(hash_code),
      locking_method_(nullptr),
      locking_dex_pc_(0),
      monitor_id_(id) {
#ifdef __LP64__
  next_free_ = nullptr;
#endif
  // We should only inflate a lock if the owner is ourselves or suspended. This avoids a race
  // with the owner unlocking the thin-lock.
  CHECK(owner == nullptr || owner == self || owner->IsSuspended());
  // The identity hash code is set for the life time of the monitor.
}

int32_t Monitor::GetHashCode() {
  while (!HasHashCode()) {
    if (hash_code_.CompareExchangeWeakRelaxed(0, mirror::Object::GenerateIdentityHashCode())) {
      break;
    }
  }
  DCHECK(HasHashCode());
  return hash_code_.LoadRelaxed();
}

bool Monitor::Install(Thread* self) {
  MutexLock mu(self, monitor_lock_);  // Uncontended mutex acquisition as monitor isn't yet public.
  CHECK(owner_ == nullptr || owner_ == self || owner_->IsSuspended());
  // Propagate the lock state.
  LockWord lw(GetObject()->GetLockWord(false));
  switch (lw.GetState()) {
    case LockWord::kThinLocked: {
      CHECK_EQ(owner_->GetThreadId(), lw.ThinLockOwner());
      lock_count_ = lw.ThinLockCount();
      break;
    }
    case LockWord::kHashCode: {
      CHECK_EQ(hash_code_.LoadRelaxed(), static_cast<int32_t>(lw.GetHashCode()));
      break;
    }
    case LockWord::kFatLocked: {
      // The owner_ is suspended but another thread beat us to install a monitor.
      return false;
    }
    case LockWord::kUnlocked: {
      LOG(FATAL) << "Inflating unlocked lock word";
      break;
    }
    default: {
      LOG(FATAL) << "Invalid monitor state " << lw.GetState();
      return false;
    }
  }
  LockWord fat(this, lw.ReadBarrierState());
  // Publish the updated lock word, which may race with other threads.
  bool success = GetObject()->CasLockWordWeakSequentiallyConsistent(lw, fat);
  // Lock profiling.
  if (success && owner_ != nullptr && lock_profiling_threshold_ != 0) {
    // Do not abort on dex pc errors. This can easily happen when we want to dump a stack trace on
    // abort.
    locking_method_ = owner_->GetCurrentMethod(&locking_dex_pc_, false);
  }
  return success;
}

Monitor::~Monitor() {
  // Deflated monitors have a null object.
}

void Monitor::AppendToWaitSet(Thread* thread) {
  DCHECK(owner_ == Thread::Current());
  DCHECK(thread != nullptr);
  DCHECK(thread->GetWaitNext() == nullptr) << thread->GetWaitNext();
  if (wait_set_ == nullptr) {
    wait_set_ = thread;
    return;
  }

  // push_back.
  Thread* t = wait_set_;
  while (t->GetWaitNext() != nullptr) {
    t = t->GetWaitNext();
  }
  t->SetWaitNext(thread);
}

void Monitor::RemoveFromWaitSet(Thread *thread) {
  DCHECK(owner_ == Thread::Current());
  DCHECK(thread != nullptr);
  if (wait_set_ == nullptr) {
    return;
  }
  if (wait_set_ == thread) {
    wait_set_ = thread->GetWaitNext();
    thread->SetWaitNext(nullptr);
    return;
  }

  Thread* t = wait_set_;
  while (t->GetWaitNext() != nullptr) {
    if (t->GetWaitNext() == thread) {
      t->SetWaitNext(thread->GetWaitNext());
      thread->SetWaitNext(nullptr);
      return;
    }
    t = t->GetWaitNext();
  }
}

void Monitor::SetObject(mirror::Object* object) {
  obj_ = GcRoot<mirror::Object>(object);
}

// Note: Adapted from CurrentMethodVisitor in thread.cc. We must not resolve here.

struct NthCallerWithDexPcVisitor FINAL : public StackVisitor {
  explicit NthCallerWithDexPcVisitor(Thread* thread, size_t frame)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFramesNoResolve),
        method_(nullptr),
        dex_pc_(0),
        current_frame_number_(0),
        wanted_frame_number_(frame) {}
  bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    if (m == nullptr || m->IsRuntimeMethod()) {
      // Runtime method, upcall, or resolution issue. Skip.
      return true;
    }

    // Is this the requested frame?
    if (current_frame_number_ == wanted_frame_number_) {
      method_ = m;
      dex_pc_ = GetDexPc(false /* abort_on_error*/);
      return false;
    }

    // Look for more.
    current_frame_number_++;
    return true;
  }

  ArtMethod* method_;
  uint32_t dex_pc_;

 private:
  size_t current_frame_number_;
  const size_t wanted_frame_number_;
};

// This function is inlined and just helps to not have the VLOG and ATRACE check at all the
// potential tracing points.
void Monitor::AtraceMonitorLock(Thread* self, mirror::Object* obj, bool is_wait) {
  if (UNLIKELY(VLOG_IS_ON(systrace_lock_logging) && ATRACE_ENABLED())) {
    AtraceMonitorLockImpl(self, obj, is_wait);
  }
}

void Monitor::AtraceMonitorLockImpl(Thread* self, mirror::Object* obj, bool is_wait) {
  // Wait() requires a deeper call stack to be useful. Otherwise you'll see "Waiting at
  // Object.java". Assume that we'll wait a nontrivial amount, so it's OK to do a longer
  // stack walk than if !is_wait.
  NthCallerWithDexPcVisitor visitor(self, is_wait ? 1U : 0U);
  visitor.WalkStack(false);
  const char* prefix = is_wait ? "Waiting on " : "Locking ";

  const char* filename;
  int32_t line_number;
  TranslateLocation(visitor.method_, visitor.dex_pc_, &filename, &line_number);

  // It would be nice to have a stable "ID" for the object here. However, the only stable thing
  // would be the identity hashcode. But we cannot use IdentityHashcode here: For one, there are
  // times when it is unsafe to make that call (see stack dumping for an explanation). More
  // importantly, we would have to give up on thin-locking when adding systrace locks, as the
  // identity hashcode is stored in the lockword normally (so can't be used with thin-locks).
  //
  // Because of thin-locks we also cannot use the monitor id (as there is no monitor). Monitor ids
  // also do not have to be stable, as the monitor may be deflated.
  std::string tmp = StringPrintf("%s %d at %s:%d",
      prefix,
      (obj == nullptr ? -1 : static_cast<int32_t>(reinterpret_cast<uintptr_t>(obj))),
      (filename != nullptr ? filename : "null"),
      line_number);
  ATRACE_BEGIN(tmp.c_str());
}

void Monitor::AtraceMonitorUnlock() {
  if (UNLIKELY(VLOG_IS_ON(systrace_lock_logging))) {
    ATRACE_END();
  }
}

std::string Monitor::PrettyContentionInfo(const std::string& owner_name,
                                          pid_t owner_tid,
                                          ArtMethod* owners_method,
                                          uint32_t owners_dex_pc,
                                          size_t num_waiters) {
  const char* owners_filename;
  int32_t owners_line_number;
  if (owners_method != nullptr) {
    TranslateLocation(owners_method, owners_dex_pc, &owners_filename, &owners_line_number);
  }
  std::ostringstream oss;
  oss << "monitor contention with owner " << owner_name << " (" << owner_tid << ")";
  if (owners_method != nullptr) {
    oss << " at " << PrettyMethod(owners_method);
    oss << "(" << owners_filename << ":" << owners_line_number << ")";
  }
  oss << " waiters=" << num_waiters;
  return oss.str();
}

bool Monitor::TryLockLocked(Thread* self) {
  if (owner_ == nullptr) {  // Unowned.
    owner_ = self;
    CHECK_EQ(lock_count_, 0);
    // When debugging, save the current monitor holder for future
    // acquisition failures to use in sampled logging.
    if (lock_profiling_threshold_ != 0) {
      locking_method_ = self->GetCurrentMethod(&locking_dex_pc_);
    }
  } else if (owner_ == self) {  // Recursive.
    lock_count_++;
  } else {
    return false;
  }
  AtraceMonitorLock(self, GetObject(), false /* is_wait */);
  return true;
}

bool Monitor::TryLock(Thread* self) {
  MutexLock mu(self, monitor_lock_);
  return TryLockLocked(self);
}

void Monitor::Lock(Thread* self) {
  MutexLock mu(self, monitor_lock_);
  while (true) {
    if (TryLockLocked(self)) {
      return;
    }
    // Contended.
    const bool log_contention = (lock_profiling_threshold_ != 0);
    uint64_t wait_start_ms = log_contention ? MilliTime() : 0;
    ArtMethod* owners_method = locking_method_;
    uint32_t owners_dex_pc = locking_dex_pc_;
    // Do this before releasing the lock so that we don't get deflated.
    size_t num_waiters = num_waiters_;
    ++num_waiters_;
    monitor_lock_.Unlock(self);  // Let go of locks in order.
    self->SetMonitorEnterObject(GetObject());
    {
      uint32_t original_owner_thread_id = 0u;
      ScopedThreadStateChange tsc(self, kBlocked);  // Change to blocked and give up mutator_lock_.
      {
        // Reacquire monitor_lock_ without mutator_lock_ for Wait.
        MutexLock mu2(self, monitor_lock_);
        if (owner_ != nullptr) {  // Did the owner_ give the lock up?
          original_owner_thread_id = owner_->GetThreadId();
          if (ATRACE_ENABLED()) {
            std::ostringstream oss;
            std::string name;
            owner_->GetThreadName(name);
            oss << PrettyContentionInfo(name,
                                        owner_->GetTid(),
                                        owners_method,
                                        owners_dex_pc,
                                        num_waiters);
            // Add info for contending thread.
            uint32_t pc;
            ArtMethod* m = self->GetCurrentMethod(&pc);
            const char* filename;
            int32_t line_number;
            TranslateLocation(m, pc, &filename, &line_number);
            oss << " blocking from "
                << PrettyMethod(m) << "(" << (filename != nullptr ? filename : "null") << ":"
                << line_number << ")";
            ATRACE_BEGIN(oss.str().c_str());
          }
          monitor_contenders_.Wait(self);  // Still contended so wait.
        }
      }
      if (original_owner_thread_id != 0u) {
        // Woken from contention.
        if (log_contention) {
          uint32_t original_owner_tid = 0;
          std::string original_owner_name;
          {
            MutexLock mu2(Thread::Current(), *Locks::thread_list_lock_);
            // Re-find the owner in case the thread got killed.
            Thread* original_owner = Runtime::Current()->GetThreadList()->FindThreadByThreadId(
                original_owner_thread_id);
            // Do not do any work that requires the mutator lock.
            if (original_owner != nullptr) {
              original_owner_tid = original_owner->GetTid();
              original_owner->GetThreadName(original_owner_name);
            }
          }

          if (original_owner_tid != 0u) {
            uint64_t wait_ms = MilliTime() - wait_start_ms;
            uint32_t sample_percent;
            if (wait_ms >= lock_profiling_threshold_) {
              sample_percent = 100;
            } else {
              sample_percent = 100 * wait_ms / lock_profiling_threshold_;
            }
            if (sample_percent != 0 && (static_cast<uint32_t>(rand() % 100) < sample_percent)) {
              if (wait_ms > kLongWaitMs && owners_method != nullptr) {
                uint32_t pc;
                ArtMethod* m = self->GetCurrentMethod(&pc);
                // TODO: We should maybe check that original_owner is still a live thread.
                LOG(WARNING) << "Long "
                    << PrettyContentionInfo(original_owner_name,
                                            original_owner_tid,
                                            owners_method,
                                            owners_dex_pc,
                                            num_waiters)
                    << " in " << PrettyMethod(m) << " for " << PrettyDuration(MsToNs(wait_ms));
              }
              const char* owners_filename;
              int32_t owners_line_number;
              TranslateLocation(owners_method,
                                owners_dex_pc,
                                &owners_filename,
                                &owners_line_number);
              LogContentionEvent(self,
                                 wait_ms,
                                 sample_percent,
                                 owners_filename,
                                 owners_line_number);
            }
          }
        }
        ATRACE_END();
      }
    }
    self->SetMonitorEnterObject(nullptr);
    monitor_lock_.Lock(self);  // Reacquire locks in order.
    --num_waiters_;
  }
}

static void ThrowIllegalMonitorStateExceptionF(const char* fmt, ...)
                                              __attribute__((format(printf, 1, 2)));

static void ThrowIllegalMonitorStateExceptionF(const char* fmt, ...)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  va_list args;
  va_start(args, fmt);
  Thread* self = Thread::Current();
  self->ThrowNewExceptionV("Ljava/lang/IllegalMonitorStateException;", fmt, args);
  if (!Runtime::Current()->IsStarted() || VLOG_IS_ON(monitor)) {
    std::ostringstream ss;
    self->Dump(ss);
    LOG(Runtime::Current()->IsStarted() ? INFO : ERROR)
        << self->GetException()->Dump() << "\n" << ss.str();
  }
  va_end(args);
}

static std::string ThreadToString(Thread* thread) {
  if (thread == nullptr) {
    return "nullptr";
  }
  std::ostringstream oss;
  // TODO: alternatively, we could just return the thread's name.
  oss << *thread;
  return oss.str();
}

void Monitor::FailedUnlock(mirror::Object* o,
                           uint32_t expected_owner_thread_id,
                           uint32_t found_owner_thread_id,
                           Monitor* monitor) {
  // Acquire thread list lock so threads won't disappear from under us.
  std::string current_owner_string;
  std::string expected_owner_string;
  std::string found_owner_string;
  uint32_t current_owner_thread_id = 0u;
  {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    ThreadList* const thread_list = Runtime::Current()->GetThreadList();
    Thread* expected_owner = thread_list->FindThreadByThreadId(expected_owner_thread_id);
    Thread* found_owner = thread_list->FindThreadByThreadId(found_owner_thread_id);

    // Re-read owner now that we hold lock.
    Thread* current_owner = (monitor != nullptr) ? monitor->GetOwner() : nullptr;
    if (current_owner != nullptr) {
      current_owner_thread_id = current_owner->GetThreadId();
    }
    // Get short descriptions of the threads involved.
    current_owner_string = ThreadToString(current_owner);
    expected_owner_string = expected_owner != nullptr ? ThreadToString(expected_owner) : "unnamed";
    found_owner_string = found_owner != nullptr ? ThreadToString(found_owner) : "unnamed";
  }

  if (current_owner_thread_id == 0u) {
    if (found_owner_thread_id == 0u) {
      ThrowIllegalMonitorStateExceptionF("unlock of unowned monitor on object of type '%s'"
                                         " on thread '%s'",
                                         PrettyTypeOf(o).c_str(),
                                         expected_owner_string.c_str());
    } else {
      // Race: the original read found an owner but now there is none
      ThrowIllegalMonitorStateExceptionF("unlock of monitor owned by '%s' on object of type '%s'"
                                         " (where now the monitor appears unowned) on thread '%s'",
                                         found_owner_string.c_str(),
                                         PrettyTypeOf(o).c_str(),
                                         expected_owner_string.c_str());
    }
  } else {
    if (found_owner_thread_id == 0u) {
      // Race: originally there was no owner, there is now
      ThrowIllegalMonitorStateExceptionF("unlock of monitor owned by '%s' on object of type '%s'"
                                         " (originally believed to be unowned) on thread '%s'",
                                         current_owner_string.c_str(),
                                         PrettyTypeOf(o).c_str(),
                                         expected_owner_string.c_str());
    } else {
      if (found_owner_thread_id != current_owner_thread_id) {
        // Race: originally found and current owner have changed
        ThrowIllegalMonitorStateExceptionF("unlock of monitor originally owned by '%s' (now"
                                           " owned by '%s') on object of type '%s' on thread '%s'",
                                           found_owner_string.c_str(),
                                           current_owner_string.c_str(),
                                           PrettyTypeOf(o).c_str(),
                                           expected_owner_string.c_str());
      } else {
        ThrowIllegalMonitorStateExceptionF("unlock of monitor owned by '%s' on object of type '%s'"
                                           " on thread '%s",
                                           current_owner_string.c_str(),
                                           PrettyTypeOf(o).c_str(),
                                           expected_owner_string.c_str());
      }
    }
  }
}

bool Monitor::Unlock(Thread* self) {
  DCHECK(self != nullptr);
  uint32_t owner_thread_id = 0u;
  {
    MutexLock mu(self, monitor_lock_);
    Thread* owner = owner_;
    if (owner != nullptr) {
      owner_thread_id = owner->GetThreadId();
    }
    if (owner == self) {
      // We own the monitor, so nobody else can be in here.
      AtraceMonitorUnlock();
      if (lock_count_ == 0) {
        owner_ = nullptr;
        locking_method_ = nullptr;
        locking_dex_pc_ = 0;
        // Wake a contender.
        monitor_contenders_.Signal(self);
      } else {
        --lock_count_;
      }
      return true;
    }
  }
  // We don't own this, so we're not allowed to unlock it.
  // The JNI spec says that we should throw IllegalMonitorStateException in this case.
  FailedUnlock(GetObject(), self->GetThreadId(), owner_thread_id, this);
  return false;
}

void Monitor::Wait(Thread* self, int64_t ms, int32_t ns,
                   bool interruptShouldThrow, ThreadState why) {
  DCHECK(self != nullptr);
  DCHECK(why == kTimedWaiting || why == kWaiting || why == kSleeping);

  monitor_lock_.Lock(self);

  // Make sure that we hold the lock.
  if (owner_ != self) {
    monitor_lock_.Unlock(self);
    ThrowIllegalMonitorStateExceptionF("object not locked by thread before wait()");
    return;
  }

  // We need to turn a zero-length timed wait into a regular wait because
  // Object.wait(0, 0) is defined as Object.wait(0), which is defined as Object.wait().
  if (why == kTimedWaiting && (ms == 0 && ns == 0)) {
    why = kWaiting;
  }

  // Enforce the timeout range.
  if (ms < 0 || ns < 0 || ns > 999999) {
    monitor_lock_.Unlock(self);
    self->ThrowNewExceptionF("Ljava/lang/IllegalArgumentException;",
                             "timeout arguments out of range: ms=%" PRId64 " ns=%d", ms, ns);
    return;
  }

  /*
   * Add ourselves to the set of threads waiting on this monitor, and
   * release our hold.  We need to let it go even if we're a few levels
   * deep in a recursive lock, and we need to restore that later.
   *
   * We append to the wait set ahead of clearing the count and owner
   * fields so the subroutine can check that the calling thread owns
   * the monitor.  Aside from that, the order of member updates is
   * not order sensitive as we hold the pthread mutex.
   */
  AppendToWaitSet(self);
  ++num_waiters_;
  int prev_lock_count = lock_count_;
  lock_count_ = 0;
  owner_ = nullptr;
  ArtMethod* saved_method = locking_method_;
  locking_method_ = nullptr;
  uintptr_t saved_dex_pc = locking_dex_pc_;
  locking_dex_pc_ = 0;

  AtraceMonitorUnlock();  // For the implict Unlock() just above. This will only end the deepest
                          // nesting, but that is enough for the visualization, and corresponds to
                          // the single Lock() we do afterwards.
  AtraceMonitorLock(self, GetObject(), true /* is_wait */);

  bool was_interrupted = false;
  {
    // Update thread state. If the GC wakes up, it'll ignore us, knowing
    // that we won't touch any references in this state, and we'll check
    // our suspend mode before we transition out.
    ScopedThreadSuspension sts(self, why);

    // Pseudo-atomically wait on self's wait_cond_ and release the monitor lock.
    MutexLock mu(self, *self->GetWaitMutex());

    // Set wait_monitor_ to the monitor object we will be waiting on. When wait_monitor_ is
    // non-null a notifying or interrupting thread must signal the thread's wait_cond_ to wake it
    // up.
    DCHECK(self->GetWaitMonitor() == nullptr);
    self->SetWaitMonitor(this);

    // Release the monitor lock.
    monitor_contenders_.Signal(self);
    monitor_lock_.Unlock(self);

    // Handle the case where the thread was interrupted before we called wait().
    if (self->IsInterruptedLocked()) {
      was_interrupted = true;
    } else {
      // Wait for a notification or a timeout to occur.
      if (why == kWaiting) {
        self->GetWaitConditionVariable()->Wait(self);
      } else {
        DCHECK(why == kTimedWaiting || why == kSleeping) << why;
        self->GetWaitConditionVariable()->TimedWait(self, ms, ns);
      }
      was_interrupted = self->IsInterruptedLocked();
    }
  }

  {
    // We reset the thread's wait_monitor_ field after transitioning back to runnable so
    // that a thread in a waiting/sleeping state has a non-null wait_monitor_ for debugging
    // and diagnostic purposes. (If you reset this earlier, stack dumps will claim that threads
    // are waiting on "null".)
    MutexLock mu(self, *self->GetWaitMutex());
    DCHECK(self->GetWaitMonitor() != nullptr);
    self->SetWaitMonitor(nullptr);
  }

  // Allocate the interrupted exception not holding the monitor lock since it may cause a GC.
  // If the GC requires acquiring the monitor for enqueuing cleared references, this would
  // cause a deadlock if the monitor is held.
  if (was_interrupted && interruptShouldThrow) {
    /*
     * We were interrupted while waiting, or somebody interrupted an
     * un-interruptible thread earlier and we're bailing out immediately.
     *
     * The doc sayeth: "The interrupted status of the current thread is
     * cleared when this exception is thrown."
     */
    {
      MutexLock mu(self, *self->GetWaitMutex());
      self->SetInterruptedLocked(false);
    }
    self->ThrowNewException("Ljava/lang/InterruptedException;", nullptr);
  }

  AtraceMonitorUnlock();  // End Wait().

  // Re-acquire the monitor and lock.
  Lock(self);
  monitor_lock_.Lock(self);
  self->GetWaitMutex()->AssertNotHeld(self);

  /*
   * We remove our thread from wait set after restoring the count
   * and owner fields so the subroutine can check that the calling
   * thread owns the monitor. Aside from that, the order of member
   * updates is not order sensitive as we hold the pthread mutex.
   */
  owner_ = self;
  lock_count_ = prev_lock_count;
  locking_method_ = saved_method;
  locking_dex_pc_ = saved_dex_pc;
  --num_waiters_;
  RemoveFromWaitSet(self);

  monitor_lock_.Unlock(self);
}

void Monitor::Notify(Thread* self) {
  DCHECK(self != nullptr);
  MutexLock mu(self, monitor_lock_);
  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateExceptionF("object not locked by thread before notify()");
    return;
  }
  // Signal the first waiting thread in the wait set.
  while (wait_set_ != nullptr) {
    Thread* thread = wait_set_;
    wait_set_ = thread->GetWaitNext();
    thread->SetWaitNext(nullptr);

    // Check to see if the thread is still waiting.
    MutexLock wait_mu(self, *thread->GetWaitMutex());
    if (thread->GetWaitMonitor() != nullptr) {
      thread->GetWaitConditionVariable()->Signal(self);
      return;
    }
  }
}

void Monitor::NotifyAll(Thread* self) {
  DCHECK(self != nullptr);
  MutexLock mu(self, monitor_lock_);
  // Make sure that we hold the lock.
  if (owner_ != self) {
    ThrowIllegalMonitorStateExceptionF("object not locked by thread before notifyAll()");
    return;
  }
  // Signal all threads in the wait set.
  while (wait_set_ != nullptr) {
    Thread* thread = wait_set_;
    wait_set_ = thread->GetWaitNext();
    thread->SetWaitNext(nullptr);
    thread->Notify();
  }
}

bool Monitor::Deflate(Thread* self, mirror::Object* obj) {
  DCHECK(obj != nullptr);
  // Don't need volatile since we only deflate with mutators suspended.
  LockWord lw(obj->GetLockWord(false));
  // If the lock isn't an inflated monitor, then we don't need to deflate anything.
  if (lw.GetState() == LockWord::kFatLocked) {
    Monitor* monitor = lw.FatLockMonitor();
    DCHECK(monitor != nullptr);
    MutexLock mu(self, monitor->monitor_lock_);
    // Can't deflate if we have anybody waiting on the CV.
    if (monitor->num_waiters_ > 0) {
      return false;
    }
    Thread* owner = monitor->owner_;
    if (owner != nullptr) {
      // Can't deflate if we are locked and have a hash code.
      if (monitor->HasHashCode()) {
        return false;
      }
      // Can't deflate if our lock count is too high.
      if (monitor->lock_count_ > LockWord::kThinLockMaxCount) {
        return false;
      }
      // Deflate to a thin lock.
      LockWord new_lw = LockWord::FromThinLockId(owner->GetThreadId(), monitor->lock_count_,
                                                 lw.ReadBarrierState());
      // Assume no concurrent read barrier state changes as mutators are suspended.
      obj->SetLockWord(new_lw, false);
      VLOG(monitor) << "Deflated " << obj << " to thin lock " << owner->GetTid() << " / "
          << monitor->lock_count_;
    } else if (monitor->HasHashCode()) {
      LockWord new_lw = LockWord::FromHashCode(monitor->GetHashCode(), lw.ReadBarrierState());
      // Assume no concurrent read barrier state changes as mutators are suspended.
      obj->SetLockWord(new_lw, false);
      VLOG(monitor) << "Deflated " << obj << " to hash monitor " << monitor->GetHashCode();
    } else {
      // No lock and no hash, just put an empty lock word inside the object.
      LockWord new_lw = LockWord::FromDefault(lw.ReadBarrierState());
      // Assume no concurrent read barrier state changes as mutators are suspended.
      obj->SetLockWord(new_lw, false);
      VLOG(monitor) << "Deflated" << obj << " to empty lock word";
    }
    // The monitor is deflated, mark the object as null so that we know to delete it during the
    // next GC.
    monitor->obj_ = GcRoot<mirror::Object>(nullptr);
  }
  return true;
}

void Monitor::Inflate(Thread* self, Thread* owner, mirror::Object* obj, int32_t hash_code) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  // Allocate and acquire a new monitor.
  Monitor* m = MonitorPool::CreateMonitor(self, owner, obj, hash_code);
  DCHECK(m != nullptr);
  if (m->Install(self)) {
    if (owner != nullptr) {
      VLOG(monitor) << "monitor: thread" << owner->GetThreadId()
          << " created monitor " << m << " for object " << obj;
    } else {
      VLOG(monitor) << "monitor: Inflate with hashcode " << hash_code
          << " created monitor " << m << " for object " << obj;
    }
    Runtime::Current()->GetMonitorList()->Add(m);
    CHECK_EQ(obj->GetLockWord(true).GetState(), LockWord::kFatLocked);
  } else {
    MonitorPool::ReleaseMonitor(self, m);
  }
}

void Monitor::InflateThinLocked(Thread* self, Handle<mirror::Object> obj, LockWord lock_word,
                                uint32_t hash_code) {
  DCHECK_EQ(lock_word.GetState(), LockWord::kThinLocked);
  uint32_t owner_thread_id = lock_word.ThinLockOwner();
  if (owner_thread_id == self->GetThreadId()) {
    // We own the monitor, we can easily inflate it.
    Inflate(self, self, obj.Get(), hash_code);
  } else {
    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    // Suspend the owner, inflate. First change to blocked and give up mutator_lock_.
    self->SetMonitorEnterObject(obj.Get());
    bool timed_out;
    Thread* owner;
    {
      ScopedThreadSuspension sts(self, kBlocked);
      owner = thread_list->SuspendThreadByThreadId(owner_thread_id, false, &timed_out);
    }
    if (owner != nullptr) {
      // We succeeded in suspending the thread, check the lock's status didn't change.
      lock_word = obj->GetLockWord(true);
      if (lock_word.GetState() == LockWord::kThinLocked &&
          lock_word.ThinLockOwner() == owner_thread_id) {
        // Go ahead and inflate the lock.
        Inflate(self, owner, obj.Get(), hash_code);
      }
      thread_list->Resume(owner, false);
    }
    self->SetMonitorEnterObject(nullptr);
  }
}

// Fool annotalysis into thinking that the lock on obj is acquired.
static mirror::Object* FakeLock(mirror::Object* obj)
    EXCLUSIVE_LOCK_FUNCTION(obj) NO_THREAD_SAFETY_ANALYSIS {
  return obj;
}

// Fool annotalysis into thinking that the lock on obj is release.
static mirror::Object* FakeUnlock(mirror::Object* obj)
    UNLOCK_FUNCTION(obj) NO_THREAD_SAFETY_ANALYSIS {
  return obj;
}

mirror::Object* Monitor::MonitorEnter(Thread* self, mirror::Object* obj, bool trylock) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  self->AssertThreadSuspensionIsAllowable();
  obj = FakeLock(obj);
  uint32_t thread_id = self->GetThreadId();
  size_t contention_count = 0;
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_obj(hs.NewHandle(obj));
  while (true) {
    LockWord lock_word = h_obj->GetLockWord(true);
    switch (lock_word.GetState()) {
      case LockWord::kUnlocked: {
        LockWord thin_locked(LockWord::FromThinLockId(thread_id, 0, lock_word.ReadBarrierState()));
        if (h_obj->CasLockWordWeakSequentiallyConsistent(lock_word, thin_locked)) {
          AtraceMonitorLock(self, h_obj.Get(), false /* is_wait */);
          // CasLockWord enforces more than the acquire ordering we need here.
          return h_obj.Get();  // Success!
        }
        continue;  // Go again.
      }
      case LockWord::kThinLocked: {
        uint32_t owner_thread_id = lock_word.ThinLockOwner();
        if (owner_thread_id == thread_id) {
          // We own the lock, increase the recursion count.
          uint32_t new_count = lock_word.ThinLockCount() + 1;
          if (LIKELY(new_count <= LockWord::kThinLockMaxCount)) {
            LockWord thin_locked(LockWord::FromThinLockId(thread_id, new_count,
                                                          lock_word.ReadBarrierState()));
            if (!kUseReadBarrier) {
              h_obj->SetLockWord(thin_locked, true);
              AtraceMonitorLock(self, h_obj.Get(), false /* is_wait */);
              return h_obj.Get();  // Success!
            } else {
              // Use CAS to preserve the read barrier state.
              if (h_obj->CasLockWordWeakSequentiallyConsistent(lock_word, thin_locked)) {
                AtraceMonitorLock(self, h_obj.Get(), false /* is_wait */);
                return h_obj.Get();  // Success!
              }
            }
            continue;  // Go again.
          } else {
            // We'd overflow the recursion count, so inflate the monitor.
            InflateThinLocked(self, h_obj, lock_word, 0);
          }
        } else {
          if (trylock) {
            return nullptr;
          }
          // Contention.
          contention_count++;
          Runtime* runtime = Runtime::Current();
          if (contention_count <= runtime->GetMaxSpinsBeforeThinkLockInflation()) {
            // TODO: Consider switching the thread state to kBlocked when we are yielding.
            // Use sched_yield instead of NanoSleep since NanoSleep can wait much longer than the
            // parameter you pass in. This can cause thread suspension to take excessively long
            // and make long pauses. See b/16307460.
            sched_yield();
          } else {
            contention_count = 0;
            InflateThinLocked(self, h_obj, lock_word, 0);
          }
        }
        continue;  // Start from the beginning.
      }
      case LockWord::kFatLocked: {
        Monitor* mon = lock_word.FatLockMonitor();
        if (trylock) {
          return mon->TryLock(self) ? h_obj.Get() : nullptr;
        } else {
          mon->Lock(self);
          return h_obj.Get();  // Success!
        }
      }
      case LockWord::kHashCode:
        // Inflate with the existing hashcode.
        Inflate(self, nullptr, h_obj.Get(), lock_word.GetHashCode());
        continue;  // Start from the beginning.
      default: {
        LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
        UNREACHABLE();
      }
    }
  }
}

bool Monitor::MonitorExit(Thread* self, mirror::Object* obj) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  self->AssertThreadSuspensionIsAllowable();
  obj = FakeUnlock(obj);
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_obj(hs.NewHandle(obj));
  while (true) {
    LockWord lock_word = obj->GetLockWord(true);
    switch (lock_word.GetState()) {
      case LockWord::kHashCode:
        // Fall-through.
      case LockWord::kUnlocked:
        FailedUnlock(h_obj.Get(), self->GetThreadId(), 0u, nullptr);
        return false;  // Failure.
      case LockWord::kThinLocked: {
        uint32_t thread_id = self->GetThreadId();
        uint32_t owner_thread_id = lock_word.ThinLockOwner();
        if (owner_thread_id != thread_id) {
          FailedUnlock(h_obj.Get(), thread_id, owner_thread_id, nullptr);
          return false;  // Failure.
        } else {
          // We own the lock, decrease the recursion count.
          LockWord new_lw = LockWord::Default();
          if (lock_word.ThinLockCount() != 0) {
            uint32_t new_count = lock_word.ThinLockCount() - 1;
            new_lw = LockWord::FromThinLockId(thread_id, new_count, lock_word.ReadBarrierState());
          } else {
            new_lw = LockWord::FromDefault(lock_word.ReadBarrierState());
          }
          if (!kUseReadBarrier) {
            DCHECK_EQ(new_lw.ReadBarrierState(), 0U);
            h_obj->SetLockWord(new_lw, true);
            AtraceMonitorUnlock();
            // Success!
            return true;
          } else {
            // Use CAS to preserve the read barrier state.
            if (h_obj->CasLockWordWeakSequentiallyConsistent(lock_word, new_lw)) {
              AtraceMonitorUnlock();
              // Success!
              return true;
            }
          }
          continue;  // Go again.
        }
      }
      case LockWord::kFatLocked: {
        Monitor* mon = lock_word.FatLockMonitor();
        return mon->Unlock(self);
      }
      default: {
        LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
        return false;
      }
    }
  }
}

void Monitor::Wait(Thread* self, mirror::Object *obj, int64_t ms, int32_t ns,
                   bool interruptShouldThrow, ThreadState why) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  while (lock_word.GetState() != LockWord::kFatLocked) {
    switch (lock_word.GetState()) {
      case LockWord::kHashCode:
        // Fall-through.
      case LockWord::kUnlocked:
        ThrowIllegalMonitorStateExceptionF("object not locked by thread before wait()");
        return;  // Failure.
      case LockWord::kThinLocked: {
        uint32_t thread_id = self->GetThreadId();
        uint32_t owner_thread_id = lock_word.ThinLockOwner();
        if (owner_thread_id != thread_id) {
          ThrowIllegalMonitorStateExceptionF("object not locked by thread before wait()");
          return;  // Failure.
        } else {
          // We own the lock, inflate to enqueue ourself on the Monitor. May fail spuriously so
          // re-load.
          Inflate(self, self, obj, 0);
          lock_word = obj->GetLockWord(true);
        }
        break;
      }
      case LockWord::kFatLocked:  // Unreachable given the loop condition above. Fall-through.
      default: {
        LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
        return;
      }
    }
  }
  Monitor* mon = lock_word.FatLockMonitor();
  mon->Wait(self, ms, ns, interruptShouldThrow, why);
}

void Monitor::DoNotify(Thread* self, mirror::Object* obj, bool notify_all) {
  DCHECK(self != nullptr);
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
      // Fall-through.
    case LockWord::kUnlocked:
      ThrowIllegalMonitorStateExceptionF("object not locked by thread before notify()");
      return;  // Failure.
    case LockWord::kThinLocked: {
      uint32_t thread_id = self->GetThreadId();
      uint32_t owner_thread_id = lock_word.ThinLockOwner();
      if (owner_thread_id != thread_id) {
        ThrowIllegalMonitorStateExceptionF("object not locked by thread before notify()");
        return;  // Failure.
      } else {
        // We own the lock but there's no Monitor and therefore no waiters.
        return;  // Success.
      }
    }
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      if (notify_all) {
        mon->NotifyAll(self);
      } else {
        mon->Notify(self);
      }
      return;  // Success.
    }
    default: {
      LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
      return;
    }
  }
}

uint32_t Monitor::GetLockOwnerThreadId(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
      // Fall-through.
    case LockWord::kUnlocked:
      return ThreadList::kInvalidThreadId;
    case LockWord::kThinLocked:
      return lock_word.ThinLockOwner();
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      return mon->GetOwnerThreadId();
    }
    default: {
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
    }
  }
}

void Monitor::DescribeWait(std::ostream& os, const Thread* thread) {
  // Determine the wait message and object we're waiting or blocked upon.
  mirror::Object* pretty_object = nullptr;
  const char* wait_message = nullptr;
  uint32_t lock_owner = ThreadList::kInvalidThreadId;
  ThreadState state = thread->GetState();
  if (state == kWaiting || state == kTimedWaiting || state == kSleeping) {
    wait_message = (state == kSleeping) ? "  - sleeping on " : "  - waiting on ";
    Thread* self = Thread::Current();
    MutexLock mu(self, *thread->GetWaitMutex());
    Monitor* monitor = thread->GetWaitMonitor();
    if (monitor != nullptr) {
      pretty_object = monitor->GetObject();
    }
  } else if (state == kBlocked) {
    wait_message = "  - waiting to lock ";
    pretty_object = thread->GetMonitorEnterObject();
    if (pretty_object != nullptr) {
      lock_owner = pretty_object->GetLockOwnerThreadId();
    }
  }

  if (wait_message != nullptr) {
    if (pretty_object == nullptr) {
      os << wait_message << "an unknown object";
    } else {
      if ((pretty_object->GetLockWord(true).GetState() == LockWord::kThinLocked) &&
          Locks::mutator_lock_->IsExclusiveHeld(Thread::Current())) {
        // Getting the identity hashcode here would result in lock inflation and suspension of the
        // current thread, which isn't safe if this is the only runnable thread.
        os << wait_message << StringPrintf("<@addr=0x%" PRIxPTR "> (a %s)",
                                           reinterpret_cast<intptr_t>(pretty_object),
                                           PrettyTypeOf(pretty_object).c_str());
      } else {
        // - waiting on <0x6008c468> (a java.lang.Class<java.lang.ref.ReferenceQueue>)
        // Call PrettyTypeOf before IdentityHashCode since IdentityHashCode can cause thread
        // suspension and move pretty_object.
        const std::string pretty_type(PrettyTypeOf(pretty_object));
        os << wait_message << StringPrintf("<0x%08x> (a %s)", pretty_object->IdentityHashCode(),
                                           pretty_type.c_str());
      }
    }
    // - waiting to lock <0x613f83d8> (a java.lang.Object) held by thread 5
    if (lock_owner != ThreadList::kInvalidThreadId) {
      os << " held by thread " << lock_owner;
    }
    os << "\n";
  }
}

mirror::Object* Monitor::GetContendedMonitor(Thread* thread) {
  // This is used to implement JDWP's ThreadReference.CurrentContendedMonitor, and has a bizarre
  // definition of contended that includes a monitor a thread is trying to enter...
  mirror::Object* result = thread->GetMonitorEnterObject();
  if (result == nullptr) {
    // ...but also a monitor that the thread is waiting on.
    MutexLock mu(Thread::Current(), *thread->GetWaitMutex());
    Monitor* monitor = thread->GetWaitMonitor();
    if (monitor != nullptr) {
      result = monitor->GetObject();
    }
  }
  return result;
}

void Monitor::VisitLocks(StackVisitor* stack_visitor, void (*callback)(mirror::Object*, void*),
                         void* callback_context, bool abort_on_failure) {
  ArtMethod* m = stack_visitor->GetMethod();
  CHECK(m != nullptr);

  // Native methods are an easy special case.
  // TODO: use the JNI implementation's table of explicit MonitorEnter calls and dump those too.
  if (m->IsNative()) {
    if (m->IsSynchronized()) {
      mirror::Object* jni_this =
          stack_visitor->GetCurrentHandleScope(sizeof(void*))->GetReference(0);
      callback(jni_this, callback_context);
    }
    return;
  }

  // Proxy methods should not be synchronized.
  if (m->IsProxyMethod()) {
    CHECK(!m->IsSynchronized());
    return;
  }

  // Is there any reason to believe there's any synchronization in this method?
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  CHECK(code_item != nullptr) << PrettyMethod(m);
  if (code_item->tries_size_ == 0) {
    return;  // No "tries" implies no synchronization, so no held locks to report.
  }

  // Get the dex pc. If abort_on_failure is false, GetDexPc will not abort in the case it cannot
  // find the dex pc, and instead return kDexNoIndex. Then bail out, as it indicates we have an
  // inconsistent stack anyways.
  uint32_t dex_pc = stack_visitor->GetDexPc(abort_on_failure);
  if (!abort_on_failure && dex_pc == DexFile::kDexNoIndex) {
    LOG(ERROR) << "Could not find dex_pc for " << PrettyMethod(m);
    return;
  }

  // Ask the verifier for the dex pcs of all the monitor-enter instructions corresponding to
  // the locks held in this stack frame.
  std::vector<uint32_t> monitor_enter_dex_pcs;
  verifier::MethodVerifier::FindLocksAtDexPc(m, dex_pc, &monitor_enter_dex_pcs);
  for (uint32_t monitor_dex_pc : monitor_enter_dex_pcs) {
    // The verifier works in terms of the dex pcs of the monitor-enter instructions.
    // We want the registers used by those instructions (so we can read the values out of them).
    const Instruction* monitor_enter_instruction =
        Instruction::At(&code_item->insns_[monitor_dex_pc]);

    // Quick sanity check.
    CHECK_EQ(monitor_enter_instruction->Opcode(), Instruction::MONITOR_ENTER)
      << "expected monitor-enter @" << monitor_dex_pc << "; was "
      << reinterpret_cast<const void*>(monitor_enter_instruction);

    uint16_t monitor_register = monitor_enter_instruction->VRegA();
    uint32_t value;
    bool success = stack_visitor->GetVReg(m, monitor_register, kReferenceVReg, &value);
    CHECK(success) << "Failed to read v" << monitor_register << " of kind "
                   << kReferenceVReg << " in method " << PrettyMethod(m);
    mirror::Object* o = reinterpret_cast<mirror::Object*>(value);
    callback(o, callback_context);
  }
}

bool Monitor::IsValidLockWord(LockWord lock_word) {
  switch (lock_word.GetState()) {
    case LockWord::kUnlocked:
      // Nothing to check.
      return true;
    case LockWord::kThinLocked:
      // Basic sanity check of owner.
      return lock_word.ThinLockOwner() != ThreadList::kInvalidThreadId;
    case LockWord::kFatLocked: {
      // Check the  monitor appears in the monitor list.
      Monitor* mon = lock_word.FatLockMonitor();
      MonitorList* list = Runtime::Current()->GetMonitorList();
      MutexLock mu(Thread::Current(), list->monitor_list_lock_);
      for (Monitor* list_mon : list->list_) {
        if (mon == list_mon) {
          return true;  // Found our monitor.
        }
      }
      return false;  // Fail - unowned monitor in an object.
    }
    case LockWord::kHashCode:
      return true;
    default:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

bool Monitor::IsLocked() SHARED_REQUIRES(Locks::mutator_lock_) {
  MutexLock mu(Thread::Current(), monitor_lock_);
  return owner_ != nullptr;
}

void Monitor::TranslateLocation(ArtMethod* method,
                                uint32_t dex_pc,
                                const char** source_file,
                                int32_t* line_number) {
  // If method is null, location is unknown
  if (method == nullptr) {
    *source_file = "";
    *line_number = 0;
    return;
  }
  *source_file = method->GetDeclaringClassSourceFile();
  if (*source_file == nullptr) {
    *source_file = "";
  }
  *line_number = method->GetLineNumFromDexPC(dex_pc);
}

uint32_t Monitor::GetOwnerThreadId() {
  MutexLock mu(Thread::Current(), monitor_lock_);
  Thread* owner = owner_;
  if (owner != nullptr) {
    return owner->GetThreadId();
  } else {
    return ThreadList::kInvalidThreadId;
  }
}

MonitorList::MonitorList()
    : allow_new_monitors_(true), monitor_list_lock_("MonitorList lock", kMonitorListLock),
      monitor_add_condition_("MonitorList disallow condition", monitor_list_lock_) {
}

MonitorList::~MonitorList() {
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  // Release all monitors to the pool.
  // TODO: Is it an invariant that *all* open monitors are in the list? Then we could
  // clear faster in the pool.
  MonitorPool::ReleaseMonitors(self, &list_);
}

void MonitorList::DisallowNewMonitors() {
  CHECK(!kUseReadBarrier);
  MutexLock mu(Thread::Current(), monitor_list_lock_);
  allow_new_monitors_ = false;
}

void MonitorList::AllowNewMonitors() {
  CHECK(!kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  allow_new_monitors_ = true;
  monitor_add_condition_.Broadcast(self);
}

void MonitorList::BroadcastForNewMonitors() {
  CHECK(kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  monitor_add_condition_.Broadcast(self);
}

void MonitorList::Add(Monitor* m) {
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  while (UNLIKELY((!kUseReadBarrier && !allow_new_monitors_) ||
                  (kUseReadBarrier && !self->GetWeakRefAccessEnabled()))) {
    monitor_add_condition_.WaitHoldingLocks(self);
  }
  list_.push_front(m);
}

void MonitorList::SweepMonitorList(IsMarkedVisitor* visitor) {
  Thread* self = Thread::Current();
  MutexLock mu(self, monitor_list_lock_);
  for (auto it = list_.begin(); it != list_.end(); ) {
    Monitor* m = *it;
    // Disable the read barrier in GetObject() as this is called by GC.
    mirror::Object* obj = m->GetObject<kWithoutReadBarrier>();
    // The object of a monitor can be null if we have deflated it.
    mirror::Object* new_obj = obj != nullptr ? visitor->IsMarked(obj) : nullptr;
    if (new_obj == nullptr) {
      VLOG(monitor) << "freeing monitor " << m << " belonging to unmarked object "
                    << obj;
      MonitorPool::ReleaseMonitor(self, m);
      it = list_.erase(it);
    } else {
      m->SetObject(new_obj);
      ++it;
    }
  }
}

class MonitorDeflateVisitor : public IsMarkedVisitor {
 public:
  MonitorDeflateVisitor() : self_(Thread::Current()), deflate_count_(0) {}

  virtual mirror::Object* IsMarked(mirror::Object* object) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (Monitor::Deflate(self_, object)) {
      DCHECK_NE(object->GetLockWord(true).GetState(), LockWord::kFatLocked);
      ++deflate_count_;
      // If we deflated, return null so that the monitor gets removed from the array.
      return nullptr;
    }
    return object;  // Monitor was not deflated.
  }

  Thread* const self_;
  size_t deflate_count_;
};

size_t MonitorList::DeflateMonitors() {
  MonitorDeflateVisitor visitor;
  Locks::mutator_lock_->AssertExclusiveHeld(visitor.self_);
  SweepMonitorList(&visitor);
  return visitor.deflate_count_;
}

MonitorInfo::MonitorInfo(mirror::Object* obj) : owner_(nullptr), entry_count_(0) {
  DCHECK(obj != nullptr);
  LockWord lock_word = obj->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kUnlocked:
      // Fall-through.
    case LockWord::kForwardingAddress:
      // Fall-through.
    case LockWord::kHashCode:
      break;
    case LockWord::kThinLocked:
      owner_ = Runtime::Current()->GetThreadList()->FindThreadByThreadId(lock_word.ThinLockOwner());
      entry_count_ = 1 + lock_word.ThinLockCount();
      // Thin locks have no waiters.
      break;
    case LockWord::kFatLocked: {
      Monitor* mon = lock_word.FatLockMonitor();
      owner_ = mon->owner_;
      entry_count_ = 1 + mon->lock_count_;
      for (Thread* waiter = mon->wait_set_; waiter != nullptr; waiter = waiter->GetWaitNext()) {
        waiters_.push_back(waiter);
      }
      break;
    }
  }
}

}  // namespace art
