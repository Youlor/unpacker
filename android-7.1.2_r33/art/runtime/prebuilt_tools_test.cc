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

#include "common_runtime_test.h"

#include <cstdio>

#include "gtest/gtest.h"

namespace art {

// Run the tests only on host.
#ifndef __ANDROID__

class PrebuiltToolsTest : public CommonRuntimeTest {
};

static void CheckToolsExist(const std::string& tools_dir) {
  const char* tools[] { "as", "objcopy", "objdump" };  // NOLINT
  for (const char* tool : tools) {
    struct stat exec_st;
    std::string exec_path = tools_dir + tool;
    if (stat(exec_path.c_str(), &exec_st) != 0) {
      ADD_FAILURE() << "Cannot find " << tool << " in " << tools_dir;
    }
  }
}

TEST_F(PrebuiltToolsTest, CheckHostTools) {
  std::string tools_dir = GetAndroidHostToolsDir();
  if (tools_dir.empty()) {
    ADD_FAILURE() << "Cannot find Android tools directory for host";
  } else {
    CheckToolsExist(tools_dir);
  }
}

TEST_F(PrebuiltToolsTest, CheckTargetTools) {
  // Other prebuilts are missing from the build server's repo manifest.
  InstructionSet isas[] = { kThumb2 };  // NOLINT
  for (InstructionSet isa : isas) {
    std::string tools_dir = GetAndroidTargetToolsDir(isa);
    if (tools_dir.empty()) {
      ADD_FAILURE() << "Cannot find Android tools directory for " << isa;
    } else {
      CheckToolsExist(tools_dir);
    }
  }
}

#endif  // __ANDROID__

}  // namespace art
