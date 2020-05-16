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

#include "registers_arm64.h"

#include <ostream>

namespace art {
namespace arm64 {

static const char* kRegisterNames[] = {
  "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
  "x10", "x11", "x12", "x13", "x14", "x15", "ip0", "ip1", "x18", "x19",
  "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "fp",
  "lr", "sp", "xzr"
};

static const char* kWRegisterNames[] = {
  "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7", "w8", "w9",
  "w10", "w11", "w12", "w13", "w14", "w15", "w16", "w17", "w18", "w19",
  "w20", "w21", "w22", "w23", "w24", "w25", "w26", "w27", "w28", "w29",
  "w30", "wsp", "wzr"
};

std::ostream& operator<<(std::ostream& os, const XRegister& rhs) {
  if (rhs >= X0 && rhs < kNumberOfXRegisters) {
    os << kRegisterNames[rhs];
  } else {
    os << "XRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const WRegister& rhs) {
  if (rhs >= W0 && rhs < kNumberOfWRegisters) {
    os << kWRegisterNames[rhs];
  } else {
    os << "WRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const DRegister& rhs) {
  if (rhs >= D0 && rhs < kNumberOfDRegisters) {
    os << "d" << static_cast<int>(rhs);
  } else {
    os << "DRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const SRegister& rhs) {
  if (rhs >= S0 && rhs < kNumberOfSRegisters) {
    os << "s" << static_cast<int>(rhs);
  } else {
    os << "SRegister[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

}  // namespace arm64
}  // namespace art
