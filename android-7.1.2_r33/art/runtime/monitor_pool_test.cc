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

#include "monitor_pool.h"

#include "common_runtime_test.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

class MonitorPoolTest : public CommonRuntimeTest {};

class RandGen {
 public:
  explicit RandGen(uint32_t seed) : val_(seed) {}

  uint32_t next() {
    val_ = val_ * 48271 % 2147483647 + 13;
    return val_;
  }

  uint32_t val_;
};

static void VerifyMonitor(Monitor* mon, Thread* self) {
  // Check whether the monitor id is correct.
  EXPECT_EQ(MonitorPool::MonitorIdFromMonitor(mon), mon->GetMonitorId());
  // Check whether the monitor id agrees with the compuation.
  EXPECT_EQ(MonitorPool::ComputeMonitorId(mon, self), mon->GetMonitorId());
  // Check whether we can use the monitor ID to get the monitor.
  EXPECT_EQ(mon, MonitorPool::MonitorFromMonitorId(mon->GetMonitorId()));
}

TEST_F(MonitorPoolTest, MonitorPoolTest) {
  std::vector<Monitor*> monitors;
  RandGen r(0x1234);

  // 1) Create and release monitors without increasing the storage.

  // Number of max alive monitors before resize.
  // Note: for correct testing, make sure this is corresponding to monitor-pool's initial size.
  const size_t kMaxUsage = 28;

  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  // Allocate and release monitors.
  for (size_t i = 0; i < 1000 ; i++) {
    bool alloc;
    if (monitors.size() == 0) {
      alloc = true;
    } else if (monitors.size() == kMaxUsage) {
      alloc = false;
    } else {
      // Random decision.
      alloc = r.next() % 2 == 0;
    }

    if (alloc) {
      Monitor* mon = MonitorPool::CreateMonitor(self, self, nullptr, static_cast<int32_t>(i));
      monitors.push_back(mon);

      VerifyMonitor(mon, self);
    } else {
      // Release a random monitor.
      size_t index = r.next() % monitors.size();
      Monitor* mon = monitors[index];
      monitors.erase(monitors.begin() + index);

      // Recheck the monitor.
      VerifyMonitor(mon, self);

      MonitorPool::ReleaseMonitor(self, mon);
    }
  }

  // Loop some time.

  for (size_t i = 0; i < 10; ++i) {
    // 2.1) Create enough monitors to require new chunks.
    size_t target_size = monitors.size() + 2*kMaxUsage;
    while (monitors.size() < target_size) {
      Monitor* mon = MonitorPool::CreateMonitor(self, self, nullptr,
                                                static_cast<int32_t>(-monitors.size()));
      monitors.push_back(mon);

      VerifyMonitor(mon, self);
    }

    // 2.2) Verify all monitors.
    for (Monitor* mon : monitors) {
      VerifyMonitor(mon, self);
    }

    // 2.3) Release a number of monitors randomly.
    for (size_t j = 0; j < kMaxUsage; j++) {
      // Release a random monitor.
      size_t index = r.next() % monitors.size();
      Monitor* mon = monitors[index];
      monitors.erase(monitors.begin() + index);

      MonitorPool::ReleaseMonitor(self, mon);
    }
  }

  // Check and release all remaining monitors.
  for (Monitor* mon : monitors) {
    VerifyMonitor(mon, self);
    MonitorPool::ReleaseMonitor(self, mon);
  }
}

}  // namespace art
