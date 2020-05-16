/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "memory_region.h"

#include "gtest/gtest.h"

namespace art {

TEST(MemoryRegion, LoadUnaligned) {
  const size_t n = 8;
  uint8_t data[n] = { 0, 1, 2, 3, 4, 5, 6, 7 };
  MemoryRegion region(&data, n);

  ASSERT_EQ(0, region.LoadUnaligned<char>(0));
  ASSERT_EQ(1u
            + (2u << kBitsPerByte)
            + (3u << 2 * kBitsPerByte)
            + (4u << 3 * kBitsPerByte),
            region.LoadUnaligned<uint32_t>(1));
  ASSERT_EQ(5 + (6 << kBitsPerByte), region.LoadUnaligned<int16_t>(5));
  ASSERT_EQ(7u, region.LoadUnaligned<unsigned char>(7));
}

TEST(MemoryRegion, StoreUnaligned) {
  const size_t n = 8;
  uint8_t data[n] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  MemoryRegion region(&data, n);

  region.StoreUnaligned<unsigned char>(0u, 7);
  region.StoreUnaligned<int16_t>(1, 6 + (5 << kBitsPerByte));
  region.StoreUnaligned<uint32_t>(3,
                                  4u
                                  + (3u << kBitsPerByte)
                                  + (2u << 2 * kBitsPerByte)
                                  + (1u << 3 * kBitsPerByte));
  region.StoreUnaligned<char>(7, 0);

  uint8_t expected[n] = { 7, 6, 5, 4, 3, 2, 1, 0 };
  for (size_t i = 0; i < n; ++i) {
    ASSERT_EQ(expected[i], data[i]);
  }
}

}  // namespace art
