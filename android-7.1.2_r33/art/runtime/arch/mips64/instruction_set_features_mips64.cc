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

#include "instruction_set_features_mips64.h"

#include <fstream>
#include <sstream>

#include "base/stringprintf.h"
#include "utils.h"  // For Trim.

namespace art {

const Mips64InstructionSetFeatures* Mips64InstructionSetFeatures::FromVariant(
    const std::string& variant, std::string* error_msg ATTRIBUTE_UNUSED) {
  if (variant != "default" && variant != "mips64r6") {
    LOG(WARNING) << "Unexpected CPU variant for Mips64 using defaults: " << variant;
  }
  bool smp = true;  // Conservative default.
  return new Mips64InstructionSetFeatures(smp);
}

const Mips64InstructionSetFeatures* Mips64InstructionSetFeatures::FromBitmap(uint32_t bitmap) {
  bool smp = (bitmap & kSmpBitfield) != 0;
  return new Mips64InstructionSetFeatures(smp);
}

const Mips64InstructionSetFeatures* Mips64InstructionSetFeatures::FromCppDefines() {
  const bool smp = true;

  return new Mips64InstructionSetFeatures(smp);
}

const Mips64InstructionSetFeatures* Mips64InstructionSetFeatures::FromCpuInfo() {
  // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
  // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
  bool smp = false;

  std::ifstream in("/proc/cpuinfo");
  if (!in.fail()) {
    while (!in.eof()) {
      std::string line;
      std::getline(in, line);
      if (!in.eof()) {
        LOG(INFO) << "cpuinfo line: " << line;
        if (line.find("processor") != std::string::npos && line.find(": 1") != std::string::npos) {
          smp = true;
        }
      }
    }
    in.close();
  } else {
    LOG(ERROR) << "Failed to open /proc/cpuinfo";
  }
  return new Mips64InstructionSetFeatures(smp);
}

const Mips64InstructionSetFeatures* Mips64InstructionSetFeatures::FromHwcap() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

const Mips64InstructionSetFeatures* Mips64InstructionSetFeatures::FromAssembly() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

bool Mips64InstructionSetFeatures::Equals(const InstructionSetFeatures* other) const {
  if (kMips64 != other->GetInstructionSet()) {
    return false;
  }
  return (IsSmp() == other->IsSmp());
}

uint32_t Mips64InstructionSetFeatures::AsBitmap() const {
  return (IsSmp() ? kSmpBitfield : 0);
}

std::string Mips64InstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if (IsSmp()) {
    result += "smp";
  } else {
    result += "-smp";
  }
  return result;
}

const InstructionSetFeatures* Mips64InstructionSetFeatures::AddFeaturesFromSplitString(
    const bool smp, const std::vector<std::string>& features, std::string* error_msg) const {
  auto i = features.begin();
  if (i != features.end()) {
    // We don't have any features.
    std::string feature = Trim(*i);
    *error_msg = StringPrintf("Unknown instruction set feature: '%s'", feature.c_str());
    return nullptr;
  }
  return new Mips64InstructionSetFeatures(smp);
}

}  // namespace art
