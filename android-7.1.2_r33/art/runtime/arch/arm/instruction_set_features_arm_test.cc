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

#include "instruction_set_features_arm.h"

#include "gtest/gtest.h"

namespace art {

TEST(ArmInstructionSetFeaturesTest, ArmFeaturesFromVariant) {
  // Build features for a 32-bit ARM krait processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> krait_features(
      InstructionSetFeatures::FromVariant(kArm, "krait", &error_msg));
  ASSERT_TRUE(krait_features.get() != nullptr) << error_msg;

  ASSERT_EQ(krait_features->GetInstructionSet(), kArm);
  EXPECT_TRUE(krait_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd());
  EXPECT_STREQ("smp,div,atomic_ldrd_strd", krait_features->GetFeatureString().c_str());
  EXPECT_EQ(krait_features->AsBitmap(), 7U);

  // Build features for a 32-bit ARM denver processor.
  std::unique_ptr<const InstructionSetFeatures> denver_features(
      InstructionSetFeatures::FromVariant(kArm, "denver", &error_msg));
  ASSERT_TRUE(denver_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(denver_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd());
  EXPECT_STREQ("smp,div,atomic_ldrd_strd", denver_features->GetFeatureString().c_str());
  EXPECT_EQ(denver_features->AsBitmap(), 7U);

  // Build features for a 32-bit ARMv7 processor.
  std::unique_ptr<const InstructionSetFeatures> arm7_features(
      InstructionSetFeatures::FromVariant(kArm, "arm7", &error_msg));
  ASSERT_TRUE(arm7_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(arm7_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->Equals(krait_features.get()));
  EXPECT_FALSE(krait_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd());
  EXPECT_STREQ("smp,-div,-atomic_ldrd_strd", arm7_features->GetFeatureString().c_str());
  EXPECT_EQ(arm7_features->AsBitmap(), 1U);

  // ARM6 is not a supported architecture variant.
  std::unique_ptr<const InstructionSetFeatures> arm6_features(
      InstructionSetFeatures::FromVariant(kArm, "arm6", &error_msg));
  EXPECT_TRUE(arm6_features.get() == nullptr);
  EXPECT_NE(error_msg.size(), 0U);
}

TEST(ArmInstructionSetFeaturesTest, ArmAddFeaturesFromString) {
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> base_features(
      InstructionSetFeatures::FromVariant(kArm, "arm7", &error_msg));
  ASSERT_TRUE(base_features.get() != nullptr) << error_msg;

  // Build features for a 32-bit ARM with LPAE and div processor.
  std::unique_ptr<const InstructionSetFeatures> krait_features(
      base_features->AddFeaturesFromString("atomic_ldrd_strd,div", &error_msg));
  ASSERT_TRUE(krait_features.get() != nullptr) << error_msg;

  ASSERT_EQ(krait_features->GetInstructionSet(), kArm);
  EXPECT_TRUE(krait_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd());
  EXPECT_STREQ("smp,div,atomic_ldrd_strd", krait_features->GetFeatureString().c_str());
  EXPECT_EQ(krait_features->AsBitmap(), 7U);

  // Build features for a 32-bit ARM processor with LPAE and div flipped.
  std::unique_ptr<const InstructionSetFeatures> denver_features(
      base_features->AddFeaturesFromString("div,atomic_ldrd_strd", &error_msg));
  ASSERT_TRUE(denver_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(denver_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd());
  EXPECT_STREQ("smp,div,atomic_ldrd_strd", denver_features->GetFeatureString().c_str());
  EXPECT_EQ(denver_features->AsBitmap(), 7U);

  // Build features for a 32-bit default ARM processor.
  std::unique_ptr<const InstructionSetFeatures> arm7_features(
      base_features->AddFeaturesFromString("default", &error_msg));
  ASSERT_TRUE(arm7_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(arm7_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->Equals(krait_features.get()));
  EXPECT_FALSE(krait_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasAtomicLdrdAndStrd());
  EXPECT_STREQ("smp,-div,-atomic_ldrd_strd", arm7_features->GetFeatureString().c_str());
  EXPECT_EQ(arm7_features->AsBitmap(), 1U);
}

}  // namespace art
