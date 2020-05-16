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

#include "barrier.h"
#include "monitor.h"

#include <string>

#include "atomic.h"
#include "base/time_utils.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "mirror/string-inl.h"  // Strings are easiest to allocate
#include "object_lock.h"
#include "scoped_thread_state_change.h"
#include "thread_pool.h"

namespace art {

class MonitorTest : public CommonRuntimeTest {
 protected:
  void SetUpRuntimeOptions(RuntimeOptions *options) OVERRIDE {
    // Use a smaller heap
    for (std::pair<std::string, const void*>& pair : *options) {
      if (pair.first.find("-Xmx") == 0) {
        pair.first = "-Xmx4M";  // Smallest we can go.
      }
    }
    options->push_back(std::make_pair("-Xint", nullptr));
  }
 public:
  std::unique_ptr<Monitor> monitor_;
  Handle<mirror::String> object_;
  Handle<mirror::String> second_object_;
  Handle<mirror::String> watchdog_object_;
  // One exception test is for waiting on another Thread's lock. This is used to race-free &
  // loop-free pass
  Thread* thread_;
  std::unique_ptr<Barrier> barrier_;
  std::unique_ptr<Barrier> complete_barrier_;
  bool completed_;
};

// Fill the heap.
static const size_t kMaxHandles = 1000000;  // Use arbitrary large amount for now.
static void FillHeap(Thread* self, ClassLinker* class_linker,
                     std::unique_ptr<StackHandleScope<kMaxHandles>>* hsp,
                     std::vector<MutableHandle<mirror::Object>>* handles)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Runtime::Current()->GetHeap()->SetIdealFootprint(1 * GB);

  hsp->reset(new StackHandleScope<kMaxHandles>(self));
  // Class java.lang.Object.
  Handle<mirror::Class> c((*hsp)->NewHandle(class_linker->FindSystemClass(self,
                                                                       "Ljava/lang/Object;")));
  // Array helps to fill memory faster.
  Handle<mirror::Class> ca((*hsp)->NewHandle(class_linker->FindSystemClass(self,
                                                                        "[Ljava/lang/Object;")));

  // Start allocating with 128K
  size_t length = 128 * KB / 4;
  while (length > 10) {
    MutableHandle<mirror::Object> h((*hsp)->NewHandle<mirror::Object>(
        mirror::ObjectArray<mirror::Object>::Alloc(self, ca.Get(), length / 4)));
    if (self->IsExceptionPending() || h.Get() == nullptr) {
      self->ClearException();

      // Try a smaller length
      length = length / 8;
      // Use at most half the reported free space.
      size_t mem = Runtime::Current()->GetHeap()->GetFreeMemory();
      if (length * 8 > mem) {
        length = mem / 8;
      }
    } else {
      handles->push_back(h);
    }
  }

  // Allocate simple objects till it fails.
  while (!self->IsExceptionPending()) {
    MutableHandle<mirror::Object> h = (*hsp)->NewHandle<mirror::Object>(c->AllocObject(self));
    if (!self->IsExceptionPending() && h.Get() != nullptr) {
      handles->push_back(h);
    }
  }
  self->ClearException();
}

// Check that an exception can be thrown correctly.
// This test is potentially racy, but the timeout is long enough that it should work.

class CreateTask : public Task {
 public:
  CreateTask(MonitorTest* monitor_test, uint64_t initial_sleep, int64_t millis, bool expected) :
      monitor_test_(monitor_test), initial_sleep_(initial_sleep), millis_(millis),
      expected_(expected) {}

  void Run(Thread* self) {
    {
      ScopedObjectAccess soa(self);

      monitor_test_->thread_ = self;        // Pass the Thread.
      monitor_test_->object_.Get()->MonitorEnter(self);  // Lock the object. This should transition
      LockWord lock_after = monitor_test_->object_.Get()->GetLockWord(false);  // it to thinLocked.
      LockWord::LockState new_state = lock_after.GetState();

      // Cannot use ASSERT only, as analysis thinks we'll keep holding the mutex.
      if (LockWord::LockState::kThinLocked != new_state) {
        monitor_test_->object_.Get()->MonitorExit(self);         // To appease analysis.
        ASSERT_EQ(LockWord::LockState::kThinLocked, new_state);  // To fail the test.
        return;
      }

      // Force a fat lock by running identity hashcode to fill up lock word.
      monitor_test_->object_.Get()->IdentityHashCode();
      LockWord lock_after2 = monitor_test_->object_.Get()->GetLockWord(false);
      LockWord::LockState new_state2 = lock_after2.GetState();

      // Cannot use ASSERT only, as analysis thinks we'll keep holding the mutex.
      if (LockWord::LockState::kFatLocked != new_state2) {
        monitor_test_->object_.Get()->MonitorExit(self);         // To appease analysis.
        ASSERT_EQ(LockWord::LockState::kFatLocked, new_state2);  // To fail the test.
        return;
      }
    }  // Need to drop the mutator lock to use the barrier.

    monitor_test_->barrier_->Wait(self);           // Let the other thread know we're done.

    {
      ScopedObjectAccess soa(self);

      // Give the other task a chance to do its thing.
      NanoSleep(initial_sleep_ * 1000 * 1000);

      // Now try to Wait on the Monitor.
      Monitor::Wait(self, monitor_test_->object_.Get(), millis_, 0, true,
                    ThreadState::kTimedWaiting);

      // Check the exception status against what we expect.
      EXPECT_EQ(expected_, self->IsExceptionPending());
      if (expected_) {
        self->ClearException();
      }
    }

    monitor_test_->complete_barrier_->Wait(self);  // Wait for test completion.

    {
      ScopedObjectAccess soa(self);
      monitor_test_->object_.Get()->MonitorExit(self);  // Release the object. Appeases analysis.
    }
  }

  void Finalize() {
    delete this;
  }

 private:
  MonitorTest* monitor_test_;
  uint64_t initial_sleep_;
  int64_t millis_;
  bool expected_;
};


class UseTask : public Task {
 public:
  UseTask(MonitorTest* monitor_test, uint64_t initial_sleep, int64_t millis, bool expected) :
      monitor_test_(monitor_test), initial_sleep_(initial_sleep), millis_(millis),
      expected_(expected) {}

  void Run(Thread* self) {
    monitor_test_->barrier_->Wait(self);  // Wait for the other thread to set up the monitor.

    {
      ScopedObjectAccess soa(self);

      // Give the other task a chance to do its thing.
      NanoSleep(initial_sleep_ * 1000 * 1000);

      Monitor::Wait(self, monitor_test_->object_.Get(), millis_, 0, true,
                    ThreadState::kTimedWaiting);

      // Check the exception status against what we expect.
      EXPECT_EQ(expected_, self->IsExceptionPending());
      if (expected_) {
        self->ClearException();
      }
    }

    monitor_test_->complete_barrier_->Wait(self);  // Wait for test completion.
  }

  void Finalize() {
    delete this;
  }

 private:
  MonitorTest* monitor_test_;
  uint64_t initial_sleep_;
  int64_t millis_;
  bool expected_;
};

class InterruptTask : public Task {
 public:
  InterruptTask(MonitorTest* monitor_test, uint64_t initial_sleep, uint64_t millis) :
      monitor_test_(monitor_test), initial_sleep_(initial_sleep), millis_(millis) {}

  void Run(Thread* self) {
    monitor_test_->barrier_->Wait(self);  // Wait for the other thread to set up the monitor.

    {
      ScopedObjectAccess soa(self);

      // Give the other task a chance to do its thing.
      NanoSleep(initial_sleep_ * 1000 * 1000);

      // Interrupt the other thread.
      monitor_test_->thread_->Interrupt(self);

      // Give it some more time to get to the exception code.
      NanoSleep(millis_ * 1000 * 1000);

      // Now try to Wait.
      Monitor::Wait(self, monitor_test_->object_.Get(), 10, 0, true,
                    ThreadState::kTimedWaiting);

      // No check here, as depending on scheduling we may or may not fail.
      if (self->IsExceptionPending()) {
        self->ClearException();
      }
    }

    monitor_test_->complete_barrier_->Wait(self);  // Wait for test completion.
  }

  void Finalize() {
    delete this;
  }

 private:
  MonitorTest* monitor_test_;
  uint64_t initial_sleep_;
  uint64_t millis_;
};

class WatchdogTask : public Task {
 public:
  explicit WatchdogTask(MonitorTest* monitor_test) : monitor_test_(monitor_test) {}

  void Run(Thread* self) {
    ScopedObjectAccess soa(self);

    monitor_test_->watchdog_object_.Get()->MonitorEnter(self);        // Lock the object.

    monitor_test_->watchdog_object_.Get()->Wait(self, 30 * 1000, 0);  // Wait for 30s, or being
                                                                      // woken up.

    monitor_test_->watchdog_object_.Get()->MonitorExit(self);         // Release the lock.

    if (!monitor_test_->completed_) {
      LOG(FATAL) << "Watchdog timeout!";
    }
  }

  void Finalize() {
    delete this;
  }

 private:
  MonitorTest* monitor_test_;
};

static void CommonWaitSetup(MonitorTest* test, ClassLinker* class_linker, uint64_t create_sleep,
                            int64_t c_millis, bool c_expected, bool interrupt, uint64_t use_sleep,
                            int64_t u_millis, bool u_expected, const char* pool_name) {
  Thread* const self = Thread::Current();
  ScopedObjectAccess soa(self);
  // First create the object we lock. String is easiest.
  StackHandleScope<3> hs(soa.Self());
  test->object_ = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "hello, world!"));
  test->watchdog_object_ = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self,
                                                                              "hello, world!"));

  // Create the barrier used to synchronize.
  test->barrier_ = std::unique_ptr<Barrier>(new Barrier(2));
  test->complete_barrier_ = std::unique_ptr<Barrier>(new Barrier(3));
  test->completed_ = false;

  // Fill the heap.
  std::unique_ptr<StackHandleScope<kMaxHandles>> hsp;
  std::vector<MutableHandle<mirror::Object>> handles;

  // Our job: Fill the heap, then try Wait.
  FillHeap(soa.Self(), class_linker, &hsp, &handles);

  // Now release everything.
  for (MutableHandle<mirror::Object>& h : handles) {
    h.Assign(nullptr);
  }

  // Need to drop the mutator lock to allow barriers.
  ScopedThreadSuspension sts(soa.Self(), kNative);
  ThreadPool thread_pool(pool_name, 3);
  thread_pool.AddTask(self, new CreateTask(test, create_sleep, c_millis, c_expected));
  if (interrupt) {
    thread_pool.AddTask(self, new InterruptTask(test, use_sleep, static_cast<uint64_t>(u_millis)));
  } else {
    thread_pool.AddTask(self, new UseTask(test, use_sleep, u_millis, u_expected));
  }
  thread_pool.AddTask(self, new WatchdogTask(test));
  thread_pool.StartWorkers(self);

  // Wait on completion barrier.
  test->complete_barrier_->Wait(self);
  test->completed_ = true;

  // Wake the watchdog.
  {
    ScopedObjectAccess soa2(self);
    test->watchdog_object_.Get()->MonitorEnter(self);     // Lock the object.
    test->watchdog_object_.Get()->NotifyAll(self);        // Wake up waiting parties.
    test->watchdog_object_.Get()->MonitorExit(self);      // Release the lock.
  }

  thread_pool.StopWorkers(self);
}


// First test: throwing an exception when trying to wait in Monitor with another thread.
TEST_F(MonitorTest, CheckExceptionsWait1) {
  // Make the CreateTask wait 10ms, the UseTask wait 10ms.
  // => The use task will get the lock first and get to self == owner check.
  // This will lead to OOM and monitor error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);
  CommonWaitSetup(this, class_linker_, 10, 50, false, false, 2, 50, true,
                  "Monitor test thread pool 1");
}

// Second test: throwing an exception for invalid wait time.
TEST_F(MonitorTest, CheckExceptionsWait2) {
  // Make the CreateTask wait 0ms, the UseTask wait 10ms.
  // => The create task will get the lock first and get to ms >= 0
  // This will lead to OOM and monitor error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);
  CommonWaitSetup(this, class_linker_, 0, -1, true, false, 10, 50, true,
                  "Monitor test thread pool 2");
}

// Third test: throwing an interrupted-exception.
TEST_F(MonitorTest, CheckExceptionsWait3) {
  // Make the CreateTask wait 0ms, then Wait for a long time. Make the InterruptTask wait 10ms,
  // after which it will interrupt the create task and then wait another 10ms.
  // => The create task will get to the interrupted-exception throw.
  // This will lead to OOM and monitor error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);
  CommonWaitSetup(this, class_linker_, 0, 500, true, true, 10, 50, true,
                  "Monitor test thread pool 3");
}

class TryLockTask : public Task {
 public:
  explicit TryLockTask(Handle<mirror::Object> obj) : obj_(obj) {}

  void Run(Thread* self) {
    ScopedObjectAccess soa(self);
    // Lock is held by other thread, try lock should fail.
    ObjectTryLock<mirror::Object> lock(self, obj_);
    EXPECT_FALSE(lock.Acquired());
  }

  void Finalize() {
    delete this;
  }

 private:
  Handle<mirror::Object> obj_;
};

// Test trylock in deadlock scenarios.
TEST_F(MonitorTest, TestTryLock) {
  ScopedLogSeverity sls(LogSeverity::FATAL);

  Thread* const self = Thread::Current();
  ThreadPool thread_pool("the pool", 2);
  ScopedObjectAccess soa(self);
  StackHandleScope<3> hs(self);
  Handle<mirror::Object> obj1(
      hs.NewHandle<mirror::Object>(mirror::String::AllocFromModifiedUtf8(self, "hello, world!")));
  Handle<mirror::Object> obj2(
      hs.NewHandle<mirror::Object>(mirror::String::AllocFromModifiedUtf8(self, "hello, world!")));
  {
    ObjectLock<mirror::Object> lock1(self, obj1);
    ObjectLock<mirror::Object> lock2(self, obj1);
    {
      ObjectTryLock<mirror::Object> trylock(self, obj1);
      EXPECT_TRUE(trylock.Acquired());
    }
    // Test failure case.
    thread_pool.AddTask(self, new TryLockTask(obj1));
    thread_pool.StartWorkers(self);
    ScopedThreadSuspension sts(self, kSuspended);
    thread_pool.Wait(Thread::Current(), /*do_work*/false, /*may_hold_locks*/false);
  }
  // Test that the trylock actually locks the object.
  {
    ObjectTryLock<mirror::Object> trylock(self, obj1);
    EXPECT_TRUE(trylock.Acquired());
    obj1->Notify(self);
    // Since we hold the lock there should be no monitor state exeception.
    self->AssertNoPendingException();
  }
  thread_pool.StopWorkers(self);
}


}  // namespace art
