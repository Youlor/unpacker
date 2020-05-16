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

#include "registers_mips64.h"

#include <ostream>

namespace art {
namespace mips64 {

static const char* kRegisterNames[] = {
  "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
};

std::ostream& operator<<(std::ostream& os, const GpuRegister& rhs) {
  if (rhs >= ZERO && rhs < kNumberOfGpuRegisters) {
    os << kRegisterNames[rhs];
  } else {
    os << "GpuRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const FpuRegister& rhs) {
  if (rhs >= F0 && rhs < kNumberOfFpuRegisters) {
    os << "f" << static_cast<int>(rhs);
  } else {
    os << "FpuRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

}  // namespace mips64
}  // namespace art
