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

#include "thread_pool.h"

#include <pthread.h>

#include <sys/time.h>
#include <sys/resource.h>

#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/time_utils.h"
#include "runtime.h"
#include "thread-inl.h"

namespace art {

static constexpr bool kMeasureWaitTime = false;

ThreadPoolWorker::ThreadPoolWorker(ThreadPool* thread_pool, const std::string& name,
                                   size_t stack_size)
    : thread_pool_(thread_pool),
      name_(name) {
  // Add an inaccessible page to catch stack overflow.
  stack_size += kPageSize;
  std::string error_msg;
  stack_.reset(MemMap::MapAnonymous(name.c_str(), nullptr, stack_size, PROT_READ | PROT_WRITE,
                                    false, false, &error_msg));
  CHECK(stack_.get() != nullptr) << error_msg;
  CHECK_ALIGNED(stack_->Begin(), kPageSize);
  int mprotect_result = mprotect(stack_->Begin(), kPageSize, PROT_NONE);
  CHECK_EQ(mprotect_result, 0) << "Failed to mprotect() bottom page of thread pool worker stack.";
  const char* reason = "new thread pool worker thread";
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), reason);
  CHECK_PTHREAD_CALL(pthread_attr_setstack, (&attr, stack_->Begin(), stack_->Size()), reason);
  CHECK_PTHREAD_CALL(pthread_create, (&pthread_, &attr, &Callback, this), reason);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), reason);
}

ThreadPoolWorker::~ThreadPoolWorker() {
  CHECK_PTHREAD_CALL(pthread_join, (pthread_, nullptr), "thread pool worker shutdown");
}

void ThreadPoolWorker::SetPthreadPriority(int priority) {
  CHECK_GE(priority, PRIO_MIN);
  CHECK_LE(priority, PRIO_MAX);
#if defined(__ANDROID__)
  int result = setpriority(PRIO_PROCESS, pthread_gettid_np(pthread_), priority);
  if (result != 0) {
    PLOG(ERROR) << "Failed to setpriority to :" << priority;
  }
#else
  UNUSED(priority);
#endif
}

void ThreadPoolWorker::Run() {
  Thread* self = Thread::Current();
  Task* task = nullptr;
  thread_pool_->creation_barier_.Wait(self);
  while ((task = thread_pool_->GetTask(self)) != nullptr) {
    task->Run(self);
    task->Finalize();
  }
}

void* ThreadPoolWorker::Callback(void* arg) {
  ThreadPoolWorker* worker = reinterpret_cast<ThreadPoolWorker*>(arg);
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->AttachCurrentThread(worker->name_.c_str(),
                                     true,
                                     nullptr,
                                     worker->thread_pool_->create_peers_));
  // Thread pool workers cannot call into java.
  Thread::Current()->SetCanCallIntoJava(false);
  // Do work until its time to shut down.
  worker->Run();
  runtime->DetachCurrentThread();
  return nullptr;
}

void ThreadPool::AddTask(Thread* self, Task* task) {
  MutexLock mu(self, task_queue_lock_);
  tasks_.push_back(task);
  // If we have any waiters, signal one.
  if (started_ && waiting_count_ != 0) {
    task_queue_condition_.Signal(self);
  }
}

void ThreadPool::RemoveAllTasks(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  tasks_.clear();
}

ThreadPool::ThreadPool(const char* name, size_t num_threads, bool create_peers)
  : name_(name),
    task_queue_lock_("task queue lock"),
    task_queue_condition_("task queue condition", task_queue_lock_),
    completion_condition_("task completion condition", task_queue_lock_),
    started_(false),
    shutting_down_(false),
    waiting_count_(0),
    start_time_(0),
    total_wait_time_(0),
    // Add one since the caller of constructor waits on the barrier too.
    creation_barier_(num_threads + 1),
    max_active_workers_(num_threads),
    create_peers_(create_peers) {
  Thread* self = Thread::Current();
  while (GetThreadCount() < num_threads) {
    const std::string worker_name = StringPrintf("%s worker thread %zu", name_.c_str(),
                                                 GetThreadCount());
    threads_.push_back(
        new ThreadPoolWorker(this, worker_name, ThreadPoolWorker::kDefaultStackSize));
  }
  // Wait for all of the threads to attach.
  creation_barier_.Wait(self);
}

void ThreadPool::SetMaxActiveWorkers(size_t threads) {
  MutexLock mu(Thread::Current(), task_queue_lock_);
  CHECK_LE(threads, GetThreadCount());
  max_active_workers_ = threads;
}

ThreadPool::~ThreadPool() {
  {
    Thread* self = Thread::Current();
    MutexLock mu(self, task_queue_lock_);
    // Tell any remaining workers to shut down.
    shutting_down_ = true;
    // Broadcast to everyone waiting.
    task_queue_condition_.Broadcast(self);
    completion_condition_.Broadcast(self);
  }
  // Wait for the threads to finish.
  STLDeleteElements(&threads_);
}

void ThreadPool::StartWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = true;
  task_queue_condition_.Broadcast(self);
  start_time_ = NanoTime();
  total_wait_time_ = 0;
}

void ThreadPool::StopWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = false;
}

Task* ThreadPool::GetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  while (!IsShuttingDown()) {
    const size_t thread_count = GetThreadCount();
    // Ensure that we don't use more threads than the maximum active workers.
    const size_t active_threads = thread_count - waiting_count_;
    // <= since self is considered an active worker.
    if (active_threads <= max_active_workers_) {
      Task* task = TryGetTaskLocked();
      if (task != nullptr) {
        return task;
      }
    }

    ++waiting_count_;
    if (waiting_count_ == GetThreadCount() && tasks_.empty()) {
      // We may be done, lets broadcast to the completion condition.
      completion_condition_.Broadcast(self);
    }
    const uint64_t wait_start = kMeasureWaitTime ? NanoTime() : 0;
    task_queue_condition_.Wait(self);
    if (kMeasureWaitTime) {
      const uint64_t wait_end = NanoTime();
      total_wait_time_ += wait_end - std::max(wait_start, start_time_);
    }
    --waiting_count_;
  }

  // We are shutting down, return null to tell the worker thread to stop looping.
  return nullptr;
}

Task* ThreadPool::TryGetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return TryGetTaskLocked();
}

Task* ThreadPool::TryGetTaskLocked() {
  if (started_ && !tasks_.empty()) {
    Task* task = tasks_.front();
    tasks_.pop_front();
    return task;
  }
  return nullptr;
}

void ThreadPool::Wait(Thread* self, bool do_work, bool may_hold_locks) {
  if (do_work) {
    CHECK(!create_peers_);
    Task* task = nullptr;
    while ((task = TryGetTask(self)) != nullptr) {
      task->Run(self);
      task->Finalize();
    }
  }
  // Wait until each thread is waiting and the task list is empty.
  MutexLock mu(self, task_queue_lock_);
  while (!shutting_down_ && (waiting_count_ != GetThreadCount() || !tasks_.empty())) {
    if (!may_hold_locks) {
      completion_condition_.Wait(self);
    } else {
      completion_condition_.WaitHoldingLocks(self);
    }
  }
}

size_t ThreadPool::GetTaskCount(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return tasks_.size();
}

void ThreadPool::SetPthreadPriority(int priority) {
  for (ThreadPoolWorker* worker : threads_) {
    worker->SetPthreadPriority(priority);
  }
}

}  // namespace art
