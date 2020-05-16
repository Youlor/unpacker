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

#include <fstream>
#include <sstream>

#include "arch/x86_64/instruction_set_features_x86_64.h"
#include "base/stringprintf.h"
#include "utils.h"  // For Trim.

namespace art {

// Feature-support arrays.

static constexpr const char* x86_known_variants[] = {
    "atom",
    "silvermont",
};

static constexpr const char* x86_variants_with_ssse3[] = {
    "atom",
    "silvermont",
};

static constexpr const char* x86_variants_with_sse4_1[] = {
    "silvermont",
};

static constexpr const char* x86_variants_with_sse4_2[] = {
    "silvermont",
};

static constexpr const char* x86_variants_prefer_locked_add_sync[] = {
    "atom",
    "silvermont",
};

static constexpr const char* x86_variants_with_popcnt[] = {
    "silvermont",
};

const X86InstructionSetFeatures* X86InstructionSetFeatures::FromVariant(
    const std::string& variant, std::string* error_msg ATTRIBUTE_UNUSED,
    bool x86_64) {
  bool smp = true;  // Conservative default.
  bool has_SSSE3 = FindVariantInArray(x86_variants_with_ssse3, arraysize(x86_variants_with_ssse3),
                                      variant);
  bool has_SSE4_1 = FindVariantInArray(x86_variants_with_sse4_1,
                                       arraysize(x86_variants_with_sse4_1),
                                       variant);
  bool has_SSE4_2 = FindVariantInArray(x86_variants_with_sse4_2,
                                       arraysize(x86_variants_with_sse4_2),
                                       variant);
  bool has_AVX = false;
  bool has_AVX2 = false;

  bool prefers_locked_add = FindVariantInArray(x86_variants_prefer_locked_add_sync,
                                               arraysize(x86_variants_prefer_locked_add_sync),
                                               variant);

  bool has_POPCNT = FindVariantInArray(x86_variants_with_popcnt,
                                       arraysize(x86_variants_with_popcnt),
                                       variant);

  // Verify that variant is known.
  bool known_variant = FindVariantInArray(x86_known_variants, arraysize(x86_known_variants),
                                          variant);
  if (!known_variant && variant != "default") {
    LOG(WARNING) << "Unexpected CPU variant for X86 using defaults: " << variant;
  }

  if (x86_64) {
    return new X86_64InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                            has_AVX2, prefers_locked_add, has_POPCNT);
  } else {
    return new X86InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                            has_AVX2, prefers_locked_add, has_POPCNT);
  }
}

const X86InstructionSetFeatures* X86InstructionSetFeatures::FromBitmap(uint32_t bitmap,
                                                                       bool x86_64) {
  bool smp = (bitmap & kSmpBitfield) != 0;
  bool has_SSSE3 = (bitmap & kSsse3Bitfield) != 0;
  bool has_SSE4_1 = (bitmap & kSse4_1Bitfield) != 0;
  bool has_SSE4_2 = (bitmap & kSse4_2Bitfield) != 0;
  bool has_AVX = (bitmap & kAvxBitfield) != 0;
  bool has_AVX2 = (bitmap & kAvxBitfield) != 0;
  bool prefers_locked_add = (bitmap & kPrefersLockedAdd) != 0;
  bool has_POPCNT = (bitmap & kPopCntBitfield) != 0;
  if (x86_64) {
    return new X86_64InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2,
                                            has_AVX, has_AVX2, prefers_locked_add,
                                            has_POPCNT);
  } else {
    return new X86InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2,
                                         has_AVX, has_AVX2, prefers_locked_add,
                                         has_POPCNT);
  }
}

const X86InstructionSetFeatures* X86InstructionSetFeatures::FromCppDefines(bool x86_64) {
  const bool smp = true;

#ifndef __SSSE3__
  const bool has_SSSE3 = false;
#else
  const bool has_SSSE3 = true;
#endif

#ifndef __SSE4_1__
  const bool has_SSE4_1 = false;
#else
  const bool has_SSE4_1 = true;
#endif

#ifndef __SSE4_2__
  const bool has_SSE4_2 = false;
#else
  const bool has_SSE4_2 = true;
#endif

#ifndef __AVX__
  const bool has_AVX = false;
#else
  const bool has_AVX = true;
#endif

#ifndef __AVX2__
  const bool has_AVX2 = false;
#else
  const bool has_AVX2 = true;
#endif

  // No #define for memory synchronization preference.
  const bool prefers_locked_add = false;

#ifndef __POPCNT__
  const bool has_POPCNT = false;
#else
  const bool has_POPCNT = true;
#endif

  if (x86_64) {
    return new X86_64InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                            has_AVX2, prefers_locked_add, has_POPCNT);
  } else {
    return new X86InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                         has_AVX2, prefers_locked_add, has_POPCNT);
  }
}

const X86InstructionSetFeatures* X86InstructionSetFeatures::FromCpuInfo(bool x86_64) {
  // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
  // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
  bool smp = false;
  bool has_SSSE3 = false;
  bool has_SSE4_1 = false;
  bool has_SSE4_2 = false;
  bool has_AVX = false;
  bool has_AVX2 = false;
  // No cpuinfo for memory synchronization preference.
  const bool prefers_locked_add = false;
  bool has_POPCNT = false;

  std::ifstream in("/proc/cpuinfo");
  if (!in.fail()) {
    while (!in.eof()) {
      std::string line;
      std::getline(in, line);
      if (!in.eof()) {
        LOG(INFO) << "cpuinfo line: " << line;
        if (line.find("flags") != std::string::npos) {
          LOG(INFO) << "found flags";
          if (line.find("ssse3") != std::string::npos) {
            has_SSSE3 = true;
          }
          if (line.find("sse4_1") != std::string::npos) {
            has_SSE4_1 = true;
          }
          if (line.find("sse4_2") != std::string::npos) {
            has_SSE4_2 = true;
          }
          if (line.find("avx") != std::string::npos) {
            has_AVX = true;
          }
          if (line.find("avx2") != std::string::npos) {
            has_AVX2 = true;
          }
          if (line.find("popcnt") != std::string::npos) {
            has_POPCNT = true;
          }
        } else if (line.find("processor") != std::string::npos &&
            line.find(": 1") != std::string::npos) {
          smp = true;
        }
      }
    }
    in.close();
  } else {
    LOG(ERROR) << "Failed to open /proc/cpuinfo";
  }
  if (x86_64) {
    return new X86_64InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                            has_AVX2, prefers_locked_add, has_POPCNT);
  } else {
    return new X86InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                         has_AVX2, prefers_locked_add, has_POPCNT);
  }
}

const X86InstructionSetFeatures* X86InstructionSetFeatures::FromHwcap(bool x86_64) {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines(x86_64);
}

const X86InstructionSetFeatures* X86InstructionSetFeatures::FromAssembly(bool x86_64) {
  UNIMPLEMENTED(WARNING);
  return FromCppDefines(x86_64);
}

bool X86InstructionSetFeatures::Equals(const InstructionSetFeatures* other) const {
  if (GetInstructionSet() != other->GetInstructionSet()) {
    return false;
  }
  const X86InstructionSetFeatures* other_as_x86 = other->AsX86InstructionSetFeatures();
  return (IsSmp() == other->IsSmp()) &&
      (has_SSSE3_ == other_as_x86->has_SSSE3_) &&
      (has_SSE4_1_ == other_as_x86->has_SSE4_1_) &&
      (has_SSE4_2_ == other_as_x86->has_SSE4_2_) &&
      (has_AVX_ == other_as_x86->has_AVX_) &&
      (has_AVX2_ == other_as_x86->has_AVX2_) &&
      (prefers_locked_add_ == other_as_x86->prefers_locked_add_) &&
      (has_POPCNT_ == other_as_x86->has_POPCNT_);
}

uint32_t X86InstructionSetFeatures::AsBitmap() const {
  return (IsSmp() ? kSmpBitfield : 0) |
      (has_SSSE3_ ? kSsse3Bitfield : 0) |
      (has_SSE4_1_ ? kSse4_1Bitfield : 0) |
      (has_SSE4_2_ ? kSse4_2Bitfield : 0) |
      (has_AVX_ ? kAvxBitfield : 0) |
      (has_AVX2_ ? kAvx2Bitfield : 0) |
      (prefers_locked_add_ ? kPrefersLockedAdd : 0) |
      (has_POPCNT_ ? kPopCntBitfield : 0);
}

std::string X86InstructionSetFeatures::GetFeatureString() const {
  std::string result;
  if (IsSmp()) {
    result += "smp";
  } else {
    result += "-smp";
  }
  if (has_SSSE3_) {
    result += ",ssse3";
  } else {
    result += ",-ssse3";
  }
  if (has_SSE4_1_) {
    result += ",sse4.1";
  } else {
    result += ",-sse4.1";
  }
  if (has_SSE4_2_) {
    result += ",sse4.2";
  } else {
    result += ",-sse4.2";
  }
  if (has_AVX_) {
    result += ",avx";
  } else {
    result += ",-avx";
  }
  if (has_AVX2_) {
    result += ",avx2";
  } else {
    result += ",-avx2";
  }
  if (prefers_locked_add_) {
    result += ",lock_add";
  } else {
    result += ",-lock_add";
  }
  if (has_POPCNT_) {
    result += ",popcnt";
  } else {
    result += ",-popcnt";
  }
  return result;
}

const InstructionSetFeatures* X86InstructionSetFeatures::AddFeaturesFromSplitString(
    const bool smp, const std::vector<std::string>& features, bool x86_64,
    std::string* error_msg) const {
  bool has_SSSE3 = has_SSSE3_;
  bool has_SSE4_1 = has_SSE4_1_;
  bool has_SSE4_2 = has_SSE4_2_;
  bool has_AVX = has_AVX_;
  bool has_AVX2 = has_AVX2_;
  bool prefers_locked_add = prefers_locked_add_;
  bool has_POPCNT = has_POPCNT_;
  for (auto i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "ssse3") {
      has_SSSE3 = true;
    } else if (feature == "-ssse3") {
      has_SSSE3 = false;
    } else if (feature == "sse4.1") {
      has_SSE4_1 = true;
    } else if (feature == "-sse4.1") {
      has_SSE4_1 = false;
    } else if (feature == "sse4.2") {
      has_SSE4_2 = true;
    } else if (feature == "-sse4.2") {
      has_SSE4_2 = false;
    } else if (feature == "avx") {
      has_AVX = true;
    } else if (feature == "-avx") {
      has_AVX = false;
    } else if (feature == "avx2") {
      has_AVX2 = true;
    } else if (feature == "-avx2") {
      has_AVX2 = false;
    } else if (feature == "lock_add") {
      prefers_locked_add = true;
    } else if (feature == "-lock_add") {
      prefers_locked_add = false;
    } else if (feature == "popcnt") {
      has_POPCNT = true;
    } else if (feature == "-popcnt") {
      has_POPCNT = false;
    } else {
      *error_msg = StringPrintf("Unknown instruction set feature: '%s'", feature.c_str());
      return nullptr;
    }
  }
  if (x86_64) {
    return new X86_64InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                            has_AVX2, prefers_locked_add, has_POPCNT);
  } else {
    return new X86InstructionSetFeatures(smp, has_SSSE3, has_SSE4_1, has_SSE4_2, has_AVX,
                                         has_AVX2, prefers_locked_add, has_POPCNT);
  }
}

}  // namespace art
