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

#include "hex_dump.h"

#include "globals.h"

#include "gtest/gtest.h"

#include <stdint.h>

namespace art {

#if defined(__LP64__)
#define ZEROPREFIX "00000000"
#else
#define ZEROPREFIX
#endif

TEST(HexDump, OneLine) {
  const char* test_text = "0123456789abcdef";
  std::ostringstream oss;
  oss << HexDump(test_text, strlen(test_text), false, "");
  EXPECT_STREQ(oss.str().c_str(),
               ZEROPREFIX
               "00000000: 30 31 32 33 34 35 36 37 38 39 61 62 63 64 65 66  0123456789abcdef");
}

TEST(HexDump, MultiLine) {
  const char* test_text = "0123456789abcdef0123456789ABCDEF";
  std::ostringstream oss;
  oss << HexDump(test_text, strlen(test_text), false, "");
  EXPECT_STREQ(oss.str().c_str(),
               ZEROPREFIX
               "00000000: 30 31 32 33 34 35 36 37 38 39 61 62 63 64 65 66  0123456789abcdef\n"
               ZEROPREFIX
               "00000010: 30 31 32 33 34 35 36 37 38 39 41 42 43 44 45 46  0123456789ABCDEF");
}

uint64_t g16byte_aligned_number __attribute__ ((aligned(16)));  // NOLINT(whitespace/parens)
TEST(HexDump, ShowActualAddresses) {
  g16byte_aligned_number = 0x6162636465666768;
  std::ostringstream oss;
  oss << HexDump(&g16byte_aligned_number, 8, true, "");
  // Compare ignoring pointer.
  EXPECT_STREQ(oss.str().c_str() + (kBitsPerIntPtrT / 4),
               ": 68 67 66 65 64 63 62 61                          hgfedcba        ");
}

TEST(HexDump, Prefix) {
  const char* test_text = "0123456789abcdef";
  std::ostringstream oss;
  oss << HexDump(test_text, strlen(test_text), false, "test prefix: ");
  EXPECT_STREQ(oss.str().c_str(),
               "test prefix: " ZEROPREFIX "00000000: 30 31 32 33 34 35 36 37 38 39 61 62 63 64 65 66  "
               "0123456789abcdef");
}

}  // namespace art
