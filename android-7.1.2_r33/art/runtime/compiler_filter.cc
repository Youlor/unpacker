/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "compiler_filter.h"

#include "utils.h"

namespace art {

bool CompilerFilter::IsBytecodeCompilationEnabled(Filter filter) {
  switch (filter) {
    case CompilerFilter::kVerifyNone:
    case CompilerFilter::kVerifyAtRuntime:
    case CompilerFilter::kVerifyProfile:
    case CompilerFilter::kInterpretOnly: return false;

    case CompilerFilter::kSpaceProfile:
    case CompilerFilter::kSpace:
    case CompilerFilter::kBalanced:
    case CompilerFilter::kTime:
    case CompilerFilter::kSpeedProfile:
    case CompilerFilter::kSpeed:
    case CompilerFilter::kEverythingProfile:
    case CompilerFilter::kEverything: return true;
  }
  UNREACHABLE();
}

bool CompilerFilter::IsJniCompilationEnabled(Filter filter) {
  switch (filter) {
    case CompilerFilter::kVerifyNone:
    case CompilerFilter::kVerifyAtRuntime: return false;

    case CompilerFilter::kVerifyProfile:
    case CompilerFilter::kInterpretOnly:
    case CompilerFilter::kSpaceProfile:
    case CompilerFilter::kSpace:
    case CompilerFilter::kBalanced:
    case CompilerFilter::kTime:
    case CompilerFilter::kSpeedProfile:
    case CompilerFilter::kSpeed:
    case CompilerFilter::kEverythingProfile:
    case CompilerFilter::kEverything: return true;
  }
  UNREACHABLE();
}

bool CompilerFilter::IsVerificationEnabled(Filter filter) {
  switch (filter) {
    case CompilerFilter::kVerifyNone:
    case CompilerFilter::kVerifyAtRuntime: return false;

    case CompilerFilter::kVerifyProfile:
    case CompilerFilter::kInterpretOnly:
    case CompilerFilter::kSpaceProfile:
    case CompilerFilter::kSpace:
    case CompilerFilter::kBalanced:
    case CompilerFilter::kTime:
    case CompilerFilter::kSpeedProfile:
    case CompilerFilter::kSpeed:
    case CompilerFilter::kEverythingProfile:
    case CompilerFilter::kEverything: return true;
  }
  UNREACHABLE();
}

bool CompilerFilter::DependsOnImageChecksum(Filter filter) {
  // We run dex2dex with verification, so the oat file will depend on the
  // image checksum if verification is enabled.
  return IsVerificationEnabled(filter);
}

bool CompilerFilter::DependsOnProfile(Filter filter) {
  switch (filter) {
    case CompilerFilter::kVerifyNone:
    case CompilerFilter::kVerifyAtRuntime:
    case CompilerFilter::kInterpretOnly:
    case CompilerFilter::kSpace:
    case CompilerFilter::kBalanced:
    case CompilerFilter::kTime:
    case CompilerFilter::kSpeed:
    case CompilerFilter::kEverything: return false;

    case CompilerFilter::kVerifyProfile:
    case CompilerFilter::kSpaceProfile:
    case CompilerFilter::kSpeedProfile:
    case CompilerFilter::kEverythingProfile: return true;
  }
  UNREACHABLE();
}

CompilerFilter::Filter CompilerFilter::GetNonProfileDependentFilterFrom(Filter filter) {
  switch (filter) {
    case CompilerFilter::kVerifyNone:
    case CompilerFilter::kVerifyAtRuntime:
    case CompilerFilter::kInterpretOnly:
    case CompilerFilter::kSpace:
    case CompilerFilter::kBalanced:
    case CompilerFilter::kTime:
    case CompilerFilter::kSpeed:
    case CompilerFilter::kEverything:
      return filter;

    case CompilerFilter::kVerifyProfile:
      return CompilerFilter::kInterpretOnly;

    case CompilerFilter::kSpaceProfile:
      return CompilerFilter::kSpace;

    case CompilerFilter::kSpeedProfile:
      return CompilerFilter::kSpeed;

    case CompilerFilter::kEverythingProfile:
      return CompilerFilter::kEverything;
  }
  UNREACHABLE();
}


bool CompilerFilter::IsAsGoodAs(Filter current, Filter target) {
  return current >= target;
}

std::string CompilerFilter::NameOfFilter(Filter filter) {
  switch (filter) {
    case CompilerFilter::kVerifyNone: return "verify-none";
    case CompilerFilter::kVerifyAtRuntime: return "verify-at-runtime";
    case CompilerFilter::kVerifyProfile: return "verify-profile";
    case CompilerFilter::kInterpretOnly: return "interpret-only";
    case CompilerFilter::kSpaceProfile: return "space-profile";
    case CompilerFilter::kSpace: return "space";
    case CompilerFilter::kBalanced: return "balanced";
    case CompilerFilter::kTime: return "time";
    case CompilerFilter::kSpeedProfile: return "speed-profile";
    case CompilerFilter::kSpeed: return "speed";
    case CompilerFilter::kEverythingProfile: return "everything-profile";
    case CompilerFilter::kEverything: return "everything";
  }
  UNREACHABLE();
}

bool CompilerFilter::ParseCompilerFilter(const char* option, Filter* filter) {
  CHECK(filter != nullptr);

  if (strcmp(option, "verify-none") == 0) {
    *filter = kVerifyNone;
  } else if (strcmp(option, "interpret-only") == 0) {
    *filter = kInterpretOnly;
  } else if (strcmp(option, "verify-profile") == 0) {
    *filter = kVerifyProfile;
  } else if (strcmp(option, "verify-at-runtime") == 0) {
    *filter = kVerifyAtRuntime;
  } else if (strcmp(option, "space") == 0) {
    *filter = kSpace;
  } else if (strcmp(option, "space-profile") == 0) {
    *filter = kSpaceProfile;
  } else if (strcmp(option, "balanced") == 0) {
    *filter = kBalanced;
  } else if (strcmp(option, "speed") == 0) {
    *filter = kSpeed;
  } else if (strcmp(option, "speed-profile") == 0) {
    *filter = kSpeedProfile;
  } else if (strcmp(option, "everything") == 0) {
    *filter = kEverything;
  } else if (strcmp(option, "everything-profile") == 0) {
    *filter = kEverythingProfile;
  } else if (strcmp(option, "time") == 0) {
    *filter = kTime;
  } else {
    return false;
  }
  return true;
}

std::ostream& operator<<(std::ostream& os, const CompilerFilter::Filter& rhs) {
  return os << CompilerFilter::NameOfFilter(rhs);
}

}  // namespace art
