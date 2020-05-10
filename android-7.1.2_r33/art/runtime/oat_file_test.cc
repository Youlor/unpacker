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

#include "oat_file.h"

#include <string>

#include <gtest/gtest.h>

#include "common_runtime_test.h"
#include "scoped_thread_state_change.h"

namespace art {

class OatFileTest : public CommonRuntimeTest {
};

TEST_F(OatFileTest, ResolveRelativeEncodedDexLocation) {
  EXPECT_EQ(std::string("/data/app/foo/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        nullptr, "/data/app/foo/base.apk"));

  EXPECT_EQ(std::string("/system/framework/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "/system/framework/base.apk"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "base.apk"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "foo/base.apk"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk:classes2.dex"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "base.apk:classes2.dex"));

  EXPECT_EQ(std::string("/data/app/foo/base.apk:classes11.dex"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "base.apk:classes11.dex"));

  EXPECT_EQ(std::string("base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/sludge.apk", "base.apk"));

  EXPECT_EQ(std::string("o/base.apk"),
      OatFile::ResolveRelativeEncodedDexLocation(
        "/data/app/foo/base.apk", "o/base.apk"));
}

static std::vector<const DexFile*> ToConstDexFiles(
    const std::vector<std::unique_ptr<const DexFile>>& in) {
  std::vector<const DexFile*> ret;
  for (auto& d : in) {
    ret.push_back(d.get());
  }
  return ret;
}

TEST_F(OatFileTest, DexFileDependencies) {
  std::string error_msg;

  // No dependencies.
  EXPECT_TRUE(OatFile::CheckStaticDexFileDependencies(nullptr, &error_msg)) << error_msg;
  EXPECT_TRUE(OatFile::CheckStaticDexFileDependencies("", &error_msg)) << error_msg;

  // Ill-formed dependencies.
  EXPECT_FALSE(OatFile::CheckStaticDexFileDependencies("abc", &error_msg));
  EXPECT_FALSE(OatFile::CheckStaticDexFileDependencies("abc*123*def", &error_msg));
  EXPECT_FALSE(OatFile::CheckStaticDexFileDependencies("abc*def*", &error_msg));

  // Unsatisfiable dependency.
  EXPECT_FALSE(OatFile::CheckStaticDexFileDependencies("abc*123*", &error_msg));

  // Load some dex files to be able to do a real test.
  ScopedObjectAccess soa(Thread::Current());

  std::vector<std::unique_ptr<const DexFile>> dex_files1 = OpenTestDexFiles("Main");
  std::vector<const DexFile*> dex_files_const1 = ToConstDexFiles(dex_files1);
  std::string encoding1 = OatFile::EncodeDexFileDependencies(dex_files_const1);
  EXPECT_TRUE(OatFile::CheckStaticDexFileDependencies(encoding1.c_str(), &error_msg))
      << error_msg << " " << encoding1;
  std::vector<std::string> split1;
  EXPECT_TRUE(OatFile::GetDexLocationsFromDependencies(encoding1.c_str(), &split1));
  ASSERT_EQ(split1.size(), 1U);
  EXPECT_EQ(split1[0], dex_files_const1[0]->GetLocation());

  std::vector<std::unique_ptr<const DexFile>> dex_files2 = OpenTestDexFiles("MultiDex");
  EXPECT_GT(dex_files2.size(), 1U);
  std::vector<const DexFile*> dex_files_const2 = ToConstDexFiles(dex_files2);
  std::string encoding2 = OatFile::EncodeDexFileDependencies(dex_files_const2);
  EXPECT_TRUE(OatFile::CheckStaticDexFileDependencies(encoding2.c_str(), &error_msg))
      << error_msg << " " << encoding2;
  std::vector<std::string> split2;
  EXPECT_TRUE(OatFile::GetDexLocationsFromDependencies(encoding2.c_str(), &split2));
  ASSERT_EQ(split2.size(), 2U);
  EXPECT_EQ(split2[0], dex_files_const2[0]->GetLocation());
  EXPECT_EQ(split2[1], dex_files_const2[1]->GetLocation());
}

}  // namespace art
