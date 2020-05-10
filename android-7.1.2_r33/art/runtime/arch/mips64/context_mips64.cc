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

#include "context_mips64.h"

#include "base/bit_utils.h"
#include "quick/quick_method_frame_info.h"

namespace art {
namespace mips64 {

static constexpr uintptr_t gZero = 0;

void Mips64Context::Reset() {
  std::fill_n(gprs_, arraysize(gprs_), nullptr);
  std::fill_n(fprs_, arraysize(fprs_), nullptr);
  gprs_[SP] = &sp_;
  gprs_[T9] = &t9_;
  gprs_[A0] = &arg0_;
  // Initialize registers with easy to spot debug values.
  sp_ = Mips64Context::kBadGprBase + SP;
  t9_ = Mips64Context::kBadGprBase + T9;
  arg0_ = 0;
}

void Mips64Context::FillCalleeSaves(uint8_t* frame, const QuickMethodFrameInfo& frame_info) {
  int spill_pos = 0;

  // Core registers come first, from the highest down to the lowest.
  for (uint32_t core_reg : HighToLowBits(frame_info.CoreSpillMask())) {
    gprs_[core_reg] = CalleeSaveAddress(frame, spill_pos, frame_info.FrameSizeInBytes());
    ++spill_pos;
  }
  DCHECK_EQ(spill_pos, POPCOUNT(frame_info.CoreSpillMask()));

  // FP registers come second, from the highest down to the lowest.
  for (uint32_t fp_reg : HighToLowBits(frame_info.FpSpillMask())) {
    fprs_[fp_reg] = CalleeSaveAddress(frame, spill_pos, frame_info.FrameSizeInBytes());
    ++spill_pos;
  }
  DCHECK_EQ(spill_pos, POPCOUNT(frame_info.CoreSpillMask()) + POPCOUNT(frame_info.FpSpillMask()));
}

void Mips64Context::SetGPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfGpuRegisters));
  DCHECK(IsAccessibleGPR(reg));
  CHECK_NE(gprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  *gprs_[reg] = value;
}

void Mips64Context::SetFPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfFpuRegisters));
  DCHECK(IsAccessibleFPR(reg));
  CHECK_NE(fprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  *fprs_[reg] = value;
}

void Mips64Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[V0] = const_cast<uintptr_t*>(&gZero);
  gprs_[V1] = const_cast<uintptr_t*>(&gZero);
  gprs_[A1] = nullptr;
  gprs_[A0] = nullptr;
  gprs_[A2] = nullptr;
  gprs_[A3] = nullptr;
  gprs_[A4] = nullptr;
  gprs_[A5] = nullptr;
  gprs_[A6] = nullptr;
  gprs_[A7] = nullptr;

  // f0-f23 are caller-saved; f24-f31 are callee-saved.
  fprs_[F0] = nullptr;
  fprs_[F1] = nullptr;
  fprs_[F2] = nullptr;
  fprs_[F3] = nullptr;
  fprs_[F4] = nullptr;
  fprs_[F5] = nullptr;
  fprs_[F6] = nullptr;
  fprs_[F7] = nullptr;
  fprs_[F8] = nullptr;
  fprs_[F9] = nullptr;
  fprs_[F10] = nullptr;
  fprs_[F11] = nullptr;
  fprs_[F12] = nullptr;
  fprs_[F13] = nullptr;
  fprs_[F14] = nullptr;
  fprs_[F15] = nullptr;
  fprs_[F16] = nullptr;
  fprs_[F17] = nullptr;
  fprs_[F18] = nullptr;
  fprs_[F19] = nullptr;
  fprs_[F20] = nullptr;
  fprs_[F21] = nullptr;
  fprs_[F22] = nullptr;
  fprs_[F23] = nullptr;
}

extern "C" NO_RETURN void art_quick_do_long_jump(uintptr_t*, uintptr_t*);

void Mips64Context::DoLongJump() {
  uintptr_t gprs[kNumberOfGpuRegisters];
  uintptr_t fprs[kNumberOfFpuRegisters];
  for (size_t i = 0; i < kNumberOfGpuRegisters; ++i) {
    gprs[i] = gprs_[i] != nullptr ? *gprs_[i] : Mips64Context::kBadGprBase + i;
  }
  for (size_t i = 0; i < kNumberOfFpuRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : Mips64Context::kBadFprBase + i;
  }
  art_quick_do_long_jump(gprs, fprs);
}

}  // namespace mips64
}  // namespace art
