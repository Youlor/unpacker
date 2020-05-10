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

#include "base/time_utils.h"
#include "common_runtime_test.h"
#include "task_processor.h"
#include "thread_pool.h"
#include "thread-inl.h"

namespace art {
namespace gc {

class TaskProcessorTest : public CommonRuntimeTest {
 public:
};

class RecursiveTask : public HeapTask {
 public:
  RecursiveTask(TaskProcessor* task_processor, Atomic<size_t>* counter, size_t max_recursion)
     : HeapTask(NanoTime() + MsToNs(10)), task_processor_(task_processor), counter_(counter),
       max_recursion_(max_recursion) {
  }
  virtual void Run(Thread* self) OVERRIDE {
    if (max_recursion_ > 0) {
      task_processor_->AddTask(self,
                               new RecursiveTask(task_processor_, counter_, max_recursion_ - 1));
      counter_->FetchAndAddSequentiallyConsistent(1U);
    }
  }

 private:
  TaskProcessor* const task_processor_;
  Atomic<size_t>* const counter_;
  const size_t max_recursion_;
};

class WorkUntilDoneTask : public SelfDeletingTask {
 public:
  WorkUntilDoneTask(TaskProcessor* task_processor, Atomic<bool>* done_running)
      : task_processor_(task_processor), done_running_(done_running) {
  }
  virtual void Run(Thread* self) OVERRIDE {
    task_processor_->RunAllTasks(self);
    done_running_->StoreSequentiallyConsistent(true);
  }

 private:
  TaskProcessor* const task_processor_;
  Atomic<bool>* done_running_;
};

TEST_F(TaskProcessorTest, Interrupt) {
  ThreadPool thread_pool("task processor test", 1U);
  Thread* const self = Thread::Current();
  TaskProcessor task_processor;
  static constexpr size_t kRecursion = 10;
  Atomic<bool> done_running(false);
  Atomic<size_t> counter(0);
  task_processor.AddTask(self, new RecursiveTask(&task_processor, &counter, kRecursion));
  task_processor.Start(self);
  // Add a task which will wait until interrupted to the thread pool.
  thread_pool.AddTask(self, new WorkUntilDoneTask(&task_processor, &done_running));
  thread_pool.StartWorkers(self);
  ASSERT_FALSE(done_running);
  // Wait until all the tasks are done, but since we didn't interrupt, done_running should be 0.
  while (counter.LoadSequentiallyConsistent() != kRecursion) {
    usleep(10);
  }
  ASSERT_FALSE(done_running);
  task_processor.Stop(self);
  thread_pool.Wait(self, true, false);
  // After the interrupt and wait, the WorkUntilInterruptedTasktask should have terminated and
  // set done_running_ to true.
  ASSERT_TRUE(done_running.LoadSequentiallyConsistent());

  // Test that we finish remaining tasks before returning from RunTasksUntilInterrupted.
  counter.StoreSequentiallyConsistent(0);
  done_running.StoreSequentiallyConsistent(false);
  // Self interrupt before any of the other tasks run, but since we added them we should keep on
  // working until all the tasks are completed.
  task_processor.Stop(self);
  task_processor.AddTask(self, new RecursiveTask(&task_processor, &counter, kRecursion));
  thread_pool.AddTask(self, new WorkUntilDoneTask(&task_processor, &done_running));
  thread_pool.StartWorkers(self);
  thread_pool.Wait(self, true, false);
  ASSERT_TRUE(done_running.LoadSequentiallyConsistent());
  ASSERT_EQ(counter.LoadSequentiallyConsistent(), kRecursion);
}

class TestOrderTask : public HeapTask {
 public:
  TestOrderTask(uint64_t expected_time, size_t expected_counter, size_t* counter)
     : HeapTask(expected_time), expected_counter_(expected_counter), counter_(counter) {
  }
  virtual void Run(Thread* thread ATTRIBUTE_UNUSED) OVERRIDE {
    ASSERT_EQ(*counter_, expected_counter_);
    ++*counter_;
  }

 private:
  const size_t expected_counter_;
  size_t* const counter_;
};

TEST_F(TaskProcessorTest, Ordering) {
  static const size_t kNumTasks = 25;
  const uint64_t current_time = NanoTime();
  Thread* const self = Thread::Current();
  TaskProcessor task_processor;
  task_processor.Stop(self);
  size_t counter = 0;
  std::vector<std::pair<uint64_t, size_t>> orderings;
  for (size_t i = 0; i < kNumTasks; ++i) {
    orderings.push_back(std::make_pair(current_time + MsToNs(10U * i), i));
  }
  for (size_t i = 0; i < kNumTasks; ++i) {
    std::swap(orderings[i], orderings[(i * 87654231 + 12345) % orderings.size()]);
  }
  for (const auto& pair : orderings) {
    auto* task = new TestOrderTask(pair.first, pair.second, &counter);
    task_processor.AddTask(self, task);
  }
  ThreadPool thread_pool("task processor test", 1U);
  Atomic<bool> done_running(false);
  // Add a task which will wait until interrupted to the thread pool.
  thread_pool.AddTask(self, new WorkUntilDoneTask(&task_processor, &done_running));
  ASSERT_FALSE(done_running.LoadSequentiallyConsistent());
  thread_pool.StartWorkers(self);
  thread_pool.Wait(self, true, false);
  ASSERT_TRUE(done_running.LoadSequentiallyConsistent());
  ASSERT_EQ(counter, kNumTasks);
}

}  // namespace gc
}  // namespace art
