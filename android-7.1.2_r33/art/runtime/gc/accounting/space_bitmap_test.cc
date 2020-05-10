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

#include "space_bitmap.h"

#include <stdint.h>
#include <memory>

#include "common_runtime_test.h"
#include "globals.h"
#include "space_bitmap-inl.h"

namespace art {
namespace gc {
namespace accounting {

class SpaceBitmapTest : public CommonRuntimeTest {};

TEST_F(SpaceBitmapTest, Init) {
  uint8_t* heap_begin = reinterpret_cast<uint8_t*>(0x10000000);
  size_t heap_capacity = 16 * MB;
  std::unique_ptr<ContinuousSpaceBitmap> space_bitmap(
      ContinuousSpaceBitmap::Create("test bitmap", heap_begin, heap_capacity));
  EXPECT_TRUE(space_bitmap.get() != nullptr);
}

class BitmapVerify {
 public:
  BitmapVerify(ContinuousSpaceBitmap* bitmap, const mirror::Object* begin,
               const mirror::Object* end)
    : bitmap_(bitmap),
      begin_(begin),
      end_(end) {}

  void operator()(const mirror::Object* obj) {
    EXPECT_TRUE(obj >= begin_);
    EXPECT_TRUE(obj <= end_);
    EXPECT_EQ(bitmap_->Test(obj), ((reinterpret_cast<uintptr_t>(obj) & 0xF) != 0));
  }

  ContinuousSpaceBitmap* const bitmap_;
  const mirror::Object* begin_;
  const mirror::Object* end_;
};

TEST_F(SpaceBitmapTest, ScanRange) {
  uint8_t* heap_begin = reinterpret_cast<uint8_t*>(0x10000000);
  size_t heap_capacity = 16 * MB;

  std::unique_ptr<ContinuousSpaceBitmap> space_bitmap(
      ContinuousSpaceBitmap::Create("test bitmap", heap_begin, heap_capacity));
  EXPECT_TRUE(space_bitmap.get() != nullptr);

  // Set all the odd bits in the first BitsPerIntPtrT * 3 to one.
  for (size_t j = 0; j < kBitsPerIntPtrT * 3; ++j) {
    const mirror::Object* obj =
        reinterpret_cast<mirror::Object*>(heap_begin + j * kObjectAlignment);
    if (reinterpret_cast<uintptr_t>(obj) & 0xF) {
      space_bitmap->Set(obj);
    }
  }
  // Try every possible starting bit in the first word. Then for each starting bit, try each
  // possible length up to a maximum of kBitsPerWord * 2 - 1 bits.
  // This handles all the cases, having runs which start and end on the same word, and different
  // words.
  for (size_t i = 0; i < static_cast<size_t>(kBitsPerIntPtrT); ++i) {
    mirror::Object* start =
        reinterpret_cast<mirror::Object*>(heap_begin + i * kObjectAlignment);
    for (size_t j = 0; j < static_cast<size_t>(kBitsPerIntPtrT * 2); ++j) {
      mirror::Object* end =
          reinterpret_cast<mirror::Object*>(heap_begin + (i + j) * kObjectAlignment);
      BitmapVerify(space_bitmap.get(), start, end);
    }
  }
}

class SimpleCounter {
 public:
  explicit SimpleCounter(size_t* counter) : count_(counter) {}

  void operator()(mirror::Object* obj ATTRIBUTE_UNUSED) const {
    (*count_)++;
  }

  size_t* const count_;
};

class RandGen {
 public:
  explicit RandGen(uint32_t seed) : val_(seed) {}

  uint32_t next() {
    val_ = val_ * 48271 % 2147483647;
    return val_;
  }

  uint32_t val_;
};

template <size_t kAlignment>
void RunTest() NO_THREAD_SAFETY_ANALYSIS {
  uint8_t* heap_begin = reinterpret_cast<uint8_t*>(0x10000000);
  size_t heap_capacity = 16 * MB;

  // Seed with 0x1234 for reproducability.
  RandGen r(0x1234);


  for (int i = 0; i < 5 ; ++i) {
    std::unique_ptr<ContinuousSpaceBitmap> space_bitmap(
        ContinuousSpaceBitmap::Create("test bitmap", heap_begin, heap_capacity));

    for (int j = 0; j < 10000; ++j) {
      size_t offset = RoundDown(r.next() % heap_capacity, kAlignment);
      bool set = r.next() % 2 == 1;

      if (set) {
        space_bitmap->Set(reinterpret_cast<mirror::Object*>(heap_begin + offset));
      } else {
        space_bitmap->Clear(reinterpret_cast<mirror::Object*>(heap_begin + offset));
      }
    }

    for (int j = 0; j < 50; ++j) {
      size_t count = 0;
      SimpleCounter c(&count);

      size_t offset = RoundDown(r.next() % heap_capacity, kAlignment);
      size_t remain = heap_capacity - offset;
      size_t end = offset + RoundDown(r.next() % (remain + 1), kAlignment);

      space_bitmap->VisitMarkedRange(reinterpret_cast<uintptr_t>(heap_begin) + offset,
                                     reinterpret_cast<uintptr_t>(heap_begin) + end, c);

      size_t manual = 0;
      for (uintptr_t k = offset; k < end; k += kAlignment) {
        if (space_bitmap->Test(reinterpret_cast<mirror::Object*>(heap_begin + k))) {
          manual++;
        }
      }

      EXPECT_EQ(count, manual);
    }
  }
}

TEST_F(SpaceBitmapTest, VisitorObjectAlignment) {
  RunTest<kObjectAlignment>();
}

TEST_F(SpaceBitmapTest, VisitorPageAlignment) {
  RunTest<kPageSize>();
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
