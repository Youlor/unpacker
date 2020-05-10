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

#include "reference_table.h"

#include "common_runtime_test.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/string.h"
#include "primitive.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

class ReferenceTableTest : public CommonRuntimeTest {};

TEST_F(ReferenceTableTest, Basics) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Object* o1 = mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello");

  ReferenceTable rt("test", 0, 11);

  // Check dumping the empty table.
  {
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find("(empty)"), std::string::npos) << oss.str();
    EXPECT_EQ(0U, rt.Size());
  }

  // Check removal of all nullss in a empty table is a no-op.
  rt.Remove(nullptr);
  EXPECT_EQ(0U, rt.Size());

  // Check removal of all o1 in a empty table is a no-op.
  rt.Remove(o1);
  EXPECT_EQ(0U, rt.Size());

  // Add o1 and check we have 1 element and can dump.
  {
    rt.Add(o1);
    EXPECT_EQ(1U, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find("1 of java.lang.String"), std::string::npos) << oss.str();
    EXPECT_EQ(oss.str().find("short[]"), std::string::npos) << oss.str();
  }

  // Add a second object 10 times and check dumping is sane.
  mirror::Object* o2 = mirror::ShortArray::Alloc(soa.Self(), 0);
  for (size_t i = 0; i < 10; ++i) {
    rt.Add(o2);
    EXPECT_EQ(i + 2, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find(StringPrintf("Last %zd entries (of %zd):",
                                          i + 2 > 10 ? 10 : i + 2,
                                          i + 2)),
              std::string::npos) << oss.str();
    EXPECT_NE(oss.str().find("1 of java.lang.String"), std::string::npos) << oss.str();
    if (i == 0) {
      EXPECT_NE(oss.str().find("1 of short[]"), std::string::npos) << oss.str();
    } else {
      EXPECT_NE(oss.str().find(StringPrintf("%zd of short[] (1 unique instances)", i + 1)),
                std::string::npos) << oss.str();
    }
  }

  // Remove o1 (first element).
  {
    rt.Remove(o1);
    EXPECT_EQ(10U, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_EQ(oss.str().find("java.lang.String"), std::string::npos) << oss.str();
  }

  // Remove o2 ten times.
  for (size_t i = 0; i < 10; ++i) {
    rt.Remove(o2);
    EXPECT_EQ(9 - i, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    if (i == 9) {
      EXPECT_EQ(oss.str().find("short[]"), std::string::npos) << oss.str();
    } else if (i == 8) {
      EXPECT_NE(oss.str().find("1 of short[]"), std::string::npos) << oss.str();
    } else {
      EXPECT_NE(oss.str().find(StringPrintf("%zd of short[] (1 unique instances)", 10 - i - 1)),
                std::string::npos) << oss.str();
    }
  }
}

static std::vector<size_t> FindAll(const std::string& haystack, const char* needle) {
  std::vector<size_t> res;
  size_t start = 0;
  do {
    size_t pos = haystack.find(needle, start);
    if (pos == std::string::npos) {
      break;
    }
    res.push_back(pos);
    start = pos + 1;
  } while (start < haystack.size());
  return res;
}

TEST_F(ReferenceTableTest, SummaryOrder) {
  // Check that the summary statistics are sorted.
  ScopedObjectAccess soa(Thread::Current());

  ReferenceTable rt("test", 0, 20);

  {
    mirror::Object* s1 = mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello");
    mirror::Object* s2 = mirror::String::AllocFromModifiedUtf8(soa.Self(), "world");

    // 3 copies of s1, 2 copies of s2, interleaved.
    for (size_t i = 0; i != 2; ++i) {
      rt.Add(s1);
      rt.Add(s2);
    }
    rt.Add(s1);
  }

  {
    // Differently sized byte arrays. Should be sorted by identical (non-unique cound).
    mirror::Object* b1_1 = mirror::ByteArray::Alloc(soa.Self(), 1);
    rt.Add(b1_1);
    rt.Add(mirror::ByteArray::Alloc(soa.Self(), 2));
    rt.Add(b1_1);
    rt.Add(mirror::ByteArray::Alloc(soa.Self(), 2));
    rt.Add(mirror::ByteArray::Alloc(soa.Self(), 1));
    rt.Add(mirror::ByteArray::Alloc(soa.Self(), 2));
  }

  rt.Add(mirror::CharArray::Alloc(soa.Self(), 0));

  // Now dump, and ensure order.
  std::ostringstream oss;
  rt.Dump(oss);

  // Only do this on the part after Summary.
  std::string base = oss.str();
  size_t summary_pos = base.find("Summary:");
  ASSERT_NE(summary_pos, std::string::npos);

  std::string haystack = base.substr(summary_pos);

  std::vector<size_t> strCounts = FindAll(haystack, "java.lang.String");
  std::vector<size_t> b1Counts = FindAll(haystack, "byte[] (1 elements)");
  std::vector<size_t> b2Counts = FindAll(haystack, "byte[] (2 elements)");
  std::vector<size_t> cCounts = FindAll(haystack, "char[]");

  // Only one each.
  EXPECT_EQ(1u, strCounts.size());
  EXPECT_EQ(1u, b1Counts.size());
  EXPECT_EQ(1u, b2Counts.size());
  EXPECT_EQ(1u, cCounts.size());

  // Expect them to be in order.
  EXPECT_LT(strCounts[0], b1Counts[0]);
  EXPECT_LT(b1Counts[0], b2Counts[0]);
  EXPECT_LT(b2Counts[0], cCounts[0]);
}

}  // namespace art
