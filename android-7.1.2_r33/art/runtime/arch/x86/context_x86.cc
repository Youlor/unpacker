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

#include "context_x86.h"

#include "base/bit_utils.h"
#include "quick/quick_method_frame_info.h"

namespace art {
namespace x86 {

static constexpr uintptr_t gZero = 0;

void X86Context::Reset() {
  std::fill_n(gprs_, arraysize(gprs_), nullptr);
  std::fill_n(fprs_, arraysize(fprs_), nullptr);
  gprs_[ESP] = &esp_;
  gprs_[EAX] = &arg0_;
  // Initialize registers with easy to spot debug values.
  esp_ = X86Context::kBadGprBase + ESP;
  eip_ = X86Context::kBadGprBase + kNumberOfCpuRegisters;
  arg0_ = 0;
}

void X86Context::FillCalleeSaves(uint8_t* frame, const QuickMethodFrameInfo& frame_info) {
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
    // Two void* per XMM register.
    fprs_[2 * fp_reg] = reinterpret_cast<uint32_t*>(
        CalleeSaveAddress(frame, spill_pos + 1, frame_info.FrameSizeInBytes()));
    fprs_[2 * fp_reg + 1] = reinterpret_cast<uint32_t*>(
        CalleeSaveAddress(frame, spill_pos, frame_info.FrameSizeInBytes()));
    spill_pos += 2;
  }
  DCHECK_EQ(spill_pos,
            POPCOUNT(frame_info.CoreSpillMask()) - 1 + 2 * POPCOUNT(frame_info.FpSpillMask()));
}

void X86Context::SmashCallerSaves() {
  // This needs to be 0 because we want a null/zero return value.
  gprs_[EAX] = const_cast<uintptr_t*>(&gZero);
  gprs_[EDX] = const_cast<uintptr_t*>(&gZero);
  gprs_[ECX] = nullptr;
  gprs_[EBX] = nullptr;
  memset(&fprs_[0], '\0', sizeof(fprs_));
}

void X86Context::SetGPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfCpuRegisters));
  DCHECK(IsAccessibleGPR(reg));
  CHECK_NE(gprs_[reg], &gZero);
  *gprs_[reg] = value;
}

void X86Context::SetFPR(uint32_t reg, uintptr_t value) {
  CHECK_LT(reg, static_cast<uint32_t>(kNumberOfFloatRegisters));
  DCHECK(IsAccessibleFPR(reg));
  CHECK_NE(fprs_[reg], reinterpret_cast<const uint32_t*>(&gZero));
  *fprs_[reg] = value;
}

void X86Context::DoLongJump() {
#if defined(__i386__)
  // Array of GPR values, filled from the context backward for the long jump pop. We add a slot at
  // the top for the stack pointer that doesn't get popped in a pop-all.
  volatile uintptr_t gprs[kNumberOfCpuRegisters + 1];
  for (size_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    gprs[kNumberOfCpuRegisters - i - 1] = gprs_[i] != nullptr ? *gprs_[i] : X86Context::kBadGprBase + i;
  }
  uint32_t fprs[kNumberOfFloatRegisters];
  for (size_t i = 0; i < kNumberOfFloatRegisters; ++i) {
    fprs[i] = fprs_[i] != nullptr ? *fprs_[i] : X86Context::kBadFprBase + i;
  }
  // We want to load the stack pointer one slot below so that the ret will pop eip.
  uintptr_t esp = gprs[kNumberOfCpuRegisters - ESP - 1] - sizeof(intptr_t);
  gprs[kNumberOfCpuRegisters] = esp;
  *(reinterpret_cast<uintptr_t*>(esp)) = eip_;
  __asm__ __volatile__(
      "movl %1, %%ebx\n\t"          // Address base of FPRs.
      "movsd 0(%%ebx), %%xmm0\n\t"  // Load up XMM0-XMM7.
      "movsd 8(%%ebx), %%xmm1\n\t"
      "movsd 16(%%ebx), %%xmm2\n\t"
      "movsd 24(%%ebx), %%xmm3\n\t"
      "movsd 32(%%ebx), %%xmm4\n\t"
      "movsd 40(%%ebx), %%xmm5\n\t"
      "movsd 48(%%ebx), %%xmm6\n\t"
      "movsd 56(%%ebx), %%xmm7\n\t"
      "movl %0, %%esp\n\t"  // ESP points to gprs.
      "popal\n\t"           // Load all registers except ESP and EIP with values in gprs.
      "popl %%esp\n\t"      // Load stack pointer.
      "ret\n\t"             // From higher in the stack pop eip.
      :  // output.
      : "g"(&gprs[0]), "g"(&fprs[0]) // input.
      :);  // clobber.
#else
  UNIMPLEMENTED(FATAL);
#endif
  UNREACHABLE();
}

}  // namespace x86
}  // namespace art
