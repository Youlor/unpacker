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

#include "base/time_utils.h"
#include "space_test.h"
#include "large_object_space.h"

namespace art {
namespace gc {
namespace space {

class LargeObjectSpaceTest : public SpaceTest<CommonRuntimeTest> {
 public:
  void LargeObjectTest();

  static constexpr size_t kNumThreads = 10;
  static constexpr size_t kNumIterations = 1000;
  void RaceTest();
};


void LargeObjectSpaceTest::LargeObjectTest() {
  size_t rand_seed = 0;
  Thread* const self = Thread::Current();
  for (size_t i = 0; i < 2; ++i) {
    LargeObjectSpace* los = nullptr;
    if (i == 0) {
      los = space::LargeObjectMapSpace::Create("large object space");
    } else {
      los = space::FreeListSpace::Create("large object space", nullptr, 128 * MB);
    }

    static const size_t num_allocations = 64;
    static const size_t max_allocation_size = 0x100000;
    std::vector<std::pair<mirror::Object*, size_t>> requests;

    for (size_t phase = 0; phase < 2; ++phase) {
      while (requests.size() < num_allocations) {
        size_t request_size = test_rand(&rand_seed) % max_allocation_size;
        size_t allocation_size = 0;
        size_t bytes_tl_bulk_allocated;
        mirror::Object* obj = los->Alloc(self, request_size, &allocation_size, nullptr,
                                         &bytes_tl_bulk_allocated);
        ASSERT_TRUE(obj != nullptr);
        ASSERT_EQ(allocation_size, los->AllocationSize(obj, nullptr));
        ASSERT_GE(allocation_size, request_size);
        ASSERT_EQ(allocation_size, bytes_tl_bulk_allocated);
        // Fill in our magic value.
        uint8_t magic = (request_size & 0xFF) | 1;
        memset(obj, magic, request_size);
        requests.push_back(std::make_pair(obj, request_size));
      }

      // "Randomly" shuffle the requests.
      for (size_t k = 0; k < 10; ++k) {
        for (size_t j = 0; j < requests.size(); ++j) {
          std::swap(requests[j], requests[test_rand(&rand_seed) % requests.size()]);
        }
      }

      // Check the zygote flag for the first phase.
      if (phase == 0) {
        for (const auto& pair : requests) {
          mirror::Object* obj = pair.first;
          ASSERT_FALSE(los->IsZygoteLargeObject(self, obj));
        }
        los->SetAllLargeObjectsAsZygoteObjects(self);
        for (const auto& pair : requests) {
          mirror::Object* obj = pair.first;
          ASSERT_TRUE(los->IsZygoteLargeObject(self, obj));
        }
      }

      // Free 1 / 2 the allocations the first phase, and all the second phase.
      size_t limit = phase == 0 ? requests.size() / 2 : 0;
      while (requests.size() > limit) {
        mirror::Object* obj = requests.back().first;
        size_t request_size = requests.back().second;
        requests.pop_back();
        uint8_t magic = (request_size & 0xFF) | 1;
        for (size_t k = 0; k < request_size; ++k) {
          ASSERT_EQ(reinterpret_cast<const uint8_t*>(obj)[k], magic);
        }
        ASSERT_GE(los->Free(Thread::Current(), obj), request_size);
      }
    }
    // Test that dump doesn't crash.
    los->Dump(LOG(INFO));

    size_t bytes_allocated = 0, bytes_tl_bulk_allocated;
    // Checks that the coalescing works.
    mirror::Object* obj = los->Alloc(self, 100 * MB, &bytes_allocated, nullptr,
                                     &bytes_tl_bulk_allocated);
    EXPECT_TRUE(obj != nullptr);
    los->Free(Thread::Current(), obj);

    EXPECT_EQ(0U, los->GetBytesAllocated());
    EXPECT_EQ(0U, los->GetObjectsAllocated());
    delete los;
  }
}

class AllocRaceTask : public Task {
 public:
  AllocRaceTask(size_t id, size_t iterations, size_t size, LargeObjectSpace* los) :
    id_(id), iterations_(iterations), size_(size), los_(los) {}

  void Run(Thread* self) {
    for (size_t i = 0; i < iterations_ ; ++i) {
      size_t alloc_size, bytes_tl_bulk_allocated;
      mirror::Object* ptr = los_->Alloc(self, size_, &alloc_size, nullptr,
                                        &bytes_tl_bulk_allocated);

      NanoSleep((id_ + 3) * 1000);  // (3+id) mu s

      los_->Free(self, ptr);
    }
  }

  virtual void Finalize() {
    delete this;
  }

 private:
  size_t id_;
  size_t iterations_;
  size_t size_;
  LargeObjectSpace* los_;
};

void LargeObjectSpaceTest::RaceTest() {
  for (size_t los_type = 0; los_type < 2; ++los_type) {
    LargeObjectSpace* los = nullptr;
    if (los_type == 0) {
      los = space::LargeObjectMapSpace::Create("large object space");
    } else {
      los = space::FreeListSpace::Create("large object space", nullptr, 128 * MB);
    }

    Thread* self = Thread::Current();
    ThreadPool thread_pool("Large object space test thread pool", kNumThreads);
    for (size_t i = 0; i < kNumThreads; ++i) {
      thread_pool.AddTask(self, new AllocRaceTask(i, kNumIterations, 16 * KB, los));
    }

    thread_pool.StartWorkers(self);

    thread_pool.Wait(self, true, false);

    delete los;
  }
}

TEST_F(LargeObjectSpaceTest, LargeObjectTest) {
  LargeObjectTest();
}

TEST_F(LargeObjectSpaceTest, RaceTest) {
  RaceTest();
}

}  // namespace space
}  // namespace gc
}  // namespace art
