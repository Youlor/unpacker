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

#include "instruction_set_features_mips.h"

#include <fstream>
#include <sstream>

#include "base/stringprintf.h"
#include "utils.h"  // For Trim.

namespace art {

// An enum for the Mips revision.
enum class MipsLevel {
  kBase,
  kR2,
  kR5,
  kR6
};

#if defined(_MIPS_ARCH_MIPS32R6)
static constexpr MipsLevel kRuntimeMipsLevel = MipsLevel::kR6;
#elif defined(_MIPS_ARCH_MIPS32R5)
static constexpr MipsLevel kRuntimeMipsLevel = MipsLevel::kR5;
#elif defined(_MIPS_ARCH_MIPS32R2)
static constexpr MipsLevel kRuntimeMipsLevel = MipsLevel::kR2;
#else
static constexpr MipsLevel kRuntimeMipsLevel = MipsLevel::kBase;
#endif

static void GetFlagsFromCppDefined(bool* mips_isa_gte2, bool* r6, bool* fpu_32bit) {
  // Override defaults based on compiler flags.
  if (kRuntimeMipsLevel >= MipsLevel::kR2) {
    *mips_isa_gte2 = true;
  } else {
    *mips_isa_gte2 = false;
  }

  if (kRuntimeMipsLevel >= MipsLevel::kR5) {
    *fpu_32bit = false;
  } else {
    *fpu_32bit = true;
  }

  if (kRuntimeMipsLevel >= MipsLevel::kR6) {
    *r6 = true;
  } else {
    *r6 = false;
  }
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromVariant(
    const std::string& variant, std::string* error_msg ATTRIBUTE_UNUSED) {

  bool smp = true;  // Conservative default.

  // Override defaults based on compiler flags.
  // This is needed when running ART test where the variant is not defined.
  bool fpu_32bit;
  bool mips_isa_gte2;
  bool r6;
  GetFlagsFromCppDefined(&mips_isa_gte2, &r6, &fpu_32bit);

  // Override defaults based on variant string.
  // Only care if it is R1, R2 or R6 and we assume all CPUs will have a FP unit.
  constexpr const char* kMips32Prefix = "mips32r";
  const size_t kPrefixLength = strlen(kMips32Prefix);
  if (variant.compare(0, kPrefixLength, kMips32Prefix, kPrefixLength) == 0 &&
      variant.size() > kPrefixLength) {
    if (variant[kPrefixLength] >= '6') {
      fpu_32bit = false;
      r6 = true;
    }
    if (variant[kPrefixLength] >= '2') {
      mips_isa_gte2 = true;
    }
  } else if (variant == "default") {
    // Default variant is: smp = true, has fpu, is gte2, is not r6. This is the traditional
    // setting.
    mips_isa_gte2 = true;
  } else {
    LOG(WARNING) << "Unexpected CPU variant for Mips32 using defaults: " << variant;
  }

  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2, r6);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromBitmap(uint32_t bitmap) {
  bool smp = (bitmap & kSmpBitfield) != 0;
  bool fpu_32bit = (bitmap & kFpu32Bitfield) != 0;
  bool mips_isa_gte2 = (bitmap & kIsaRevGte2Bitfield) != 0;
  bool r6 = (bitmap & kR6) != 0;
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2, r6);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromCppDefines() {
  // Assume conservative defaults.
  const bool smp = true;

  bool fpu_32bit;
  bool mips_isa_gte2;
  bool r6;
  GetFlagsFromCppDefined(&mips_isa_gte2, &r6, &fpu_32bit);

  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2, r6);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromCpuInfo() {
  // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
  // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
  // Assume conservative defaults.
  bool smp = false;

  bool fpu_32bit;
  bool mips_isa_gte2;
  bool r6;
  GetFlagsFromCppDefined(&mips_isa_gte2, &r6, &fpu_32bit);

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
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2, r6);
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromHwcap() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

const MipsInstructionSetFeatures* MipsInstructionSetFeatures::FromAssembly() {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines();
}

bool MipsInstructionSetFeatures::Equals(const InstructionSetFeatures* other) const {
  if (kMips != other->GetInstructionSet()) {
    return false;
  }
  const MipsInstructionSetFeatures* other_as_mips = other->AsMipsInstructionSetFeatures();
  return (IsSmp() == other->IsSmp()) &&
      (fpu_32bit_ == other_as_mips->fpu_32bit_) &&
      (mips_isa_gte2_ == other_as_mips->mips_isa_gte2_) &&
      (r6_ == other_as_mips->r6_);
}

uint32_t MipsInstructionSetFeatures::AsBitmap() const {
  return (IsSmp() ? kSmpBitfield : 0) |
      (fpu_32bit_ ? kFpu32Bitfield : 0) |
      (mips_isa_gte2_ ? kIsaRevGte2Bitfield : 0) |
      (r6_ ? kR6 : 0);
}

std::string MipsInstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if (IsSmp()) {
    result += "smp";
  } else {
    result += "-smp";
  }
  if (fpu_32bit_) {
    result += ",fpu32";
  } else {
    result += ",-fpu32";
  }
  if (mips_isa_gte2_) {
    result += ",mips2";
  } else {
    result += ",-mips2";
  }
  if (r6_) {
    result += ",r6";
  }  // Suppress non-r6.
  return result;
}

const InstructionSetFeatures* MipsInstructionSetFeatures::AddFeaturesFromSplitString(
    const bool smp, const std::vector<std::string>& features, std::string* error_msg) const {
  bool fpu_32bit = fpu_32bit_;
  bool mips_isa_gte2 = mips_isa_gte2_;
  bool r6 = r6_;
  for (auto i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "fpu32") {
      fpu_32bit = true;
    } else if (feature == "-fpu32") {
      fpu_32bit = false;
    } else if (feature == "mips2") {
      mips_isa_gte2 = true;
    } else if (feature == "-mips2") {
      mips_isa_gte2 = false;
    } else if (feature == "r6") {
      r6 = true;
    } else if (feature == "-r6") {
      r6 = false;
    } else {
      *error_msg = StringPrintf("Unknown instruction set feature: '%s'", feature.c_str());
      return nullptr;
    }
  }
  return new MipsInstructionSetFeatures(smp, fpu_32bit, mips_isa_gte2, r6);
}

}  // namespace art
