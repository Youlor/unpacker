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

#include "gtest/gtest.h"
#include "memcmp16.h"

class RandGen {
 public:
  explicit RandGen(uint32_t seed) : val_(seed) {}

  uint32_t next() {
    val_ = val_ * 48271 % 2147483647 + 13;
    return val_;
  }

  uint32_t val_;
};

class MemCmp16Test : public testing::Test {
};

// A simple implementation to compare against.
// Note: this version is equivalent to the generic one used when no optimized version is available.
int32_t memcmp16_compare(const uint16_t* s0, const uint16_t* s1, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (s0[i] != s1[i]) {
      return static_cast<int32_t>(s0[i]) - static_cast<int32_t>(s1[i]);
    }
  }
  return 0;
}

static constexpr size_t kMemCmp16Rounds = 100000;

static void CheckSeparate(size_t max_length, size_t min_length) {
  RandGen r(0x1234);
  size_t range_of_tests = 7;  // All four (weighted) tests active in the beginning.

  for (size_t round = 0; round < kMemCmp16Rounds; ++round) {
    size_t type = r.next() % range_of_tests;
    size_t count1, count2;
    uint16_t *s1, *s2;  // Use raw pointers to simplify using clobbered addresses

    switch (type) {
      case 0:  // random, non-zero lengths of both strings
      case 1:
      case 2:
      case 3:
        count1 = (r.next() % max_length) + min_length;
        count2 = (r.next() % max_length) + min_length;
        break;

      case 4:  // random non-zero length of first, second is zero
        count1 = (r.next() % max_length) + min_length;
        count2 = 0U;
        break;

      case 5:  // random non-zero length of second, first is zero
        count1 = 0U;
        count2 = (r.next() % max_length) + min_length;
        break;

      case 6:  // both zero-length
        count1 = 0U;
        count2 = 0U;
        range_of_tests = 6;  // Don't do zero-zero again.
        break;

      default:
        ASSERT_TRUE(false) << "Should not get here.";
        continue;
    }

    if (count1 > 0U) {
      s1 = new uint16_t[count1];
    } else {
      // Leave a random pointer, should not be touched.
      s1 = reinterpret_cast<uint16_t*>(0xebad1001);
    }

    if (count2 > 0U) {
      s2 = new uint16_t[count2];
    } else {
      // Leave a random pointer, should not be touched.
      s2 = reinterpret_cast<uint16_t*>(0xebad2002);
    }

    size_t min = count1 < count2 ? count1 : count2;
    bool fill_same = r.next() % 1 == 1;

    if (fill_same) {
      for (size_t i = 0; i < min; ++i) {
        s1[i] = static_cast<uint16_t>(r.next() & 0xFFFF);
        s2[i] = s1[i];
      }
      for (size_t i = min; i < count1; ++i) {
        s1[i] = static_cast<uint16_t>(r.next() & 0xFFFF);
      }
      for (size_t i = min; i < count2; ++i) {
        s2[i] = static_cast<uint16_t>(r.next() & 0xFFFF);
      }
    } else {
      for (size_t i = 0; i < count1; ++i) {
        s1[i] = static_cast<uint16_t>(r.next() & 0xFFFF);
      }
      for (size_t i = 0; i < count2; ++i) {
        s2[i] = static_cast<uint16_t>(r.next() & 0xFFFF);
      }
    }

    uint16_t* s1_pot_unaligned = s1;
    uint16_t* s2_pot_unaligned = s2;
    size_t c1_mod = count1;
    size_t c2_mod = count2;

    if (!fill_same) {  // Don't waste a good "long" test.
      if (count1 > 1 && r.next() % 10 == 0) {
        c1_mod--;
        s1_pot_unaligned++;
      }
      if (count2 > 1 && r.next() % 10 == 0) {
        c2_mod--;
        s2_pot_unaligned++;
      }
    }
    size_t mod_min = c1_mod < c2_mod ? c1_mod : c2_mod;

    int32_t expected = memcmp16_compare(s1_pot_unaligned, s2_pot_unaligned, mod_min);
    int32_t computed = art::testing::MemCmp16Testing(s1_pot_unaligned, s2_pot_unaligned, mod_min);

    ASSERT_EQ(expected, computed) << "Run " << round << ", c1=" << count1 << " c2=" << count2;

    if (count1 > 0U) {
      delete[] s1;
    }
    if (count2 > 0U) {
      delete[] s2;
    }
  }
}

TEST_F(MemCmp16Test, RandomSeparateShort) {
  CheckSeparate(5U, 1U);
}

TEST_F(MemCmp16Test, RandomSeparateLong) {
  CheckSeparate(64U, 32U);
}

// TODO: What's a good test for overlapping memory. Is it important?
// TEST_F(MemCmp16Test, RandomOverlay) {
//
// }
