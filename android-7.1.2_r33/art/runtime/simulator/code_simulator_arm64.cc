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

#include "simulator/code_simulator_arm64.h"

namespace art {
namespace arm64 {

// VIXL has not been tested on 32bit architectures, so vixl::Simulator is not always
// available. To avoid linker error on these architectures, we check if we can simulate
// in the beginning of following methods, with compile time constant `kCanSimulate`.
// TODO: when vixl::Simulator is always available, remove the these checks.

CodeSimulatorArm64* CodeSimulatorArm64::CreateCodeSimulatorArm64() {
  if (kCanSimulate) {
    return new CodeSimulatorArm64();
  } else {
    return nullptr;
  }
}

CodeSimulatorArm64::CodeSimulatorArm64()
    : CodeSimulator(), decoder_(nullptr), simulator_(nullptr) {
  DCHECK(kCanSimulate);
  decoder_ = new vixl::Decoder();
  simulator_ = new vixl::Simulator(decoder_);
}

CodeSimulatorArm64::~CodeSimulatorArm64() {
  DCHECK(kCanSimulate);
  delete simulator_;
  delete decoder_;
}

void CodeSimulatorArm64::RunFrom(intptr_t code_buffer) {
  DCHECK(kCanSimulate);
  simulator_->RunFrom(reinterpret_cast<const vixl::Instruction*>(code_buffer));
}

bool CodeSimulatorArm64::GetCReturnBool() const {
  DCHECK(kCanSimulate);
  return simulator_->wreg(0);
}

int32_t CodeSimulatorArm64::GetCReturnInt32() const {
  DCHECK(kCanSimulate);
  return simulator_->wreg(0);
}

int64_t CodeSimulatorArm64::GetCReturnInt64() const {
  DCHECK(kCanSimulate);
  return simulator_->xreg(0);
}

}  // namespace arm64
}  // namespace art
