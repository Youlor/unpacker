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

#include "task_processor.h"

#include "base/time_utils.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace gc {

TaskProcessor::TaskProcessor()
    : lock_(new Mutex("Task processor lock", kReferenceProcessorLock)), is_running_(false),
      running_thread_(nullptr) {
  // Piggyback off the reference processor lock level.
  cond_.reset(new ConditionVariable("Task processor condition", *lock_));
}

TaskProcessor::~TaskProcessor() {
  delete lock_;
}

void TaskProcessor::AddTask(Thread* self, HeapTask* task) {
  ScopedThreadStateChange tsc(self, kBlocked);
  MutexLock mu(self, *lock_);
  tasks_.insert(task);
  cond_->Signal(self);
}

HeapTask* TaskProcessor::GetTask(Thread* self) {
  ScopedThreadStateChange tsc(self, kBlocked);
  MutexLock mu(self, *lock_);
  while (true) {
    if (tasks_.empty()) {
      if (!is_running_) {
        return nullptr;
      }
      cond_->Wait(self);  // Empty queue, wait until we are signalled.
    } else {
      // Non empty queue, look at the top element and see if we are ready to run it.
      const uint64_t current_time = NanoTime();
      HeapTask* task = *tasks_.begin();
      // If we are shutting down, return the task right away without waiting. Otherwise return the
      // task if it is late enough.
      uint64_t target_time = task->GetTargetRunTime();
      if (!is_running_ || target_time <= current_time) {
        tasks_.erase(tasks_.begin());
        return task;
      }
      DCHECK_GT(target_time, current_time);
      // Wait untl we hit the target run time.
      const uint64_t delta_time = target_time - current_time;
      const uint64_t ms_delta = NsToMs(delta_time);
      const uint64_t ns_delta = delta_time - MsToNs(ms_delta);
      cond_->TimedWait(self, static_cast<int64_t>(ms_delta), static_cast<int32_t>(ns_delta));
    }
  }
  UNREACHABLE();
}

void TaskProcessor::UpdateTargetRunTime(Thread* self, HeapTask* task, uint64_t new_target_time) {
  MutexLock mu(self, *lock_);
  // Find the task.
  auto range = tasks_.equal_range(task);
  for (auto it = range.first; it != range.second; ++it) {
    if (*it == task) {
      // Check if the target time was updated, if so re-insert then wait.
      if (new_target_time != task->GetTargetRunTime()) {
        tasks_.erase(it);
        task->SetTargetRunTime(new_target_time);
        tasks_.insert(task);
        // If we became the first task then we may need to signal since we changed the task that we
        // are sleeping on.
        if (*tasks_.begin() == task) {
          cond_->Signal(self);
        }
        return;
      }
    }
  }
}

bool TaskProcessor::IsRunning() const {
  MutexLock mu(Thread::Current(), *lock_);
  return is_running_;
}

Thread* TaskProcessor::GetRunningThread() const {
  MutexLock mu(Thread::Current(), *lock_);
  return running_thread_;
}

void TaskProcessor::Stop(Thread* self) {
  MutexLock mu(self, *lock_);
  is_running_ = false;
  running_thread_ = nullptr;
  cond_->Broadcast(self);
}

void TaskProcessor::Start(Thread* self) {
  MutexLock mu(self, *lock_);
  is_running_ = true;
  running_thread_ = self;
}

void TaskProcessor::RunAllTasks(Thread* self) {
  while (true) {
    // Wait and get a task, may be interrupted.
    HeapTask* task = GetTask(self);
    if (task != nullptr) {
      task->Run(self);
      task->Finalize();
    } else if (!IsRunning()) {
      break;
    }
  }
}

}  // namespace gc
}  // namespace art
