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

#include <vector>

#include "bit_utils.h"

#include "gtest/gtest.h"

namespace art {

// NOTE: CLZ(0u) is undefined.
static_assert(31 == CLZ<uint32_t>(1u), "TestCLZ32#1");
static_assert(30 == CLZ<uint32_t>(2u), "TestCLZ32#2");
static_assert(16 == CLZ<uint32_t>(0x00008765u), "TestCLZ32#3");
static_assert(15 == CLZ<uint32_t>(0x00012345u), "TestCLZ32#4");
static_assert(1 == CLZ<uint32_t>(0x43214321u), "TestCLZ32#5");
static_assert(0 == CLZ<uint32_t>(0x87654321u), "TestCLZ32#6");

// NOTE: CLZ(0ull) is undefined.
static_assert(63 == CLZ<uint64_t>(UINT64_C(1)), "TestCLZ64#1");
static_assert(62 == CLZ<uint64_t>(UINT64_C(3)), "TestCLZ64#2");
static_assert(48 == CLZ<uint64_t>(UINT64_C(0x00008765)), "TestCLZ64#3");
static_assert(32 == CLZ<uint64_t>(UINT64_C(0x87654321)), "TestCLZ64#4");
static_assert(31 == CLZ<uint64_t>(UINT64_C(0x123456789)), "TestCLZ64#5");
static_assert(16 == CLZ<uint64_t>(UINT64_C(0x876543211234)), "TestCLZ64#6");
static_assert(1 == CLZ<uint64_t>(UINT64_C(0x4321432187654321)), "TestCLZ64#7");
static_assert(0 == CLZ<uint64_t>(UINT64_C(0x8765432187654321)), "TestCLZ64#8");

// NOTE: CTZ(0u) is undefined.
static_assert(0 == CTZ<uint32_t>(1u), "TestCTZ32#1");
static_assert(1 == CTZ<uint32_t>(2u), "TestCTZ32#2");
static_assert(15 == CTZ<uint32_t>(0x45678000u), "TestCTZ32#3");
static_assert(16 == CTZ<uint32_t>(0x43210000u), "TestCTZ32#4");
static_assert(30 == CTZ<uint32_t>(0xc0000000u), "TestCTZ32#5");
static_assert(31 == CTZ<uint32_t>(0x80000000u), "TestCTZ32#6");

// NOTE: CTZ(0ull) is undefined.
static_assert(0 == CTZ<uint64_t>(UINT64_C(1)), "TestCTZ64#1");
static_assert(1 == CTZ<uint64_t>(UINT64_C(2)), "TestCTZ64#2");
static_assert(16 == CTZ<uint64_t>(UINT64_C(0x43210000)), "TestCTZ64#3");
static_assert(31 == CTZ<uint64_t>(UINT64_C(0x80000000)), "TestCTZ64#4");
static_assert(32 == CTZ<uint64_t>(UINT64_C(0x8765432100000000)), "TestCTZ64#5");
static_assert(48 == CTZ<uint64_t>(UINT64_C(0x4321000000000000)), "TestCTZ64#6");
static_assert(62 == CTZ<uint64_t>(UINT64_C(0x4000000000000000)), "TestCTZ64#7");
static_assert(63 == CTZ<uint64_t>(UINT64_C(0x8000000000000000)), "TestCTZ64#8");

static_assert(0 == POPCOUNT<uint32_t>(0u), "TestPOPCOUNT32#1");
static_assert(1 == POPCOUNT<uint32_t>(8u), "TestPOPCOUNT32#2");
static_assert(15 == POPCOUNT<uint32_t>(0x55555554u), "TestPOPCOUNT32#3");
static_assert(16 == POPCOUNT<uint32_t>(0xaaaaaaaau), "TestPOPCOUNT32#4");
static_assert(31 == POPCOUNT<uint32_t>(0xfffffffeu), "TestPOPCOUNT32#5");
static_assert(32 == POPCOUNT<uint32_t>(0xffffffffu), "TestPOPCOUNT32#6");

static_assert(0 == POPCOUNT<uint64_t>(UINT64_C(0)), "TestPOPCOUNT64#1");
static_assert(1 == POPCOUNT<uint64_t>(UINT64_C(0x40000)), "TestPOPCOUNT64#2");
static_assert(16 == POPCOUNT<uint64_t>(UINT64_C(0x1414141482828282)), "TestPOPCOUNT64#3");
static_assert(31 == POPCOUNT<uint64_t>(UINT64_C(0x0000ffff00007fff)), "TestPOPCOUNT64#4");
static_assert(32 == POPCOUNT<uint64_t>(UINT64_C(0x5555555555555555)), "TestPOPCOUNT64#5");
static_assert(48 == POPCOUNT<uint64_t>(UINT64_C(0x7777bbbbddddeeee)), "TestPOPCOUNT64#6");
static_assert(63 == POPCOUNT<uint64_t>(UINT64_C(0x7fffffffffffffff)), "TestPOPCOUNT64#7");
static_assert(64 == POPCOUNT<uint64_t>(UINT64_C(0xffffffffffffffff)), "TestPOPCOUNT64#8");

static_assert(-1 == MostSignificantBit<uint32_t>(0u), "TestMSB32#1");
static_assert(0 == MostSignificantBit<uint32_t>(1u), "TestMSB32#2");
static_assert(31 == MostSignificantBit<uint32_t>(~static_cast<uint32_t>(0u)), "TestMSB32#3");
static_assert(2 == MostSignificantBit<uint32_t>(0b110), "TestMSB32#4");
static_assert(2 == MostSignificantBit<uint32_t>(0b100), "TestMSB32#5");

static_assert(-1 == MostSignificantBit<uint64_t>(UINT64_C(0)), "TestMSB64#1");
static_assert(0 == MostSignificantBit<uint64_t>(UINT64_C(1)), "TestMSB64#2");
static_assert(63 == MostSignificantBit<uint64_t>(~UINT64_C(0)), "TestMSB64#3");
static_assert(34 == MostSignificantBit<uint64_t>(UINT64_C(0x700000000)), "TestMSB64#4");
static_assert(34 == MostSignificantBit<uint64_t>(UINT64_C(0x777777777)), "TestMSB64#5");

static_assert(-1 == LeastSignificantBit<uint32_t>(0u), "TestLSB32#1");
static_assert(0 == LeastSignificantBit<uint32_t>(1u), "TestLSB32#1");
static_assert(0 == LeastSignificantBit<uint32_t>(~static_cast<uint32_t>(0u)), "TestLSB32#1");
static_assert(1 == LeastSignificantBit<uint32_t>(0b110), "TestLSB32#1");
static_assert(2 == LeastSignificantBit<uint32_t>(0b100), "TestLSB32#1");

static_assert(-1 == LeastSignificantBit<uint64_t>(UINT64_C(0)), "TestLSB64#1");
static_assert(0 == LeastSignificantBit<uint64_t>(UINT64_C(1)), "TestLSB64#2");
static_assert(0 == LeastSignificantBit<uint64_t>(~UINT64_C(0)), "TestLSB64#3");
static_assert(12 == LeastSignificantBit<uint64_t>(UINT64_C(0x5000)), "TestLSB64#4");
static_assert(48 == LeastSignificantBit<uint64_t>(UINT64_C(0x5555000000000000)), "TestLSB64#5");

static_assert(0u == MinimumBitsToStore<uint32_t>(0u), "TestMinBits2Store32#1");
static_assert(1u == MinimumBitsToStore<uint32_t>(1u), "TestMinBits2Store32#2");
static_assert(2u == MinimumBitsToStore<uint32_t>(0b10u), "TestMinBits2Store32#3");
static_assert(2u == MinimumBitsToStore<uint32_t>(0b11u), "TestMinBits2Store32#4");
static_assert(3u == MinimumBitsToStore<uint32_t>(0b100u), "TestMinBits2Store32#5");
static_assert(3u == MinimumBitsToStore<uint32_t>(0b110u), "TestMinBits2Store32#6");
static_assert(3u == MinimumBitsToStore<uint32_t>(0b101u), "TestMinBits2Store32#7");
static_assert(8u == MinimumBitsToStore<uint32_t>(0xFFu), "TestMinBits2Store32#8");
static_assert(32u == MinimumBitsToStore<uint32_t>(~static_cast<uint32_t>(0u)),
              "TestMinBits2Store32#9");

static_assert(0u == MinimumBitsToStore<uint64_t>(UINT64_C(0)), "TestMinBits2Store64#1");
static_assert(1u == MinimumBitsToStore<uint64_t>(UINT64_C(1)), "TestMinBits2Store64#2");
static_assert(2u == MinimumBitsToStore<uint64_t>(UINT64_C(0b10)), "TestMinBits2Store64#3");
static_assert(2u == MinimumBitsToStore<uint64_t>(UINT64_C(0b11)), "TestMinBits2Store64#4");
static_assert(3u == MinimumBitsToStore<uint64_t>(UINT64_C(0b100)), "TestMinBits2Store64#5");
static_assert(3u == MinimumBitsToStore<uint64_t>(UINT64_C(0b110)), "TestMinBits2Store64#6");
static_assert(3u == MinimumBitsToStore<uint64_t>(UINT64_C(0b101)), "TestMinBits2Store64#7");
static_assert(8u == MinimumBitsToStore<uint64_t>(UINT64_C(0xFF)), "TestMinBits2Store64#8");
static_assert(32u == MinimumBitsToStore<uint64_t>(UINT64_C(0xFFFFFFFF)), "TestMinBits2Store64#9");
static_assert(33u == MinimumBitsToStore<uint64_t>(UINT64_C(0x1FFFFFFFF)), "TestMinBits2Store64#10");
static_assert(64u == MinimumBitsToStore<uint64_t>(~UINT64_C(0)), "TestMinBits2Store64#11");

static_assert(0 == RoundUpToPowerOfTwo<uint32_t>(0u), "TestRoundUpPowerOfTwo32#1");
static_assert(1 == RoundUpToPowerOfTwo<uint32_t>(1u), "TestRoundUpPowerOfTwo32#2");
static_assert(2 == RoundUpToPowerOfTwo<uint32_t>(2u), "TestRoundUpPowerOfTwo32#3");
static_assert(4 == RoundUpToPowerOfTwo<uint32_t>(3u), "TestRoundUpPowerOfTwo32#4");
static_assert(8 == RoundUpToPowerOfTwo<uint32_t>(7u), "TestRoundUpPowerOfTwo32#5");
static_assert(0x40000u == RoundUpToPowerOfTwo<uint32_t>(0x2aaaau),
              "TestRoundUpPowerOfTwo32#6");
static_assert(0x80000000u == RoundUpToPowerOfTwo<uint32_t>(0x40000001u),
              "TestRoundUpPowerOfTwo32#7");
static_assert(0x80000000u == RoundUpToPowerOfTwo<uint32_t>(0x80000000u),
              "TestRoundUpPowerOfTwo32#8");

static_assert(0 == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(0)), "TestRoundUpPowerOfTwo64#1");
static_assert(1 == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(1)), "TestRoundUpPowerOfTwo64#2");
static_assert(2 == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(2)), "TestRoundUpPowerOfTwo64#3");
static_assert(4 == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(3)), "TestRoundUpPowerOfTwo64#4");
static_assert(8 == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(7)), "TestRoundUpPowerOfTwo64#5");
static_assert(UINT64_C(0x40000) == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(0x2aaaa)),
              "TestRoundUpPowerOfTwo64#6");
static_assert(
    UINT64_C(0x8000000000000000) == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(0x4000000000000001)),
    "TestRoundUpPowerOfTwo64#7");
static_assert(
    UINT64_C(0x8000000000000000) == RoundUpToPowerOfTwo<uint64_t>(UINT64_C(0x8000000000000000)),
    "TestRoundUpPowerOfTwo64#8");

static constexpr int64_t kInt32MinMinus1 =
    static_cast<int64_t>(std::numeric_limits<int32_t>::min()) - 1;
static constexpr int64_t kInt32MaxPlus1 =
    static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
static constexpr int64_t kUint32MaxPlus1 =
    static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1;

TEST(BitUtilsTest, TestIsInt32) {
  EXPECT_FALSE(IsInt<int32_t>(1, -2));
  EXPECT_TRUE(IsInt<int32_t>(1, -1));
  EXPECT_TRUE(IsInt<int32_t>(1, 0));
  EXPECT_FALSE(IsInt<int32_t>(1, 1));
  EXPECT_FALSE(IsInt<int32_t>(4, -9));
  EXPECT_TRUE(IsInt<int32_t>(4, -8));
  EXPECT_TRUE(IsInt<int32_t>(4, 7));
  EXPECT_FALSE(IsInt<int32_t>(4, 8));
  EXPECT_FALSE(IsInt<int32_t>(31, std::numeric_limits<int32_t>::min()));
  EXPECT_FALSE(IsInt<int32_t>(31, std::numeric_limits<int32_t>::max()));
  EXPECT_TRUE(IsInt<int32_t>(32, std::numeric_limits<int32_t>::min()));
  EXPECT_TRUE(IsInt<int32_t>(32, std::numeric_limits<int32_t>::max()));
}

TEST(BitUtilsTest, TestIsInt64) {
  EXPECT_FALSE(IsInt<int64_t>(1, -2));
  EXPECT_TRUE(IsInt<int64_t>(1, -1));
  EXPECT_TRUE(IsInt<int64_t>(1, 0));
  EXPECT_FALSE(IsInt<int64_t>(1, 1));
  EXPECT_FALSE(IsInt<int64_t>(4, -9));
  EXPECT_TRUE(IsInt<int64_t>(4, -8));
  EXPECT_TRUE(IsInt<int64_t>(4, 7));
  EXPECT_FALSE(IsInt<int64_t>(4, 8));
  EXPECT_FALSE(IsInt<int64_t>(31, std::numeric_limits<int32_t>::min()));
  EXPECT_FALSE(IsInt<int64_t>(31, std::numeric_limits<int32_t>::max()));
  EXPECT_TRUE(IsInt<int64_t>(32, std::numeric_limits<int32_t>::min()));
  EXPECT_TRUE(IsInt<int64_t>(32, std::numeric_limits<int32_t>::max()));
  EXPECT_FALSE(IsInt<int64_t>(32, kInt32MinMinus1));
  EXPECT_FALSE(IsInt<int64_t>(32, kInt32MaxPlus1));
  EXPECT_FALSE(IsInt<int64_t>(63, std::numeric_limits<int64_t>::min()));
  EXPECT_FALSE(IsInt<int64_t>(63, std::numeric_limits<int64_t>::max()));
  EXPECT_TRUE(IsInt<int64_t>(64, std::numeric_limits<int64_t>::min()));
  EXPECT_TRUE(IsInt<int64_t>(64, std::numeric_limits<int64_t>::max()));
}

static_assert(!IsInt<1, int32_t>(-2), "TestIsInt32#1");
static_assert(IsInt<1, int32_t>(-1), "TestIsInt32#2");
static_assert(IsInt<1, int32_t>(0), "TestIsInt32#3");
static_assert(!IsInt<1, int32_t>(1), "TestIsInt32#4");
static_assert(!IsInt<4, int32_t>(-9), "TestIsInt32#5");
static_assert(IsInt<4, int32_t>(-8), "TestIsInt32#6");
static_assert(IsInt<4, int32_t>(7), "TestIsInt32#7");
static_assert(!IsInt<4, int32_t>(8), "TestIsInt32#8");
static_assert(!IsInt<31, int32_t>(std::numeric_limits<int32_t>::min()), "TestIsInt32#9");
static_assert(!IsInt<31, int32_t>(std::numeric_limits<int32_t>::max()), "TestIsInt32#10");
static_assert(IsInt<32, int32_t>(std::numeric_limits<int32_t>::min()), "TestIsInt32#11");
static_assert(IsInt<32, int32_t>(std::numeric_limits<int32_t>::max()), "TestIsInt32#12");

static_assert(!IsInt<1, int64_t>(-2), "TestIsInt64#1");
static_assert(IsInt<1, int64_t>(-1), "TestIsInt64#2");
static_assert(IsInt<1, int64_t>(0), "TestIsInt64#3");
static_assert(!IsInt<1, int64_t>(1), "TestIsInt64#4");
static_assert(!IsInt<4, int64_t>(-9), "TestIsInt64#5");
static_assert(IsInt<4, int64_t>(-8), "TestIsInt64#6");
static_assert(IsInt<4, int64_t>(7), "TestIsInt64#7");
static_assert(!IsInt<4, int64_t>(8), "TestIsInt64#8");
static_assert(!IsInt<31, int64_t>(std::numeric_limits<int32_t>::min()), "TestIsInt64#9");
static_assert(!IsInt<31, int64_t>(std::numeric_limits<int32_t>::max()), "TestIsInt64#10");
static_assert(IsInt<32, int64_t>(std::numeric_limits<int32_t>::min()), "TestIsInt64#11");
static_assert(IsInt<32, int64_t>(std::numeric_limits<int32_t>::max()), "TestIsInt64#12");
static_assert(!IsInt<32, int64_t>(kInt32MinMinus1), "TestIsInt64#13");
static_assert(!IsInt<32, int64_t>(kInt32MaxPlus1), "TestIsInt64#14");
static_assert(!IsInt<63, int64_t>(std::numeric_limits<int64_t>::min()), "TestIsInt64#15");
static_assert(!IsInt<63, int64_t>(std::numeric_limits<int64_t>::max()), "TestIsInt64#16");
static_assert(IsInt<64, int64_t>(std::numeric_limits<int64_t>::min()), "TestIsInt64#17");
static_assert(IsInt<64, int64_t>(std::numeric_limits<int64_t>::max()), "TestIsInt64#18");

static_assert(!IsUint<1, int32_t>(-1), "TestIsUint32#1");
static_assert(IsUint<1, int32_t>(0), "TestIsUint32#2");
static_assert(IsUint<1, int32_t>(1), "TestIsUint32#3");
static_assert(!IsUint<1, int32_t>(2), "TestIsUint32#4");
static_assert(!IsUint<4, int32_t>(-1), "TestIsUint32#5");
static_assert(IsUint<4, int32_t>(0), "TestIsUint32#6");
static_assert(IsUint<4, int32_t>(15), "TestIsUint32#7");
static_assert(!IsUint<4, int32_t>(16), "TestIsUint32#8");
static_assert(!IsUint<30, int32_t>(std::numeric_limits<int32_t>::max()), "TestIsUint32#9");
static_assert(IsUint<31, int32_t>(std::numeric_limits<int32_t>::max()), "TestIsUint32#10");
static_assert(!IsUint<32, int32_t>(-1), "TestIsUint32#11");
static_assert(IsUint<32, int32_t>(0), "TestIsUint32#11");
static_assert(IsUint<32, uint32_t>(static_cast<uint32_t>(-1)), "TestIsUint32#12");

static_assert(!IsUint<1, int64_t>(-1), "TestIsUint64#1");
static_assert(IsUint<1, int64_t>(0), "TestIsUint64#2");
static_assert(IsUint<1, int64_t>(1), "TestIsUint64#3");
static_assert(!IsUint<1, int64_t>(2), "TestIsUint64#4");
static_assert(!IsUint<4, int64_t>(-1), "TestIsUint64#5");
static_assert(IsUint<4, int64_t>(0), "TestIsUint64#6");
static_assert(IsUint<4, int64_t>(15), "TestIsUint64#7");
static_assert(!IsUint<4, int64_t>(16), "TestIsUint64#8");
static_assert(!IsUint<30, int64_t>(std::numeric_limits<int32_t>::max()), "TestIsUint64#9");
static_assert(IsUint<31, int64_t>(std::numeric_limits<int32_t>::max()), "TestIsUint64#10");
static_assert(!IsUint<62, int64_t>(std::numeric_limits<int64_t>::max()), "TestIsUint64#11");
static_assert(IsUint<63, int64_t>(std::numeric_limits<int64_t>::max()), "TestIsUint64#12");
static_assert(!IsUint<64, int64_t>(-1), "TestIsUint64#13");
static_assert(IsUint<64, int64_t>(0), "TestIsUint64#14");
static_assert(IsUint<64, uint64_t>(static_cast<uint32_t>(-1)), "TestIsUint64#15");

static_assert(!IsAbsoluteUint<1, int32_t>(-2), "TestIsAbsoluteUint32#1");
static_assert(IsAbsoluteUint<1, int32_t>(-1), "TestIsAbsoluteUint32#2");
static_assert(IsAbsoluteUint<1, int32_t>(0), "TestIsAbsoluteUint32#3");
static_assert(IsAbsoluteUint<1, int32_t>(1), "TestIsAbsoluteUint32#4");
static_assert(!IsAbsoluteUint<1, int32_t>(2), "TestIsAbsoluteUint32#5");
static_assert(!IsAbsoluteUint<4, int32_t>(-16), "TestIsAbsoluteUint32#6");
static_assert(IsAbsoluteUint<4, int32_t>(-15), "TestIsAbsoluteUint32#7");
static_assert(IsAbsoluteUint<4, int32_t>(0), "TestIsAbsoluteUint32#8");
static_assert(IsAbsoluteUint<4, int32_t>(15), "TestIsAbsoluteUint32#9");
static_assert(!IsAbsoluteUint<4, int32_t>(16), "TestIsAbsoluteUint32#10");
static_assert(!IsAbsoluteUint<30, int32_t>(std::numeric_limits<int32_t>::max()),
              "TestIsAbsoluteUint32#11");
static_assert(IsAbsoluteUint<31, int32_t>(std::numeric_limits<int32_t>::max()),
              "TestIsAbsoluteUint32#12");
static_assert(!IsAbsoluteUint<31, int32_t>(std::numeric_limits<int32_t>::min()),
              "TestIsAbsoluteUint32#13");
static_assert(IsAbsoluteUint<31, int32_t>(std::numeric_limits<int32_t>::min() + 1),
              "TestIsAbsoluteUint32#14");
static_assert(IsAbsoluteUint<32, int32_t>(std::numeric_limits<int32_t>::max()),
              "TestIsAbsoluteUint32#15");
static_assert(IsAbsoluteUint<32, int32_t>(std::numeric_limits<int32_t>::min()),
              "TestIsAbsoluteUint32#16");
static_assert(IsAbsoluteUint<32, int32_t>(0), "TestIsAbsoluteUint32#17");

static_assert(!IsAbsoluteUint<1, int64_t>(-2), "TestIsAbsoluteUint64#1");
static_assert(IsAbsoluteUint<1, int64_t>(-1), "TestIsAbsoluteUint64#2");
static_assert(IsAbsoluteUint<1, int64_t>(0), "TestIsAbsoluteUint64#3");
static_assert(IsAbsoluteUint<1, int64_t>(1), "TestIsAbsoluteUint64#4");
static_assert(!IsAbsoluteUint<1, int64_t>(2), "TestIsAbsoluteUint64#5");
static_assert(!IsAbsoluteUint<4, int64_t>(-16), "TestIsAbsoluteUint64#6");
static_assert(IsAbsoluteUint<4, int64_t>(-15), "TestIsAbsoluteUint64#7");
static_assert(IsAbsoluteUint<4, int64_t>(0), "TestIsAbsoluteUint64#8");
static_assert(IsAbsoluteUint<4, int64_t>(15), "TestIsAbsoluteUint64#9");
static_assert(!IsAbsoluteUint<4, int64_t>(16), "TestIsAbsoluteUint64#10");
static_assert(!IsAbsoluteUint<30, int64_t>(std::numeric_limits<int32_t>::max()),
              "TestIsAbsoluteUint64#11");
static_assert(IsAbsoluteUint<31, int64_t>(std::numeric_limits<int32_t>::max()),
              "TestIsAbsoluteUint64#12");
static_assert(!IsAbsoluteUint<31, int64_t>(std::numeric_limits<int32_t>::min()),
              "TestIsAbsoluteUint64#13");
static_assert(IsAbsoluteUint<31, int64_t>(std::numeric_limits<int32_t>::min() + 1),
              "TestIsAbsoluteUint64#14");
static_assert(IsAbsoluteUint<32, int64_t>(std::numeric_limits<int32_t>::max()),
              "TestIsAbsoluteUint64#15");
static_assert(IsAbsoluteUint<32, int64_t>(std::numeric_limits<int32_t>::min()),
              "TestIsAbsoluteUint64#16");
static_assert(!IsAbsoluteUint<62, int64_t>(std::numeric_limits<int64_t>::max()),
              "TestIsAbsoluteUint64#17");
static_assert(IsAbsoluteUint<63, int64_t>(std::numeric_limits<int64_t>::max()),
              "TestIsAbsoluteUint64#18");
static_assert(!IsAbsoluteUint<63, int64_t>(std::numeric_limits<int64_t>::min()),
              "TestIsAbsoluteUint64#19");
static_assert(IsAbsoluteUint<63, int64_t>(std::numeric_limits<int64_t>::min() + 1),
              "TestIsAbsoluteUint64#20");
static_assert(IsAbsoluteUint<64, int64_t>(std::numeric_limits<int64_t>::max()),
              "TestIsAbsoluteUint64#21");
static_assert(IsAbsoluteUint<64, int64_t>(std::numeric_limits<int64_t>::min()),
              "TestIsAbsoluteUint64#22");
static_assert(!IsAbsoluteUint<32, int64_t>(-kUint32MaxPlus1), "TestIsAbsoluteUint64#23");
static_assert(IsAbsoluteUint<32, int64_t>(-kUint32MaxPlus1 + 1), "TestIsAbsoluteUint64#24");
static_assert(IsAbsoluteUint<32, int64_t>(0), "TestIsAbsoluteUint64#25");
static_assert(IsAbsoluteUint<64, int64_t>(0), "TestIsAbsoluteUint64#26");
static_assert(IsAbsoluteUint<32, int64_t>(std::numeric_limits<uint32_t>::max()),
              "TestIsAbsoluteUint64#27");
static_assert(!IsAbsoluteUint<32, int64_t>(kUint32MaxPlus1), "TestIsAbsoluteUint64#28");

template <typename Container>
void CheckElements(const std::initializer_list<uint32_t>& expected, const Container& elements) {
  auto expected_it = expected.begin();
  auto element_it = elements.begin();
  size_t idx = 0u;
  while (expected_it != expected.end() && element_it != elements.end()) {
    EXPECT_EQ(*expected_it, *element_it) << idx;
    ++idx;
    ++expected_it;
    ++element_it;
  }
  ASSERT_TRUE(expected_it == expected.end() && element_it == elements.end())
      << std::boolalpha << (expected_it == expected.end()) << " " << (element_it == elements.end());
}

TEST(BitUtilsTest, TestLowToHighBits32) {
  CheckElements({}, LowToHighBits<uint32_t>(0u));
  CheckElements({0}, LowToHighBits<uint32_t>(1u));
  CheckElements({15}, LowToHighBits<uint32_t>(0x8000u));
  CheckElements({31}, LowToHighBits<uint32_t>(0x80000000u));
  CheckElements({0, 31}, LowToHighBits<uint32_t>(0x80000001u));
  CheckElements({0, 1, 2, 3, 4, 5, 6, 7, 31}, LowToHighBits<uint32_t>(0x800000ffu));
  CheckElements({0, 8, 16, 24, 31}, LowToHighBits<uint32_t>(0x81010101u));
  CheckElements({16, 17, 30, 31}, LowToHighBits<uint32_t>(0xc0030000u));
  CheckElements({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31},
                LowToHighBits<uint32_t>(0xffffffffu));
}

TEST(BitUtilsTest, TestLowToHighBits64) {
  CheckElements({}, LowToHighBits<uint64_t>(UINT64_C(0)));
  CheckElements({0}, LowToHighBits<uint64_t>(UINT64_C(1)));
  CheckElements({32}, LowToHighBits<uint64_t>(UINT64_C(0x100000000)));
  CheckElements({63}, LowToHighBits<uint64_t>(UINT64_C(0x8000000000000000)));
  CheckElements({0, 63}, LowToHighBits<uint64_t>(UINT64_C(0x8000000000000001)));
  CheckElements({0, 1, 2, 3, 4, 5, 6, 7, 63},
                LowToHighBits<uint64_t>(UINT64_C(0x80000000000000ff)));
  CheckElements({0, 8, 16, 24, 32, 40, 48, 56, 63},
                LowToHighBits<uint64_t>(UINT64_C(0x8101010101010101)));
  CheckElements({16, 17, 62, 63}, LowToHighBits<uint64_t>(UINT64_C(0xc000000000030000)));
  CheckElements({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63},
                LowToHighBits<uint64_t>(UINT64_C(0xffffffffffffffff)));
}

TEST(BitUtilsTest, TestHighToLowBits32) {
  CheckElements({}, HighToLowBits<uint32_t>(0u));
  CheckElements({0}, HighToLowBits<uint32_t>(1u));
  CheckElements({15}, HighToLowBits<uint32_t>(0x8000u));
  CheckElements({31}, HighToLowBits<uint32_t>(0x80000000u));
  CheckElements({31, 0}, HighToLowBits<uint32_t>(0x80000001u));
  CheckElements({31, 7, 6, 5, 4, 3, 2, 1, 0}, HighToLowBits<uint32_t>(0x800000ffu));
  CheckElements({31, 24, 16, 8, 0}, HighToLowBits<uint32_t>(0x81010101u));
  CheckElements({31, 30, 17, 16}, HighToLowBits<uint32_t>(0xc0030000u));
  CheckElements({31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
                 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
                HighToLowBits<uint32_t>(0xffffffffu));
}

TEST(BitUtilsTest, TestHighToLowBits64) {
  CheckElements({}, HighToLowBits<uint64_t>(UINT64_C(0)));
  CheckElements({0}, HighToLowBits<uint64_t>(UINT64_C(1)));
  CheckElements({32}, HighToLowBits<uint64_t>(UINT64_C(0x100000000)));
  CheckElements({63}, HighToLowBits<uint64_t>(UINT64_C(0x8000000000000000)));
  CheckElements({63, 0}, HighToLowBits<uint64_t>(UINT64_C(0x8000000000000001)));
  CheckElements({63, 7, 6, 5, 4, 3, 2, 1, 0},
                HighToLowBits<uint64_t>(UINT64_C(0x80000000000000ff)));
  CheckElements({63, 56, 48, 40, 32, 24, 16, 8, 0},
                HighToLowBits<uint64_t>(UINT64_C(0x8101010101010101)));
  CheckElements({63, 62, 17, 16}, HighToLowBits<uint64_t>(UINT64_C(0xc000000000030000)));
  CheckElements({63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48,
                 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
                 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
                 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
                HighToLowBits<uint64_t>(UINT64_C(0xffffffffffffffff)));
}

}  // namespace art
