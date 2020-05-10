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

#include <memory>

#include "allocator.h"
#include "bit_vector-inl.h"
#include "gtest/gtest.h"

namespace art {

TEST(BitVector, Test) {
  const size_t kBits = 32;

  BitVector bv(kBits, false, Allocator::GetMallocAllocator());
  EXPECT_EQ(1U, bv.GetStorageSize());
  EXPECT_EQ(sizeof(uint32_t), bv.GetSizeOf());
  EXPECT_FALSE(bv.IsExpandable());

  EXPECT_EQ(0U, bv.NumSetBits());
  EXPECT_EQ(0U, bv.NumSetBits(1));
  EXPECT_EQ(0U, bv.NumSetBits(kBits));
  for (size_t i = 0; i < kBits; i++) {
    EXPECT_FALSE(bv.IsBitSet(i));
  }
  EXPECT_EQ(0U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0U, *bv.GetRawStorage());

  EXPECT_TRUE(bv.Indexes().begin().Done());
  EXPECT_TRUE(bv.Indexes().begin() == bv.Indexes().end());

  bv.SetBit(0);
  bv.SetBit(kBits - 1);
  EXPECT_EQ(2U, bv.NumSetBits());
  EXPECT_EQ(1U, bv.NumSetBits(1));
  EXPECT_EQ(2U, bv.NumSetBits(kBits));
  EXPECT_TRUE(bv.IsBitSet(0));
  for (size_t i = 1; i < kBits - 1; i++) {
    EXPECT_FALSE(bv.IsBitSet(i));
  }
  EXPECT_TRUE(bv.IsBitSet(kBits - 1));
  EXPECT_EQ(0x80000001U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x80000001U, *bv.GetRawStorage());

  BitVector::IndexIterator iterator = bv.Indexes().begin();
  EXPECT_TRUE(iterator != bv.Indexes().end());
  EXPECT_EQ(0u, *iterator);
  ++iterator;
  EXPECT_TRUE(iterator != bv.Indexes().end());
  EXPECT_EQ(kBits - 1u, *iterator);
  ++iterator;
  EXPECT_TRUE(iterator == bv.Indexes().end());
}

TEST(BitVector, NoopAllocator) {
  const uint32_t kWords = 2;

  uint32_t bits[kWords];
  memset(bits, 0, sizeof(bits));

  BitVector bv(false, Allocator::GetNoopAllocator(), kWords, bits);
  EXPECT_EQ(kWords, bv.GetStorageSize());
  EXPECT_EQ(kWords * sizeof(uint32_t), bv.GetSizeOf());
  EXPECT_EQ(bits, bv.GetRawStorage());
  EXPECT_EQ(0U, bv.NumSetBits());

  bv.SetBit(8);
  EXPECT_EQ(1U, bv.NumSetBits());
  EXPECT_EQ(0x00000100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00000000U, bv.GetRawStorageWord(1));
  EXPECT_EQ(1U, bv.NumSetBits());

  bv.SetBit(16);
  EXPECT_EQ(2U, bv.NumSetBits());
  EXPECT_EQ(0x00010100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00000000U, bv.GetRawStorageWord(1));
  EXPECT_EQ(2U, bv.NumSetBits());

  bv.SetBit(32);
  EXPECT_EQ(3U, bv.NumSetBits());
  EXPECT_EQ(0x00010100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00000001U, bv.GetRawStorageWord(1));
  EXPECT_EQ(3U, bv.NumSetBits());

  bv.SetBit(48);
  EXPECT_EQ(4U, bv.NumSetBits());
  EXPECT_EQ(0x00010100U, bv.GetRawStorageWord(0));
  EXPECT_EQ(0x00010001U, bv.GetRawStorageWord(1));
  EXPECT_EQ(4U, bv.NumSetBits());

  EXPECT_EQ(0U, bv.NumSetBits(1));

  EXPECT_EQ(0U, bv.NumSetBits(8));
  EXPECT_EQ(1U, bv.NumSetBits(9));
  EXPECT_EQ(1U, bv.NumSetBits(10));

  EXPECT_EQ(1U, bv.NumSetBits(16));
  EXPECT_EQ(2U, bv.NumSetBits(17));
  EXPECT_EQ(2U, bv.NumSetBits(18));

  EXPECT_EQ(2U, bv.NumSetBits(32));
  EXPECT_EQ(3U, bv.NumSetBits(33));
  EXPECT_EQ(3U, bv.NumSetBits(34));

  EXPECT_EQ(3U, bv.NumSetBits(48));
  EXPECT_EQ(4U, bv.NumSetBits(49));
  EXPECT_EQ(4U, bv.NumSetBits(50));

  EXPECT_EQ(4U, bv.NumSetBits(64));
}

TEST(BitVector, SetInitialBits) {
  const uint32_t kWords = 2;

  uint32_t bits[kWords];
  memset(bits, 0, sizeof(bits));

  BitVector bv(false, Allocator::GetNoopAllocator(), kWords, bits);
  bv.SetInitialBits(0u);
  EXPECT_EQ(0u, bv.NumSetBits());
  bv.SetInitialBits(1u);
  EXPECT_EQ(1u, bv.NumSetBits());
  bv.SetInitialBits(32u);
  EXPECT_EQ(32u, bv.NumSetBits());
  bv.SetInitialBits(63u);
  EXPECT_EQ(63u, bv.NumSetBits());
  bv.SetInitialBits(64u);
  EXPECT_EQ(64u, bv.NumSetBits());
}

TEST(BitVector, UnionIfNotIn) {
  {
    BitVector first(2, true, Allocator::GetMallocAllocator());
    BitVector second(5, true, Allocator::GetMallocAllocator());
    BitVector third(5, true, Allocator::GetMallocAllocator());

    second.SetBit(64);
    third.SetBit(64);
    bool changed = first.UnionIfNotIn(&second, &third);
    EXPECT_EQ(0u, first.NumSetBits());
    EXPECT_FALSE(changed);
  }

  {
    BitVector first(2, true, Allocator::GetMallocAllocator());
    BitVector second(5, true, Allocator::GetMallocAllocator());
    BitVector third(5, true, Allocator::GetMallocAllocator());

    second.SetBit(64);
    bool changed = first.UnionIfNotIn(&second, &third);
    EXPECT_EQ(1u, first.NumSetBits());
    EXPECT_TRUE(changed);
    EXPECT_TRUE(first.IsBitSet(64));
  }
}

TEST(BitVector, Subset) {
  {
    BitVector first(2, true, Allocator::GetMallocAllocator());
    BitVector second(5, true, Allocator::GetMallocAllocator());

    EXPECT_TRUE(first.IsSubsetOf(&second));
    second.SetBit(4);
    EXPECT_TRUE(first.IsSubsetOf(&second));
  }

  {
    BitVector first(5, true, Allocator::GetMallocAllocator());
    BitVector second(5, true, Allocator::GetMallocAllocator());

    first.SetBit(5);
    EXPECT_FALSE(first.IsSubsetOf(&second));
    second.SetBit(4);
    EXPECT_FALSE(first.IsSubsetOf(&second));
  }

  {
    BitVector first(5, true, Allocator::GetMallocAllocator());
    BitVector second(5, true, Allocator::GetMallocAllocator());

    first.SetBit(16);
    first.SetBit(32);
    first.SetBit(48);
    second.SetBit(16);
    second.SetBit(32);
    second.SetBit(48);

    EXPECT_TRUE(first.IsSubsetOf(&second));
    second.SetBit(8);
    EXPECT_TRUE(first.IsSubsetOf(&second));
    second.SetBit(40);
    EXPECT_TRUE(first.IsSubsetOf(&second));
    second.SetBit(52);
    EXPECT_TRUE(first.IsSubsetOf(&second));

    first.SetBit(9);
    EXPECT_FALSE(first.IsSubsetOf(&second));
  }
}

TEST(BitVector, CopyTo) {
  {
    // Test copying an empty BitVector. Padding should fill `buf` with zeroes.
    BitVector bv(0, true, Allocator::GetMallocAllocator());
    uint32_t buf;

    bv.CopyTo(&buf, sizeof(buf));
    EXPECT_EQ(0u, bv.GetSizeOf());
    EXPECT_EQ(0u, buf);
  }

  {
    // Test copying when `bv.storage_` and `buf` are of equal lengths.
    BitVector bv(0, true, Allocator::GetMallocAllocator());
    uint32_t buf;

    bv.SetBit(0);
    bv.SetBit(17);
    bv.SetBit(26);
    EXPECT_EQ(sizeof(buf), bv.GetSizeOf());

    bv.CopyTo(&buf, sizeof(buf));
    EXPECT_EQ(0x04020001u, buf);
  }

  {
    // Test copying when the `bv.storage_` is longer than `buf`. As long as
    // `buf` is long enough to hold all set bits, copying should succeed.
    BitVector bv(0, true, Allocator::GetMallocAllocator());
    uint8_t buf[5];

    bv.SetBit(18);
    bv.SetBit(39);
    EXPECT_LT(sizeof(buf), bv.GetSizeOf());

    bv.CopyTo(buf, sizeof(buf));
    EXPECT_EQ(0x00u, buf[0]);
    EXPECT_EQ(0x00u, buf[1]);
    EXPECT_EQ(0x04u, buf[2]);
    EXPECT_EQ(0x00u, buf[3]);
    EXPECT_EQ(0x80u, buf[4]);
  }

  {
    // Test zero padding when `bv.storage_` is shorter than `buf`.
    BitVector bv(0, true, Allocator::GetMallocAllocator());
    uint32_t buf[2];

    bv.SetBit(18);
    bv.SetBit(31);
    EXPECT_GT(sizeof(buf), bv.GetSizeOf());

    bv.CopyTo(buf, sizeof(buf));
    EXPECT_EQ(0x80040000U, buf[0]);
    EXPECT_EQ(0x00000000U, buf[1]);
  }
}

}  // namespace art
