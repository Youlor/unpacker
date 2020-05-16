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

#include "instruction_set_features_arm64.h"

#include <gtest/gtest.h>

namespace art {

TEST(Arm64InstructionSetFeaturesTest, Arm64Features) {
  // Build features for an ARM64 processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> arm64_features(
      InstructionSetFeatures::FromVariant(kArm64, "default", &error_msg));
  ASSERT_TRUE(arm64_features.get() != nullptr) << error_msg;
  EXPECT_EQ(arm64_features->GetInstructionSet(), kArm64);
  EXPECT_TRUE(arm64_features->Equals(arm64_features.get()));
  EXPECT_STREQ("smp,a53", arm64_features->GetFeatureString().c_str());
  EXPECT_EQ(arm64_features->AsBitmap(), 3U);
}

}  // namespace art
