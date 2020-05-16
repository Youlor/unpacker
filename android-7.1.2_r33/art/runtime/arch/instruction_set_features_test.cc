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

#include "instruction_set_features.h"

#include <gtest/gtest.h>

#ifdef __ANDROID__
#include "cutils/properties.h"
#endif

#include "base/stringprintf.h"

namespace art {

#ifdef __ANDROID__
#if defined(__aarch64__)
TEST(InstructionSetFeaturesTest, DISABLED_FeaturesFromSystemPropertyVariant) {
  LOG(WARNING) << "Test disabled due to no CPP define for A53 erratum 835769";
#else
TEST(InstructionSetFeaturesTest, FeaturesFromSystemPropertyVariant) {
#endif
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Read the variant property.
  std::string key = StringPrintf("dalvik.vm.isa.%s.variant", GetInstructionSetString(kRuntimeISA));
  char dex2oat_isa_variant[PROPERTY_VALUE_MAX];
  if (property_get(key.c_str(), dex2oat_isa_variant, nullptr) > 0) {
    // Use features from property to build InstructionSetFeatures and check against build's
    // features.
    std::string error_msg;
    std::unique_ptr<const InstructionSetFeatures> property_features(
        InstructionSetFeatures::FromVariant(kRuntimeISA, dex2oat_isa_variant, &error_msg));
    ASSERT_TRUE(property_features.get() != nullptr) << error_msg;

    EXPECT_TRUE(property_features->Equals(instruction_set_features.get()))
      << "System property features: " << *property_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
  }
}

#if defined(__aarch64__)
TEST(InstructionSetFeaturesTest, DISABLED_FeaturesFromSystemPropertyString) {
  LOG(WARNING) << "Test disabled due to no CPP define for A53 erratum 835769";
#else
TEST(InstructionSetFeaturesTest, FeaturesFromSystemPropertyString) {
#endif
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Read the variant property.
  std::string variant_key = StringPrintf("dalvik.vm.isa.%s.variant",
                                         GetInstructionSetString(kRuntimeISA));
  char dex2oat_isa_variant[PROPERTY_VALUE_MAX];
  if (property_get(variant_key.c_str(), dex2oat_isa_variant, nullptr) > 0) {
    // Read the features property.
    std::string features_key = StringPrintf("dalvik.vm.isa.%s.features",
                                            GetInstructionSetString(kRuntimeISA));
    char dex2oat_isa_features[PROPERTY_VALUE_MAX];
    if (property_get(features_key.c_str(), dex2oat_isa_features, nullptr) > 0) {
      // Use features from property to build InstructionSetFeatures and check against build's
      // features.
      std::string error_msg;
      std::unique_ptr<const InstructionSetFeatures> base_features(
          InstructionSetFeatures::FromVariant(kRuntimeISA, dex2oat_isa_variant, &error_msg));
      ASSERT_TRUE(base_features.get() != nullptr) << error_msg;

      std::unique_ptr<const InstructionSetFeatures> property_features(
          base_features->AddFeaturesFromString(dex2oat_isa_features, &error_msg));
      ASSERT_TRUE(property_features.get() != nullptr) << error_msg;

      EXPECT_TRUE(property_features->Equals(instruction_set_features.get()))
      << "System property features: " << *property_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
    }
  }
}

#if defined(__arm__)
TEST(InstructionSetFeaturesTest, DISABLED_FeaturesFromCpuInfo) {
  LOG(WARNING) << "Test disabled due to buggy ARM kernels";
#else
TEST(InstructionSetFeaturesTest, FeaturesFromCpuInfo) {
#endif
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Check we get the same instruction set features using /proc/cpuinfo.
  std::unique_ptr<const InstructionSetFeatures> cpuinfo_features(
      InstructionSetFeatures::FromCpuInfo());
  EXPECT_TRUE(cpuinfo_features->Equals(instruction_set_features.get()))
      << "CPU Info features: " << *cpuinfo_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
}
#endif

#ifndef __ANDROID__
TEST(InstructionSetFeaturesTest, HostFeaturesFromCppDefines) {
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> default_features(
      InstructionSetFeatures::FromVariant(kRuntimeISA, "default", &error_msg));
  ASSERT_TRUE(error_msg.empty());

  std::unique_ptr<const InstructionSetFeatures> cpp_features(
      InstructionSetFeatures::FromCppDefines());
  EXPECT_TRUE(default_features->Equals(cpp_features.get()))
      << "Default variant features: " << *default_features.get()
      << "\nFeatures from build: " << *cpp_features.get();
}
#endif

#if defined(__arm__)
TEST(InstructionSetFeaturesTest, DISABLED_FeaturesFromHwcap) {
  LOG(WARNING) << "Test disabled due to buggy ARM kernels";
#else
TEST(InstructionSetFeaturesTest, FeaturesFromHwcap) {
#endif
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Check we get the same instruction set features using AT_HWCAP.
  std::unique_ptr<const InstructionSetFeatures> hwcap_features(
      InstructionSetFeatures::FromHwcap());
  EXPECT_TRUE(hwcap_features->Equals(instruction_set_features.get()))
      << "Hwcap features: " << *hwcap_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
}

TEST(InstructionSetFeaturesTest, FeaturesFromAssembly) {
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Check we get the same instruction set features using assembly tests.
  std::unique_ptr<const InstructionSetFeatures> assembly_features(
      InstructionSetFeatures::FromAssembly());
  EXPECT_TRUE(assembly_features->Equals(instruction_set_features.get()))
      << "Assembly features: " << *assembly_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
}

}  // namespace art
