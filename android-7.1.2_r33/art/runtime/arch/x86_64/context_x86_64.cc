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

#include "context_x86_64.h"

#include "base/bit_utils.h"
#include "quick/quick_method_frame_info.h"

namespace art {
namespace x86_64 {

static constexpr uintptr_t gZero = 0;

void X86_64Context::Reset() {
  std::fill_n(gprs_, arraysize(gprs_), nullptr);
  std::fill_n(fprs_, arraysize(fprs_), nullptr);
  gprs_[RSP] = &rsp_;
  gprs_[RDI] = &arg0_;
  // Initialize registers with easy to spot debug values.
  rsp_ = X86_64Context::kBadGprBase + RSP;
  rip_ = X86_64Context::kBadGprBase + kNumberOfCpuRegisters;
  arg0_ = 0;
}

void X86_64Context::FillCalleeSaves(uint8_t* frame, const QuickMethodFrameInfo& frame_info) {
  int spill_pos = 0;

  // Core registers come first, from the highest down to the lowest.
  uint32_t core_regs =
      frame_info.CoreSpillMask() & ~(static_cast<uint32_t>(-1) << kNumberOfCpuRegisters);
  DCHECK_EQ(1, POPCOUNT(frame_info.CoreSpillMask() & ~core_regs));  // Return address spill.
  for (uint32_t core_reg : HighToLowBits(core_regs)) {
    gprs_[core_reg] = CalleeSaveAddress(frame, spill_pos, frame_info.FrameSizeInBytes());
    ++spill_pos;
  }
  DCHECK_EQ(spill_pos, POPCOUNT(frame_info.CoreSpillMask()) - 1);

  // FP registers come second, from the highest down to the lowest.
  uint32_t fp_regs = frame_info.FpSpillMask();
  DCHECK_EQ(0u, fp_regs & (static_cast<uint32_t>(-1) << kNumberOfFloatRegisters));
  for (uint32_t fp_reg : HighToLowBits(fp_regs)) {
    fprs_[fp_reg] = reinterpret_cast<uint64_t*>(
        CalleeSaveAddress(frame, spill_pos, frame_info.FrameSizeInBytes()));
    ++spill_pos;
  }
  DCHECK_EQ(spill_pos,
            POPCOUNT(frame_info.CoreSpillMask()) - 1 + POPCOUNT(frame_info.FpSpillMask()));
}

void X86_64Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[RAX] = const_cast<uintptr_t*>(&gZero);
  gprs_[RDX] = const_cast<uintptr_t*>(&gZero);
  gprs_[RCX] = nullptr;
  gprs_[RSI] = nullptr;
  gprs_[RDI] = nullptr;
  gprs_[R8] = nullptr;
  gprs_[R9] = nullptr;
  gprs_[R10] = nullptr;
  gprs_[R11] = nullptr;
  fprs_[XMM0] = nullptr;
  fprs_[XMM1] = nullptr;
  fprs_[XMM2] = nullptr;
  fprs_[XMM3] = nullptr;
  fprs_[XMM4] = nullptr;
  fprs_[XMM5] = nullptr;
  fprs_[XMM6] = nullptr;
  fprs_[XMM7] = nullptr;
  fprs_[XMM8] = nullptr;
  fprs_[XMM9] = nullptr;
  fprs_[XMM10] = nullptr;
  fprs_[XMM11] = nullptr;
}

void X86_64Context::SetGPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfCpuRegisters));
  DCHECK(IsAccessibleGPR(reg));
  CHECK_NE(gprs_[reg], &gZero);
  *gprs_[reg] = value;
}

void X86_64Context::SetFPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfFloatRegisters));
  DCHECK(IsAccessibleFPR(reg));
  CHECK_NE(fprs_[reg], reinterpret_cast<const uint64_t*>(&gZero));
  *fprs_[reg] = value;
}

extern "C" NO_RETURN void art_quick_do_long_jump(uintptr_t*, uintptr_t*);

void X86_64Context::DoLongJump() {
#if defined(__x86_64__)
  uintptr_t gprs[kNumberOfCpuRegisters + 1];
  uintptr_t fprs[kNumberOfFloatRegisters];

  for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    gprs[kNumberOfCpuRegisters - i - 1] = gprs_[i] != nullptr ? *gprs_[i] : X86_64Context::kBadGprBase + i;
  }
  for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : X86_64Context::kBadFprBase + i;
  }

  // We want to load the stack pointer one slot below so that the ret will pop eip.
  uintptr_t rsp = gprs[kNumberOfCpuRegisters - RSP - 1] - sizeof(intptr_t);
  gprs[kNumberOfCpuRegisters] = rsp;
  *(reinterpret_cast<uintptr_t*>(rsp)) = rip_;

  art_quick_do_long_jump(gprs, fprs);
#else
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
#endif
}

}  // namespace x86_64
}  // namespace art
