/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "context_arm.h"

#include "base/bit_utils.h"
#include "quick/quick_method_frame_info.h"
#include "thread-inl.h"

namespace art {
namespace arm {

static constexpr uint32_t gZero = 0;

void ArmContext::Reset() {
  std::fill_n(gprs_, arraysize(gprs_), nullptr);
  std::fill_n(fprs_, arraysize(fprs_), nullptr);
  gprs_[SP] = &sp_;
  gprs_[PC] = &pc_;
  gprs_[R0] = &arg0_;
  // Initialize registers with easy to spot debug values.
  sp_ = ArmContext::kBadGprBase + SP;
  pc_ = ArmContext::kBadGprBase + PC;
  arg0_ = 0;
}

void ArmContext::FillCalleeSaves(uint8_t* frame, const QuickMethodFrameInfo& frame_info) {
  int spill_pos = 0;

  // Core registers come first, from the highest down to the lowest.
  uint32_t core_regs = frame_info.CoreSpillMask();
  DCHECK_EQ(0u, core_regs & (static_cast<uint32_t>(-1) << kNumberOfCoreRegisters));
  for (uint32_t core_reg : HighToLowBits(core_regs)) {
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

void ArmContext::SetGPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfCoreRegisters));
  DCHECK(IsAccessibleGPR(reg));
  DCHECK_NE(gprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  *gprs_[reg] = value;
}

void ArmContext::SetFPR(uint32_t reg, uintptr_t value) {
  DCHECK_LT(reg, static_cast<uint32_t>(kNumberOfSRegisters));
  DCHECK(IsAccessibleFPR(reg));
  DCHECK_NE(fprs_[reg], &gZero);  // Can't overwrite this static value since they are never reset.
  *fprs_[reg] = value;
}

void ArmContext::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[R0] = const_cast<uint32_t*>(&gZero);
  gprs_[R1] = const_cast<uint32_t*>(&gZero);
  gprs_[R2] = nullptr;
  gprs_[R3] = nullptr;

  fprs_[S0] = nullptr;
  fprs_[S1] = nullptr;
  fprs_[S2] = nullptr;
  fprs_[S3] = nullptr;
  fprs_[S4] = nullptr;
  fprs_[S5] = nullptr;
  fprs_[S6] = nullptr;
  fprs_[S7] = nullptr;
  fprs_[S8] = nullptr;
  fprs_[S9] = nullptr;
  fprs_[S10] = nullptr;
  fprs_[S11] = nullptr;
  fprs_[S12] = nullptr;
  fprs_[S13] = nullptr;
  fprs_[S14] = nullptr;
  fprs_[S15] = nullptr;
}

extern "C" NO_RETURN void art_quick_do_long_jump(uint32_t*, uint32_t*);

void ArmContext::DoLongJump() {
  uintptr_t gprs[kNumberOfCoreRegisters];
  uint32_t fprs[kNumberOfSRegisters];
  for (size_t i = 0; i < kNumberOfCoreRegisters; ++i) {
    gprs[i] = gprs_[i] != nullptr ? *gprs_[i] : ArmContext::kBadGprBase + i;
  }
  for (size_t i = 0; i < kNumberOfSRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : ArmContext::kBadFprBase + i;
  }
  DCHECK_EQ(reinterpret_cast<uintptr_t>(Thread::Current()), gprs[TR]);
  art_quick_do_long_jump(gprs, fprs);
}

}  // namespace arm
}  // namespace art
