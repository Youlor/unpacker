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

#include "memcmp16.h"

// This linked against by assembly stubs, only.
#pragma GCC diagnostic ignored "-Wunused-function"

int32_t memcmp16_generic_static(const uint16_t* s0, const uint16_t* s1, size_t count);
int32_t memcmp16_generic_static(const uint16_t* s0, const uint16_t* s1, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (s0[i] != s1[i]) {
      return static_cast<int32_t>(s0[i]) - static_cast<int32_t>(s1[i]);
    }
  }
  return 0;
}

namespace art {

namespace testing {

int32_t MemCmp16Testing(const uint16_t* s0, const uint16_t* s1, size_t count) {
  return MemCmp16(s0, s1, count);
}

}

}  // namespace art

#pragma GCC diagnostic warning "-Wunused-function"
