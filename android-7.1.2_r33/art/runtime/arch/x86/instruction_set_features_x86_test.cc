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

#include "instruction_set_features_x86.h"

#include <gtest/gtest.h>

namespace art {

TEST(X86InstructionSetFeaturesTest, X86FeaturesFromDefaultVariant) {
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> x86_features(
      InstructionSetFeatures::FromVariant(kX86, "default", &error_msg));
  ASSERT_TRUE(x86_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_features->Equals(x86_features.get()));
  EXPECT_STREQ("smp,-ssse3,-sse4.1,-sse4.2,-avx,-avx2,-lock_add,-popcnt",
               x86_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_features->AsBitmap(), 1U);
}

TEST(X86InstructionSetFeaturesTest, X86FeaturesFromAtomVariant) {
  // Build features for a 32-bit x86 atom processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> x86_features(
      InstructionSetFeatures::FromVariant(kX86, "atom", &error_msg));
  ASSERT_TRUE(x86_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_features->Equals(x86_features.get()));
  EXPECT_STREQ("smp,ssse3,-sse4.1,-sse4.2,-avx,-avx2,lock_add,-popcnt",
               x86_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_features->AsBitmap(), 67U);

  // Build features for a 32-bit x86 default processor.
  std::unique_ptr<const InstructionSetFeatures> x86_default_features(
      InstructionSetFeatures::FromVariant(kX86, "default", &error_msg));
  ASSERT_TRUE(x86_default_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_default_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_default_features->Equals(x86_default_features.get()));
  EXPECT_STREQ("smp,-ssse3,-sse4.1,-sse4.2,-avx,-avx2,-lock_add,-popcnt",
               x86_default_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_default_features->AsBitmap(), 1U);

  // Build features for a 64-bit x86-64 atom processor.
  std::unique_ptr<const InstructionSetFeatures> x86_64_features(
      InstructionSetFeatures::FromVariant(kX86_64, "atom", &error_msg));
  ASSERT_TRUE(x86_64_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_64_features->GetInstructionSet(), kX86_64);
  EXPECT_TRUE(x86_64_features->Equals(x86_64_features.get()));
  EXPECT_STREQ("smp,ssse3,-sse4.1,-sse4.2,-avx,-avx2,lock_add,-popcnt",
               x86_64_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_64_features->AsBitmap(), 67U);

  EXPECT_FALSE(x86_64_features->Equals(x86_features.get()));
  EXPECT_FALSE(x86_64_features->Equals(x86_default_features.get()));
  EXPECT_FALSE(x86_features->Equals(x86_default_features.get()));
}

TEST(X86InstructionSetFeaturesTest, X86FeaturesFromSilvermontVariant) {
  // Build features for a 32-bit x86 silvermont processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> x86_features(
      InstructionSetFeatures::FromVariant(kX86, "silvermont", &error_msg));
  ASSERT_TRUE(x86_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_features->Equals(x86_features.get()));
  EXPECT_STREQ("smp,ssse3,sse4.1,sse4.2,-avx,-avx2,lock_add,popcnt",
               x86_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_features->AsBitmap(), 207U);

  // Build features for a 32-bit x86 default processor.
  std::unique_ptr<const InstructionSetFeatures> x86_default_features(
      InstructionSetFeatures::FromVariant(kX86, "default", &error_msg));
  ASSERT_TRUE(x86_default_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_default_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_default_features->Equals(x86_default_features.get()));
  EXPECT_STREQ("smp,-ssse3,-sse4.1,-sse4.2,-avx,-avx2,-lock_add,-popcnt",
               x86_default_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_default_features->AsBitmap(), 1U);

  // Build features for a 64-bit x86-64 silvermont processor.
  std::unique_ptr<const InstructionSetFeatures> x86_64_features(
      InstructionSetFeatures::FromVariant(kX86_64, "silvermont", &error_msg));
  ASSERT_TRUE(x86_64_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_64_features->GetInstructionSet(), kX86_64);
  EXPECT_TRUE(x86_64_features->Equals(x86_64_features.get()));
  EXPECT_STREQ("smp,ssse3,sse4.1,sse4.2,-avx,-avx2,lock_add,popcnt",
               x86_64_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_64_features->AsBitmap(), 207U);

  EXPECT_FALSE(x86_64_features->Equals(x86_features.get()));
  EXPECT_FALSE(x86_64_features->Equals(x86_default_features.get()));
  EXPECT_FALSE(x86_features->Equals(x86_default_features.get()));
}

}  // namespace art
