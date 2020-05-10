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

#include "scoped_flock.h"

#include "common_runtime_test.h"

namespace art {

class ScopedFlockTest : public CommonRuntimeTest {};

TEST_F(ScopedFlockTest, TestLocking) {
  ScratchFile scratch_file;
  std::string error_msg;

  // NOTE: Locks applied using flock(2) and fcntl(2) are oblivious
  // to each other, so attempting to query locks set by flock using
  // using fcntl(,F_GETLK,) will not work. see kernel doc at
  // Documentation/filesystems/locks.txt.
  ScopedFlock file_lock;
  ASSERT_TRUE(file_lock.Init(scratch_file.GetFilename().c_str(),
                             &error_msg));

  ASSERT_FALSE(file_lock.Init("/guaranteed/not/to/exist", &error_msg));
}

}  // namespace art
