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

#include "time_utils.h"

#include "gtest/gtest.h"

namespace art {

TEST(TimeUtilsTest, PrettyDuration) {
  const uint64_t one_sec = 1000000000;
  const uint64_t one_ms  = 1000000;
  const uint64_t one_us  = 1000;

  EXPECT_EQ("1s", PrettyDuration(1 * one_sec));
  EXPECT_EQ("10s", PrettyDuration(10 * one_sec));
  EXPECT_EQ("100s", PrettyDuration(100 * one_sec));
  EXPECT_EQ("1.001s", PrettyDuration(1 * one_sec + one_ms));
  EXPECT_EQ("1.000001s", PrettyDuration(1 * one_sec + one_us, 6));
  EXPECT_EQ("1.000000001s", PrettyDuration(1 * one_sec + 1, 9));
  EXPECT_EQ("1.000s", PrettyDuration(1 * one_sec + one_us, 3));

  EXPECT_EQ("1ms", PrettyDuration(1 * one_ms));
  EXPECT_EQ("10ms", PrettyDuration(10 * one_ms));
  EXPECT_EQ("100ms", PrettyDuration(100 * one_ms));
  EXPECT_EQ("1.001ms", PrettyDuration(1 * one_ms + one_us));
  EXPECT_EQ("1.000001ms", PrettyDuration(1 * one_ms + 1, 6));

  EXPECT_EQ("1us", PrettyDuration(1 * one_us));
  EXPECT_EQ("10us", PrettyDuration(10 * one_us));
  EXPECT_EQ("100us", PrettyDuration(100 * one_us));
  EXPECT_EQ("1.001us", PrettyDuration(1 * one_us + 1));

  EXPECT_EQ("1ns", PrettyDuration(1));
  EXPECT_EQ("10ns", PrettyDuration(10));
  EXPECT_EQ("100ns", PrettyDuration(100));
}

TEST(TimeUtilsTest, TestSleep) {
  auto start = NanoTime();
  NanoSleep(MsToNs(1500));
  EXPECT_GT(NanoTime() - start, MsToNs(1000));
}

}  // namespace art
