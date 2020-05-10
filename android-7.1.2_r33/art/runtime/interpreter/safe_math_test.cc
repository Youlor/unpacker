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

#include "safe_math.h"

#include <limits>

#include "gtest/gtest.h"

namespace art {
namespace interpreter {

TEST(SafeMath, Add) {
  // Adding 1 overflows 0x7ff... to 0x800... aka max and min.
  EXPECT_EQ(SafeAdd(std::numeric_limits<int32_t>::max(), 1),
            std::numeric_limits<int32_t>::min());
  EXPECT_EQ(SafeAdd(std::numeric_limits<int64_t>::max(), 1),
            std::numeric_limits<int64_t>::min());

  // Vanilla arithmetic should work too.
  EXPECT_EQ(SafeAdd(std::numeric_limits<int32_t>::max() - 1, 1),
            std::numeric_limits<int32_t>::max());
  EXPECT_EQ(SafeAdd(std::numeric_limits<int64_t>::max() - 1, 1),
            std::numeric_limits<int64_t>::max());

  EXPECT_EQ(SafeAdd(std::numeric_limits<int32_t>::min() + 1, -1),
            std::numeric_limits<int32_t>::min());
  EXPECT_EQ(SafeAdd(std::numeric_limits<int64_t>::min() + 1, -1),
            std::numeric_limits<int64_t>::min());

  EXPECT_EQ(SafeAdd(int32_t(-1), -1), -2);
  EXPECT_EQ(SafeAdd(int64_t(-1), -1), -2);

  EXPECT_EQ(SafeAdd(int32_t(1), 1), 2);
  EXPECT_EQ(SafeAdd(int64_t(1), 1), 2);

  EXPECT_EQ(SafeAdd(int32_t(-1), 1), 0);
  EXPECT_EQ(SafeAdd(int64_t(-1), 1), 0);

  EXPECT_EQ(SafeAdd(int32_t(1), -1), 0);
  EXPECT_EQ(SafeAdd(int64_t(1), -1), 0);

  // Test sign extension of smaller operand sizes.
  EXPECT_EQ(SafeAdd(int32_t(1), int8_t(-1)), 0);
  EXPECT_EQ(SafeAdd(int64_t(1), int8_t(-1)), 0);
}

TEST(SafeMath, Sub) {
  // Subtracting 1 underflows 0x800... to 0x7ff... aka min and max.
  EXPECT_EQ(SafeSub(std::numeric_limits<int32_t>::min(), 1),
            std::numeric_limits<int32_t>::max());
  EXPECT_EQ(SafeSub(std::numeric_limits<int64_t>::min(), 1),
            std::numeric_limits<int64_t>::max());

  // Vanilla arithmetic should work too.
  EXPECT_EQ(SafeSub(std::numeric_limits<int32_t>::max() - 1, -1),
            std::numeric_limits<int32_t>::max());
  EXPECT_EQ(SafeSub(std::numeric_limits<int64_t>::max() - 1, -1),
            std::numeric_limits<int64_t>::max());

  EXPECT_EQ(SafeSub(std::numeric_limits<int32_t>::min() + 1, 1),
            std::numeric_limits<int32_t>::min());
  EXPECT_EQ(SafeSub(std::numeric_limits<int64_t>::min() + 1, 1),
            std::numeric_limits<int64_t>::min());

  EXPECT_EQ(SafeSub(int32_t(-1), -1), 0);
  EXPECT_EQ(SafeSub(int64_t(-1), -1), 0);

  EXPECT_EQ(SafeSub(int32_t(1), 1), 0);
  EXPECT_EQ(SafeSub(int64_t(1), 1), 0);

  EXPECT_EQ(SafeSub(int32_t(-1), 1), -2);
  EXPECT_EQ(SafeSub(int64_t(-1), 1), -2);

  EXPECT_EQ(SafeSub(int32_t(1), -1), 2);
  EXPECT_EQ(SafeSub(int64_t(1), -1), 2);

  // Test sign extension of smaller operand sizes.
  EXPECT_EQ(SafeAdd(int32_t(1), int8_t(-1)), 0);
  EXPECT_EQ(SafeAdd(int64_t(1), int8_t(-1)), 0);
}

TEST(SafeMath, Mul) {
  // Multiplying by 2 overflows 0x7ff...f to 0xfff...e aka max and -2.
  EXPECT_EQ(SafeMul(std::numeric_limits<int32_t>::max(), 2),
            -2);
  EXPECT_EQ(SafeMul(std::numeric_limits<int64_t>::max(), 2),
            -2);

  // Vanilla arithmetic should work too.
  EXPECT_EQ(SafeMul(std::numeric_limits<int32_t>::max() / 2, 2),
            std::numeric_limits<int32_t>::max() - 1);  // -1 as LSB is lost by division.
  EXPECT_EQ(SafeMul(std::numeric_limits<int64_t>::max() / 2, 2),
            std::numeric_limits<int64_t>::max() - 1);  // -1 as LSB is lost by division.

  EXPECT_EQ(SafeMul(std::numeric_limits<int32_t>::min() / 2, 2),
            std::numeric_limits<int32_t>::min());
  EXPECT_EQ(SafeMul(std::numeric_limits<int64_t>::min() / 2, 2),
            std::numeric_limits<int64_t>::min());

  EXPECT_EQ(SafeMul(int32_t(-1), -1), 1);
  EXPECT_EQ(SafeMul(int64_t(-1), -1), 1);

  EXPECT_EQ(SafeMul(int32_t(1), 1), 1);
  EXPECT_EQ(SafeMul(int64_t(1), 1), 1);

  EXPECT_EQ(SafeMul(int32_t(-1), 1), -1);
  EXPECT_EQ(SafeMul(int64_t(-1), 1), -1);

  EXPECT_EQ(SafeMul(int32_t(1), -1), -1);
  EXPECT_EQ(SafeMul(int64_t(1), -1), -1);

  // Test sign extension of smaller operand sizes.
  EXPECT_EQ(SafeMul(int32_t(1), int8_t(-1)), -1);
  EXPECT_EQ(SafeMul(int64_t(1), int8_t(-1)), -1);
}

}  // namespace interpreter
}  // namespace art
