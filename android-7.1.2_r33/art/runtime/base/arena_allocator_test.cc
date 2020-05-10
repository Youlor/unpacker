/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "base/arena_allocator.h"
#include "base/arena_bit_vector.h"
#include "gtest/gtest.h"

namespace art {

class ArenaAllocatorTest : public testing::Test {
 protected:
  size_t NumberOfArenas(ArenaAllocator* arena) {
    size_t result = 0u;
    for (Arena* a = arena->arena_head_; a != nullptr; a = a->next_) {
      ++result;
    }
    return result;
  }
};

TEST_F(ArenaAllocatorTest, Test) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  ArenaBitVector bv(&arena, 10, true);
  bv.SetBit(5);
  EXPECT_EQ(1U, bv.GetStorageSize());
  bv.SetBit(35);
  EXPECT_EQ(2U, bv.GetStorageSize());
}

TEST_F(ArenaAllocatorTest, MakeDefined) {
  // Regression test to make sure we mark the allocated area defined.
  ArenaPool pool;
  static constexpr size_t kSmallArraySize = 10;
  static constexpr size_t kLargeArraySize = 50;
  uint32_t* small_array;
  {
    // Allocate a small array from an arena and release it.
    ArenaAllocator arena(&pool);
    small_array = arena.AllocArray<uint32_t>(kSmallArraySize);
    ASSERT_EQ(0u, small_array[kSmallArraySize - 1u]);
  }
  {
    // Reuse the previous arena and allocate more than previous allocation including red zone.
    ArenaAllocator arena(&pool);
    uint32_t* large_array = arena.AllocArray<uint32_t>(kLargeArraySize);
    ASSERT_EQ(0u, large_array[kLargeArraySize - 1u]);
    // Verify that the allocation was made on the same arena.
    ASSERT_EQ(small_array, large_array);
  }
}

TEST_F(ArenaAllocatorTest, LargeAllocations) {
  {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    // Note: Leaving some space for memory tool red zones.
    void* alloc1 = arena.Alloc(Arena::kDefaultSize * 5 / 8);
    void* alloc2 = arena.Alloc(Arena::kDefaultSize * 2 / 8);
    ASSERT_NE(alloc1, alloc2);
    ASSERT_EQ(1u, NumberOfArenas(&arena));
  }
  {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    void* alloc1 = arena.Alloc(Arena::kDefaultSize * 13 / 16);
    void* alloc2 = arena.Alloc(Arena::kDefaultSize * 11 / 16);
    ASSERT_NE(alloc1, alloc2);
    ASSERT_EQ(2u, NumberOfArenas(&arena));
    void* alloc3 = arena.Alloc(Arena::kDefaultSize * 7 / 16);
    ASSERT_NE(alloc1, alloc3);
    ASSERT_NE(alloc2, alloc3);
    ASSERT_EQ(3u, NumberOfArenas(&arena));
  }
  {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    void* alloc1 = arena.Alloc(Arena::kDefaultSize * 13 / 16);
    void* alloc2 = arena.Alloc(Arena::kDefaultSize * 9 / 16);
    ASSERT_NE(alloc1, alloc2);
    ASSERT_EQ(2u, NumberOfArenas(&arena));
    // Note: Leaving some space for memory tool red zones.
    void* alloc3 = arena.Alloc(Arena::kDefaultSize * 5 / 16);
    ASSERT_NE(alloc1, alloc3);
    ASSERT_NE(alloc2, alloc3);
    ASSERT_EQ(2u, NumberOfArenas(&arena));
  }
  {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    void* alloc1 = arena.Alloc(Arena::kDefaultSize * 9 / 16);
    void* alloc2 = arena.Alloc(Arena::kDefaultSize * 13 / 16);
    ASSERT_NE(alloc1, alloc2);
    ASSERT_EQ(2u, NumberOfArenas(&arena));
    // Note: Leaving some space for memory tool red zones.
    void* alloc3 = arena.Alloc(Arena::kDefaultSize * 5 / 16);
    ASSERT_NE(alloc1, alloc3);
    ASSERT_NE(alloc2, alloc3);
    ASSERT_EQ(2u, NumberOfArenas(&arena));
  }
  {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    // Note: Leaving some space for memory tool red zones.
    for (size_t i = 0; i != 15; ++i) {
      arena.Alloc(Arena::kDefaultSize * 1 / 16);    // Allocate 15 times from the same arena.
      ASSERT_EQ(i + 1u, NumberOfArenas(&arena));
      arena.Alloc(Arena::kDefaultSize * 17 / 16);   // Allocate a separate arena.
      ASSERT_EQ(i + 2u, NumberOfArenas(&arena));
    }
  }
}

}  // namespace art
