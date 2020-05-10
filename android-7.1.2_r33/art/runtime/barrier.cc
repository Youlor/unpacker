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

#include "barrier.h"

#include "base/logging.h"
#include "base/mutex.h"
#include "base/time_utils.h"
#include "thread.h"

namespace art {

Barrier::Barrier(int count)
    : count_(count),
      lock_("GC barrier lock", kThreadSuspendCountLock),
      condition_("GC barrier condition", lock_) {
}

void Barrier::Pass(Thread* self) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count_ - 1);
}

void Barrier::Wait(Thread* self) {
  Increment(self, -1);
}

void Barrier::Init(Thread* self, int count) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count);
}

void Barrier::Increment(Thread* self, int delta) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count_ + delta);

  // Increment the count.  If it becomes zero after the increment
  // then all the threads have already passed the barrier.  If
  // it is non-zero then there is still one or more threads
  // that have not yet called the Pass function.  When the
  // Pass function is called by the last thread, the count will
  // be decremented to zero and a Broadcast will be made on the
  // condition variable, thus waking this up.
  while (count_ != 0) {
    condition_.Wait(self);
  }
}

bool Barrier::Increment(Thread* self, int delta, uint32_t timeout_ms) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count_ + delta);
  bool timed_out = false;
  if (count_ != 0) {
    uint32_t timeout_ns = 0;
    uint64_t abs_timeout = NanoTime() + MsToNs(timeout_ms);
    for (;;) {
      timed_out = condition_.TimedWait(self, timeout_ms, timeout_ns);
      if (timed_out || count_ == 0) return timed_out;
      // Compute time remaining on timeout.
      uint64_t now = NanoTime();
      int64_t time_left = abs_timeout - now;
      if (time_left <= 0) return true;
      timeout_ns = time_left % (1000*1000);
      timeout_ms = time_left / (1000*1000);
    }
  }
  return timed_out;
}

void Barrier::SetCountLocked(Thread* self, int count) {
  count_ = count;
  if (count == 0) {
    condition_.Broadcast(self);
  }
}

Barrier::~Barrier() {
  if (gAborting == 0) {
    // Only check when not aborting.
    CHECK_EQ(count_, 0) << "Attempted to destroy barrier with non zero count";
  } else {
    if (count_ != 0) {
      LOG(WARNING) << "Attempted to destroy barrier with non zero count " << count_;
    }
  }
}

}  // namespace art
