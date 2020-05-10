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

#include "stringprintf.h"

#include "gtest/gtest.h"

namespace art {

TEST(StringPrintfTest, HexSizeT) {
  size_t size = 0x00107e59;
  EXPECT_STREQ("00107e59", StringPrintf("%08zx", size).c_str());
  EXPECT_STREQ("0x00107e59", StringPrintf("0x%08zx", size).c_str());
}

}  // namespace art
