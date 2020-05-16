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

#include <cstdio>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "linear_alloc.h"
#include "mirror/class-inl.h"
#include "mirror/string-inl.h"
#include "scoped_thread_state_change.h"

namespace art {


class StubTest : public CommonRuntimeTest {
 protected:
  // We need callee-save methods set up in the Runtime for exceptions.
  void SetUp() OVERRIDE {
    // Do the normal setup.
    CommonRuntimeTest::SetUp();

    {
      // Create callee-save methods
      ScopedObjectAccess soa(Thread::Current());
      runtime_->SetInstructionSet(kRuntimeISA);
      for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
        Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
        if (!runtime_->HasCalleeSaveMethod(type)) {
          runtime_->SetCalleeSaveMethod(runtime_->CreateCalleeSaveMethod(), type);
        }
      }
    }
  }

  void SetUpRuntimeOptions(RuntimeOptions *options) OVERRIDE {
    // Use a smaller heap
    for (std::pair<std::string, const void*>& pair : *options) {
      if (pair.first.find("-Xmx") == 0) {
        pair.first = "-Xmx4M";  // Smallest we can go.
      }
    }
    options->push_back(std::make_pair("-Xint", nullptr));
  }

  // Helper function needed since TEST_F makes a new class.
  Thread::tls_ptr_sized_values* GetTlsPtr(Thread* self) {
    return &self->tlsPtr_;
  }

 public:
  size_t Invoke3(size_t arg0, size_t arg1, size_t arg2, uintptr_t code, Thread* self) {
    return Invoke3WithReferrer(arg0, arg1, arg2, code, self, nullptr);
  }

  // TODO: Set up a frame according to referrer's specs.
  size_t Invoke3WithReferrer(size_t arg0, size_t arg1, size_t arg2, uintptr_t code, Thread* self,
                             ArtMethod* referrer) {
    return Invoke3WithReferrerAndHidden(arg0, arg1, arg2, code, self, referrer, 0);
  }

  // TODO: Set up a frame according to referrer's specs.
  size_t Invoke3WithReferrerAndHidden(size_t arg0, size_t arg1, size_t arg2, uintptr_t code,
                                      Thread* self, ArtMethod* referrer, size_t hidden) {
    // Push a transition back into managed code onto the linked list in thread.
    ManagedStack fragment;
    self->PushManagedStackFragment(&fragment);

    size_t result;
    size_t fpr_result = 0;
#if defined(__i386__)
    // TODO: Set the thread?
#define PUSH(reg) "push " # reg "\n\t .cfi_adjust_cfa_offset 4\n\t"
#define POP(reg) "pop " # reg "\n\t .cfi_adjust_cfa_offset -4\n\t"
    __asm__ __volatile__(
        "movd %[hidden], %%xmm7\n\t"  // This is a memory op, so do this early. If it is off of
                                      // esp, then we won't be able to access it after spilling.

        // Spill 6 registers.
        PUSH(%%ebx)
        PUSH(%%ecx)
        PUSH(%%edx)
        PUSH(%%esi)
        PUSH(%%edi)
        PUSH(%%ebp)

        // Store the inputs to the stack, but keep the referrer up top, less work.
        PUSH(%[referrer])           // Align stack.
        PUSH(%[referrer])           // Store referrer

        PUSH(%[arg0])
        PUSH(%[arg1])
        PUSH(%[arg2])
        PUSH(%[code])
        // Now read them back into the required registers.
        POP(%%edi)
        POP(%%edx)
        POP(%%ecx)
        POP(%%eax)
        // Call is prepared now.

        "call *%%edi\n\t"           // Call the stub
        "addl $8, %%esp\n\t"        // Pop referrer and padding.
        ".cfi_adjust_cfa_offset -8\n\t"

        // Restore 6 registers.
        POP(%%ebp)
        POP(%%edi)
        POP(%%esi)
        POP(%%edx)
        POP(%%ecx)
        POP(%%ebx)

        : "=a" (result)
          // Use the result from eax
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code),
          [referrer]"r"(referrer), [hidden]"m"(hidden)
          // This places code into edi, arg0 into eax, arg1 into ecx, and arg2 into edx
        : "memory", "xmm7");  // clobber.
#undef PUSH
#undef POP
#elif defined(__arm__)
    __asm__ __volatile__(
        "push {r1-r12, lr}\n\t"     // Save state, 13*4B = 52B
        ".cfi_adjust_cfa_offset 52\n\t"
        "push {r9}\n\t"
        ".cfi_adjust_cfa_offset 4\n\t"
        "mov r9, %[referrer]\n\n"
        "str r9, [sp, #-8]!\n\t"   // Push referrer, +8B padding so 16B aligned
        ".cfi_adjust_cfa_offset 8\n\t"
        "ldr r9, [sp, #8]\n\t"

        // Push everything on the stack, so we don't rely on the order. What a mess. :-(
        "sub sp, sp, #24\n\t"
        "str %[arg0], [sp]\n\t"
        "str %[arg1], [sp, #4]\n\t"
        "str %[arg2], [sp, #8]\n\t"
        "str %[code], [sp, #12]\n\t"
        "str %[self], [sp, #16]\n\t"
        "str %[hidden], [sp, #20]\n\t"
        "ldr r0, [sp]\n\t"
        "ldr r1, [sp, #4]\n\t"
        "ldr r2, [sp, #8]\n\t"
        "ldr r3, [sp, #12]\n\t"
        "ldr r9, [sp, #16]\n\t"
        "ldr r12, [sp, #20]\n\t"
        "add sp, sp, #24\n\t"

        "blx r3\n\t"                // Call the stub
        "add sp, sp, #12\n\t"       // Pop null and padding
        ".cfi_adjust_cfa_offset -12\n\t"
        "pop {r1-r12, lr}\n\t"      // Restore state
        ".cfi_adjust_cfa_offset -52\n\t"
        "mov %[result], r0\n\t"     // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self),
          [referrer] "r"(referrer), [hidden] "r"(hidden)
        : "r0", "memory");  // clobber.
#elif defined(__aarch64__)
    __asm__ __volatile__(
        // Spill x0-x7 which we say we don't clobber. May contain args.
        "sub sp, sp, #80\n\t"
        ".cfi_adjust_cfa_offset 80\n\t"
        "stp x0, x1, [sp]\n\t"
        "stp x2, x3, [sp, #16]\n\t"
        "stp x4, x5, [sp, #32]\n\t"
        "stp x6, x7, [sp, #48]\n\t"
        // To be extra defensive, store x20. We do this because some of the stubs might make a
        // transition into the runtime via the blr instruction below and *not* save x20.
        "str x20, [sp, #64]\n\t"
        // 8 byte buffer

        "sub sp, sp, #16\n\t"          // Reserve stack space, 16B aligned
        ".cfi_adjust_cfa_offset 16\n\t"
        "str %[referrer], [sp]\n\t"    // referrer

        // Push everything on the stack, so we don't rely on the order. What a mess. :-(
        "sub sp, sp, #48\n\t"
        ".cfi_adjust_cfa_offset 48\n\t"
        // All things are "r" constraints, so direct str/stp should work.
        "stp %[arg0], %[arg1], [sp]\n\t"
        "stp %[arg2], %[code], [sp, #16]\n\t"
        "stp %[self], %[hidden], [sp, #32]\n\t"

        // Now we definitely have x0-x3 free, use it to garble d8 - d15
        "movk x0, #0xfad0\n\t"
        "movk x0, #0xebad, lsl #16\n\t"
        "movk x0, #0xfad0, lsl #32\n\t"
        "movk x0, #0xebad, lsl #48\n\t"
        "fmov d8, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d9, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d10, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d11, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d12, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d13, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d14, x0\n\t"
        "add x0, x0, 1\n\t"
        "fmov d15, x0\n\t"

        // Load call params into the right registers.
        "ldp x0, x1, [sp]\n\t"
        "ldp x2, x3, [sp, #16]\n\t"
        "ldp x19, x17, [sp, #32]\n\t"
        "add sp, sp, #48\n\t"
        ".cfi_adjust_cfa_offset -48\n\t"

        "blr x3\n\t"              // Call the stub
        "mov x8, x0\n\t"          // Store result
        "add sp, sp, #16\n\t"     // Drop the quick "frame"
        ".cfi_adjust_cfa_offset -16\n\t"

        // Test d8 - d15. We can use x1 and x2.
        "movk x1, #0xfad0\n\t"
        "movk x1, #0xebad, lsl #16\n\t"
        "movk x1, #0xfad0, lsl #32\n\t"
        "movk x1, #0xebad, lsl #48\n\t"
        "fmov x2, d8\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d9\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d10\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d11\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d12\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d13\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d14\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"
        "add x1, x1, 1\n\t"

        "fmov x2, d15\n\t"
        "cmp x1, x2\n\t"
        "b.ne 1f\n\t"

        "mov x9, #0\n\t"              // Use x9 as flag, in clobber list

        // Finish up.
        "2:\n\t"
        "ldp x0, x1, [sp]\n\t"        // Restore stuff not named clobbered, may contain fpr_result
        "ldp x2, x3, [sp, #16]\n\t"
        "ldp x4, x5, [sp, #32]\n\t"
        "ldp x6, x7, [sp, #48]\n\t"
        "ldr x20, [sp, #64]\n\t"
        "add sp, sp, #80\n\t"         // Free stack space, now sp as on entry
        ".cfi_adjust_cfa_offset -80\n\t"

        "str x9, %[fpr_result]\n\t"   // Store the FPR comparison result
        "mov %[result], x8\n\t"              // Store the call result

        "b 3f\n\t"                     // Goto end

        // Failed fpr verification.
        "1:\n\t"
        "mov x9, #1\n\t"
        "b 2b\n\t"                     // Goto finish-up

        // End
        "3:\n\t"
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "0"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self),
          [referrer] "r"(referrer), [hidden] "r"(hidden), [fpr_result] "m" (fpr_result)
          // Leave one register unclobbered, which is needed for compiling with
          // -fstack-protector-strong. According to AAPCS64 registers x9-x15 are caller-saved,
          // which means we should unclobber one of the callee-saved registers that are unused.
          // Here we use x20.
        : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
          "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x30",
          "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
          "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
          "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
          "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
          "memory");
#elif defined(__mips__) && !defined(__LP64__)
    __asm__ __volatile__ (
        // Spill a0-a3 and t0-t7 which we say we don't clobber. May contain args.
        "addiu $sp, $sp, -64\n\t"
        "sw $a0, 0($sp)\n\t"
        "sw $a1, 4($sp)\n\t"
        "sw $a2, 8($sp)\n\t"
        "sw $a3, 12($sp)\n\t"
        "sw $t0, 16($sp)\n\t"
        "sw $t1, 20($sp)\n\t"
        "sw $t2, 24($sp)\n\t"
        "sw $t3, 28($sp)\n\t"
        "sw $t4, 32($sp)\n\t"
        "sw $t5, 36($sp)\n\t"
        "sw $t6, 40($sp)\n\t"
        "sw $t7, 44($sp)\n\t"
        // Spill gp register since it is caller save.
        "sw $gp, 52($sp)\n\t"

        "addiu $sp, $sp, -16\n\t"  // Reserve stack space, 16B aligned.
        "sw %[referrer], 0($sp)\n\t"

        // Push everything on the stack, so we don't rely on the order.
        "addiu $sp, $sp, -24\n\t"
        "sw %[arg0], 0($sp)\n\t"
        "sw %[arg1], 4($sp)\n\t"
        "sw %[arg2], 8($sp)\n\t"
        "sw %[code], 12($sp)\n\t"
        "sw %[self], 16($sp)\n\t"
        "sw %[hidden], 20($sp)\n\t"

        // Load call params into the right registers.
        "lw $a0, 0($sp)\n\t"
        "lw $a1, 4($sp)\n\t"
        "lw $a2, 8($sp)\n\t"
        "lw $t9, 12($sp)\n\t"
        "lw $s1, 16($sp)\n\t"
        "lw $t0, 20($sp)\n\t"
        "addiu $sp, $sp, 24\n\t"

        "jalr $t9\n\t"             // Call the stub.
        "nop\n\t"
        "addiu $sp, $sp, 16\n\t"   // Drop the quick "frame".

        // Restore stuff not named clobbered.
        "lw $a0, 0($sp)\n\t"
        "lw $a1, 4($sp)\n\t"
        "lw $a2, 8($sp)\n\t"
        "lw $a3, 12($sp)\n\t"
        "lw $t0, 16($sp)\n\t"
        "lw $t1, 20($sp)\n\t"
        "lw $t2, 24($sp)\n\t"
        "lw $t3, 28($sp)\n\t"
        "lw $t4, 32($sp)\n\t"
        "lw $t5, 36($sp)\n\t"
        "lw $t6, 40($sp)\n\t"
        "lw $t7, 44($sp)\n\t"
        // Restore gp.
        "lw $gp, 52($sp)\n\t"
        "addiu $sp, $sp, 64\n\t"   // Free stack space, now sp as on entry.

        "move %[result], $v0\n\t"  // Store the call result.
        : [result] "=r" (result)
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self),
          [referrer] "r"(referrer), [hidden] "r"(hidden)
        : "at", "v0", "v1", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1",
          "fp", "ra",
          "$f0", "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7", "$f8", "$f9", "$f10", "$f11",
          "$f12", "$f13", "$f14", "$f15", "$f16", "$f17", "$f18", "$f19", "$f20", "$f21", "$f22",
          "$f23", "$f24", "$f25", "$f26", "$f27", "$f28", "$f29", "$f30", "$f31",
          "memory");  // clobber.
#elif defined(__mips__) && defined(__LP64__)
    __asm__ __volatile__ (
        // Spill a0-a7 which we say we don't clobber. May contain args.
        "daddiu $sp, $sp, -64\n\t"
        "sd $a0, 0($sp)\n\t"
        "sd $a1, 8($sp)\n\t"
        "sd $a2, 16($sp)\n\t"
        "sd $a3, 24($sp)\n\t"
        "sd $a4, 32($sp)\n\t"
        "sd $a5, 40($sp)\n\t"
        "sd $a6, 48($sp)\n\t"
        "sd $a7, 56($sp)\n\t"

        "daddiu $sp, $sp, -16\n\t"  // Reserve stack space, 16B aligned.
        "sd %[referrer], 0($sp)\n\t"

        // Push everything on the stack, so we don't rely on the order.
        "daddiu $sp, $sp, -48\n\t"
        "sd %[arg0], 0($sp)\n\t"
        "sd %[arg1], 8($sp)\n\t"
        "sd %[arg2], 16($sp)\n\t"
        "sd %[code], 24($sp)\n\t"
        "sd %[self], 32($sp)\n\t"
        "sd %[hidden], 40($sp)\n\t"

        // Load call params into the right registers.
        "ld $a0, 0($sp)\n\t"
        "ld $a1, 8($sp)\n\t"
        "ld $a2, 16($sp)\n\t"
        "ld $t9, 24($sp)\n\t"
        "ld $s1, 32($sp)\n\t"
        "ld $t0, 40($sp)\n\t"
        "daddiu $sp, $sp, 48\n\t"

        "jalr $t9\n\t"              // Call the stub.
        "nop\n\t"
        "daddiu $sp, $sp, 16\n\t"   // Drop the quick "frame".

        // Restore stuff not named clobbered.
        "ld $a0, 0($sp)\n\t"
        "ld $a1, 8($sp)\n\t"
        "ld $a2, 16($sp)\n\t"
        "ld $a3, 24($sp)\n\t"
        "ld $a4, 32($sp)\n\t"
        "ld $a5, 40($sp)\n\t"
        "ld $a6, 48($sp)\n\t"
        "ld $a7, 56($sp)\n\t"
        "daddiu $sp, $sp, 64\n\t"

        "move %[result], $v0\n\t"   // Store the call result.
        : [result] "=r" (result)
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self),
          [referrer] "r"(referrer), [hidden] "r"(hidden)
        // Instead aliases t0-t3, register names $12-$15 has been used in the clobber list because
        // t0-t3 are ambiguous.
        : "at", "v0", "v1", "$12", "$13", "$14", "$15", "s0", "s1", "s2", "s3", "s4", "s5", "s6",
          "s7", "t8", "t9", "k0", "k1", "fp", "ra",
          "$f0", "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7", "$f8", "$f9", "$f10", "$f11",
          "$f12", "$f13", "$f14", "$f15", "$f16", "$f17", "$f18", "$f19", "$f20", "$f21", "$f22",
          "$f23", "$f24", "$f25", "$f26", "$f27", "$f28", "$f29", "$f30", "$f31",
          "memory");  // clobber.
#elif defined(__x86_64__) && !defined(__APPLE__)
#define PUSH(reg) "pushq " # reg "\n\t .cfi_adjust_cfa_offset 8\n\t"
#define POP(reg) "popq " # reg "\n\t .cfi_adjust_cfa_offset -8\n\t"
    // Note: Uses the native convention. We do a callee-save regimen by manually spilling and
    //       restoring almost all registers.
    // TODO: Set the thread?
    __asm__ __volatile__(
        // Spill almost everything (except rax, rsp). 14 registers.
        PUSH(%%rbx)
        PUSH(%%rcx)
        PUSH(%%rdx)
        PUSH(%%rsi)
        PUSH(%%rdi)
        PUSH(%%rbp)
        PUSH(%%r8)
        PUSH(%%r9)
        PUSH(%%r10)
        PUSH(%%r11)
        PUSH(%%r12)
        PUSH(%%r13)
        PUSH(%%r14)
        PUSH(%%r15)

        PUSH(%[referrer])              // Push referrer & 16B alignment padding
        PUSH(%[referrer])

        // Now juggle the input registers.
        PUSH(%[arg0])
        PUSH(%[arg1])
        PUSH(%[arg2])
        PUSH(%[hidden])
        PUSH(%[code])
        POP(%%r8)
        POP(%%rax)
        POP(%%rdx)
        POP(%%rsi)
        POP(%%rdi)

        "call *%%r8\n\t"                  // Call the stub
        "addq $16, %%rsp\n\t"             // Pop null and padding
        ".cfi_adjust_cfa_offset -16\n\t"

        POP(%%r15)
        POP(%%r14)
        POP(%%r13)
        POP(%%r12)
        POP(%%r11)
        POP(%%r10)
        POP(%%r9)
        POP(%%r8)
        POP(%%rbp)
        POP(%%rdi)
        POP(%%rsi)
        POP(%%rdx)
        POP(%%rcx)
        POP(%%rbx)

        : "=a" (result)
        // Use the result from rax
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code),
          [referrer] "r"(referrer), [hidden] "r"(hidden)
        // This places arg0 into rdi, arg1 into rsi, arg2 into rdx, and code into some other
        // register. We can't use "b" (rbx), as ASAN uses this for the frame pointer.
        : "memory");  // We spill and restore (almost) all registers, so only mention memory here.
#undef PUSH
#undef POP
#else
    UNUSED(arg0, arg1, arg2, code, referrer, hidden);
    LOG(WARNING) << "Was asked to invoke for an architecture I do not understand.";
    result = 0;
#endif
    // Pop transition.
    self->PopManagedStackFragment(fragment);

    fp_result = fpr_result;
    EXPECT_EQ(0U, fp_result);

    return result;
  }

  static uintptr_t GetEntrypoint(Thread* self, QuickEntrypointEnum entrypoint) {
    int32_t offset;
#ifdef __LP64__
    offset = GetThreadOffset<8>(entrypoint).Int32Value();
#else
    offset = GetThreadOffset<4>(entrypoint).Int32Value();
#endif
    return *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(self) + offset);
  }

 protected:
  size_t fp_result;
};


TEST_F(StubTest, Memcpy) {
#if defined(__i386__) || (defined(__x86_64__) && !defined(__APPLE__)) || defined(__mips__)
  Thread* self = Thread::Current();

  uint32_t orig[20];
  uint32_t trg[20];
  for (size_t i = 0; i < 20; ++i) {
    orig[i] = i;
    trg[i] = 0;
  }

  Invoke3(reinterpret_cast<size_t>(&trg[4]), reinterpret_cast<size_t>(&orig[4]),
          10 * sizeof(uint32_t), StubTest::GetEntrypoint(self, kQuickMemcpy), self);

  EXPECT_EQ(orig[0], trg[0]);

  for (size_t i = 1; i < 4; ++i) {
    EXPECT_NE(orig[i], trg[i]);
  }

  for (size_t i = 4; i < 14; ++i) {
    EXPECT_EQ(orig[i], trg[i]);
  }

  for (size_t i = 14; i < 20; ++i) {
    EXPECT_NE(orig[i], trg[i]);
  }

  // TODO: Test overlapping?

#else
  LOG(INFO) << "Skipping memcpy as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping memcpy as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, LockObject) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  static constexpr size_t kThinLockLoops = 100;

  Thread* self = Thread::Current();

  const uintptr_t art_quick_lock_object = StubTest::GetEntrypoint(self, kQuickLockObject);

  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::String> obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));
  LockWord lock = obj->GetLockWord(false);
  LockWord::LockState old_state = lock.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, old_state);

  Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U, art_quick_lock_object, self);

  LockWord lock_after = obj->GetLockWord(false);
  LockWord::LockState new_state = lock_after.GetState();
  EXPECT_EQ(LockWord::LockState::kThinLocked, new_state);
  EXPECT_EQ(lock_after.ThinLockCount(), 0U);  // Thin lock starts count at zero

  for (size_t i = 1; i < kThinLockLoops; ++i) {
    Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U, art_quick_lock_object, self);

    // Check we're at lock count i

    LockWord l_inc = obj->GetLockWord(false);
    LockWord::LockState l_inc_state = l_inc.GetState();
    EXPECT_EQ(LockWord::LockState::kThinLocked, l_inc_state);
    EXPECT_EQ(l_inc.ThinLockCount(), i);
  }

  // Force a fat lock by running identity hashcode to fill up lock word.
  Handle<mirror::String> obj2(hs.NewHandle(
      mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));

  obj2->IdentityHashCode();

  Invoke3(reinterpret_cast<size_t>(obj2.Get()), 0U, 0U, art_quick_lock_object, self);

  LockWord lock_after2 = obj2->GetLockWord(false);
  LockWord::LockState new_state2 = lock_after2.GetState();
  EXPECT_EQ(LockWord::LockState::kFatLocked, new_state2);
  EXPECT_NE(lock_after2.FatLockMonitor(), static_cast<Monitor*>(nullptr));

  // Test done.
#else
  LOG(INFO) << "Skipping lock_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping lock_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


class RandGen {
 public:
  explicit RandGen(uint32_t seed) : val_(seed) {}

  uint32_t next() {
    val_ = val_ * 48271 % 2147483647 + 13;
    return val_;
  }

  uint32_t val_;
};


// NO_THREAD_SAFETY_ANALYSIS as we do not want to grab exclusive mutator lock for MonitorInfo.
static void TestUnlockObject(StubTest* test) NO_THREAD_SAFETY_ANALYSIS {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  static constexpr size_t kThinLockLoops = 100;

  Thread* self = Thread::Current();

  const uintptr_t art_quick_lock_object = StubTest::GetEntrypoint(self, kQuickLockObject);
  const uintptr_t art_quick_unlock_object = StubTest::GetEntrypoint(self, kQuickUnlockObject);
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init
  static constexpr size_t kNumberOfLocks = 10;  // Number of objects = lock
  StackHandleScope<kNumberOfLocks + 1> hs(self);
  Handle<mirror::String> obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));
  LockWord lock = obj->GetLockWord(false);
  LockWord::LockState old_state = lock.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, old_state);

  test->Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U, art_quick_unlock_object, self);
  // This should be an illegal monitor state.
  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  LockWord lock_after = obj->GetLockWord(false);
  LockWord::LockState new_state = lock_after.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, new_state);

  test->Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U, art_quick_lock_object, self);

  LockWord lock_after2 = obj->GetLockWord(false);
  LockWord::LockState new_state2 = lock_after2.GetState();
  EXPECT_EQ(LockWord::LockState::kThinLocked, new_state2);

  test->Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U, art_quick_unlock_object, self);

  LockWord lock_after3 = obj->GetLockWord(false);
  LockWord::LockState new_state3 = lock_after3.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, new_state3);

  // Stress test:
  // Keep a number of objects and their locks in flight. Randomly lock or unlock one of them in
  // each step.

  RandGen r(0x1234);

  constexpr size_t kIterations = 10000;  // Number of iterations
  constexpr size_t kMoveToFat = 1000;     // Chance of 1:kMoveFat to make a lock fat.

  size_t counts[kNumberOfLocks];
  bool fat[kNumberOfLocks];  // Whether a lock should be thin or fat.
  Handle<mirror::String> objects[kNumberOfLocks];

  // Initialize = allocate.
  for (size_t i = 0; i < kNumberOfLocks; ++i) {
    counts[i] = 0;
    fat[i] = false;
    objects[i] = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), ""));
  }

  for (size_t i = 0; i < kIterations; ++i) {
    // Select which lock to update.
    size_t index = r.next() % kNumberOfLocks;

    // Make lock fat?
    if (!fat[index] && (r.next() % kMoveToFat == 0)) {
      fat[index] = true;
      objects[index]->IdentityHashCode();

      LockWord lock_iter = objects[index]->GetLockWord(false);
      LockWord::LockState iter_state = lock_iter.GetState();
      if (counts[index] == 0) {
        EXPECT_EQ(LockWord::LockState::kHashCode, iter_state);
      } else {
        EXPECT_EQ(LockWord::LockState::kFatLocked, iter_state);
      }
    } else {
      bool take_lock;  // Whether to lock or unlock in this step.
      if (counts[index] == 0) {
        take_lock = true;
      } else if (counts[index] == kThinLockLoops) {
        take_lock = false;
      } else {
        // Randomly.
        take_lock = r.next() % 2 == 0;
      }

      if (take_lock) {
        test->Invoke3(reinterpret_cast<size_t>(objects[index].Get()), 0U, 0U, art_quick_lock_object,
                      self);
        counts[index]++;
      } else {
        test->Invoke3(reinterpret_cast<size_t>(objects[index].Get()), 0U, 0U,
                      art_quick_unlock_object, self);
        counts[index]--;
      }

      EXPECT_FALSE(self->IsExceptionPending());

      // Check the new state.
      LockWord lock_iter = objects[index]->GetLockWord(true);
      LockWord::LockState iter_state = lock_iter.GetState();
      if (fat[index]) {
        // Abuse MonitorInfo.
        EXPECT_EQ(LockWord::LockState::kFatLocked, iter_state) << index;
        MonitorInfo info(objects[index].Get());
        EXPECT_EQ(counts[index], info.entry_count_) << index;
      } else {
        if (counts[index] > 0) {
          EXPECT_EQ(LockWord::LockState::kThinLocked, iter_state);
          EXPECT_EQ(counts[index] - 1, lock_iter.ThinLockCount());
        } else {
          EXPECT_EQ(LockWord::LockState::kUnlocked, iter_state);
        }
      }
    }
  }

  // Unlock the remaining count times and then check it's unlocked. Then deallocate.
  // Go reverse order to correctly handle Handles.
  for (size_t i = 0; i < kNumberOfLocks; ++i) {
    size_t index = kNumberOfLocks - 1 - i;
    size_t count = counts[index];
    while (count > 0) {
      test->Invoke3(reinterpret_cast<size_t>(objects[index].Get()), 0U, 0U, art_quick_unlock_object,
                    self);
      count--;
    }

    LockWord lock_after4 = objects[index]->GetLockWord(false);
    LockWord::LockState new_state4 = lock_after4.GetState();
    EXPECT_TRUE(LockWord::LockState::kUnlocked == new_state4
                || LockWord::LockState::kFatLocked == new_state4);
  }

  // Test done.
#else
  UNUSED(test);
  LOG(INFO) << "Skipping unlock_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping unlock_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, UnlockObject) {
  // This will lead to monitor error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  TestUnlockObject(this);
}

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
extern "C" void art_quick_check_cast(void);
#endif

TEST_F(StubTest, CheckCast) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  Thread* self = Thread::Current();

  const uintptr_t art_quick_check_cast = StubTest::GetEntrypoint(self, kQuickCheckCast);

  // Find some classes.
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));
  Handle<mirror::Class> c2(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;")));

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(c.Get()), 0U,
          art_quick_check_cast, self);

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c2.Get()), reinterpret_cast<size_t>(c2.Get()), 0U,
          art_quick_check_cast, self);

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(c2.Get()), 0U,
          art_quick_check_cast, self);

  EXPECT_FALSE(self->IsExceptionPending());

  // TODO: Make the following work. But that would require correct managed frames.

  Invoke3(reinterpret_cast<size_t>(c2.Get()), reinterpret_cast<size_t>(c.Get()), 0U,
          art_quick_check_cast, self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

#else
  LOG(INFO) << "Skipping check_cast as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping check_cast as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


TEST_F(StubTest, APutObj) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  Thread* self = Thread::Current();

  // Do not check non-checked ones, we'd need handlers and stuff...
  const uintptr_t art_quick_aput_obj_with_null_and_bound_check =
      StubTest::GetEntrypoint(self, kQuickAputObjectWithNullAndBoundCheck);

  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  Handle<mirror::Class> ca(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;")));

  // Build a string array of size 1
  Handle<mirror::ObjectArray<mirror::Object>> array(
      hs.NewHandle(mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), ca.Get(), 10)));

  // Build a string -> should be assignable
  Handle<mirror::String> str_obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));

  // Build a generic object -> should fail assigning
  Handle<mirror::Object> obj_obj(hs.NewHandle(c->AllocObject(soa.Self())));

  // Play with it...

  // 1) Success cases
  // 1.1) Assign str_obj to array[0..3]

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(array.Get()), 0U, reinterpret_cast<size_t>(str_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(0));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 1U, reinterpret_cast<size_t>(str_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(1));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 2U, reinterpret_cast<size_t>(str_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(2));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 3U, reinterpret_cast<size_t>(str_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(3));

  // 1.2) Assign null to array[0..3]

  Invoke3(reinterpret_cast<size_t>(array.Get()), 0U, reinterpret_cast<size_t>(nullptr),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(0));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 1U, reinterpret_cast<size_t>(nullptr),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(1));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 2U, reinterpret_cast<size_t>(nullptr),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(2));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 3U, reinterpret_cast<size_t>(nullptr),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(3));

  // TODO: Check _which_ exception is thrown. Then make 3) check that it's the right check order.

  // 2) Failure cases (str into str[])
  // 2.1) Array = null
  // TODO: Throwing NPE needs actual DEX code

//  Invoke3(reinterpret_cast<size_t>(nullptr), 0U, reinterpret_cast<size_t>(str_obj.Get()),
//          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);
//
//  EXPECT_TRUE(self->IsExceptionPending());
//  self->ClearException();

  // 2.2) Index < 0

  Invoke3(reinterpret_cast<size_t>(array.Get()), static_cast<size_t>(-1),
          reinterpret_cast<size_t>(str_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // 2.3) Index > 0

  Invoke3(reinterpret_cast<size_t>(array.Get()), 10U, reinterpret_cast<size_t>(str_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // 3) Failure cases (obj into str[])

  Invoke3(reinterpret_cast<size_t>(array.Get()), 0U, reinterpret_cast<size_t>(obj_obj.Get()),
          art_quick_aput_obj_with_null_and_bound_check, self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // Tests done.
#else
  LOG(INFO) << "Skipping aput_obj as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping aput_obj as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, AllocObject) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  // This will lead to OOM  error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));

  // Play with it...

  EXPECT_FALSE(self->IsExceptionPending());
  {
    // Use an arbitrary method from c to use as referrer
    size_t result = Invoke3(static_cast<size_t>(c->GetDexTypeIndex()),    // type_idx
                            // arbitrary
                            reinterpret_cast<size_t>(c->GetVirtualMethod(0, sizeof(void*))),
                            0U,
                            StubTest::GetEntrypoint(self, kQuickAllocObject),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
  }

  {
    // We can use null in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), 0u, 0U,
                            StubTest::GetEntrypoint(self, kQuickAllocObjectResolved),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
  }

  {
    // We can use null in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), 0u, 0U,
                            StubTest::GetEntrypoint(self, kQuickAllocObjectInitialized),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
  }

  // Failure tests.

  // Out-of-memory.
  {
    Runtime::Current()->GetHeap()->SetIdealFootprint(1 * GB);

    // Array helps to fill memory faster.
    Handle<mirror::Class> ca(
        hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));

    // Use arbitrary large amount for now.
    static const size_t kMaxHandles = 1000000;
    std::unique_ptr<StackHandleScope<kMaxHandles>> hsp(new StackHandleScope<kMaxHandles>(self));

    std::vector<Handle<mirror::Object>> handles;
    // Start allocating with 128K
    size_t length = 128 * KB / 4;
    while (length > 10) {
      Handle<mirror::Object> h(hsp->NewHandle<mirror::Object>(
          mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), ca.Get(), length / 4)));
      if (self->IsExceptionPending() || h.Get() == nullptr) {
        self->ClearException();

        // Try a smaller length
        length = length / 8;
        // Use at most half the reported free space.
        size_t mem = Runtime::Current()->GetHeap()->GetFreeMemory();
        if (length * 8 > mem) {
          length = mem / 8;
        }
      } else {
        handles.push_back(h);
      }
    }
    LOG(INFO) << "Used " << handles.size() << " arrays to fill space.";

    // Allocate simple objects till it fails.
    while (!self->IsExceptionPending()) {
      Handle<mirror::Object> h = hsp->NewHandle(c->AllocObject(soa.Self()));
      if (!self->IsExceptionPending() && h.Get() != nullptr) {
        handles.push_back(h);
      }
    }
    self->ClearException();

    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), 0u, 0U,
                            StubTest::GetEntrypoint(self, kQuickAllocObjectInitialized),
                            self);
    EXPECT_TRUE(self->IsExceptionPending());
    self->ClearException();
    EXPECT_EQ(reinterpret_cast<size_t>(nullptr), result);
  }

  // Tests done.
#else
  LOG(INFO) << "Skipping alloc_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping alloc_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, AllocObjectArray) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  // TODO: Check the "Unresolved" allocation stubs

  // This will lead to OOM  error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(self);
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));

  // Needed to have a linked method.
  Handle<mirror::Class> c_obj(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));

  // Play with it...

  EXPECT_FALSE(self->IsExceptionPending());

  // For some reason this does not work, as the type_idx is artificial and outside what the
  // resolved types of c_obj allow...

  if ((false)) {
    // Use an arbitrary method from c to use as referrer
    size_t result = Invoke3(static_cast<size_t>(c->GetDexTypeIndex()),    // type_idx
                            10U,
                            // arbitrary
                            reinterpret_cast<size_t>(c_obj->GetVirtualMethod(0, sizeof(void*))),
                            StubTest::GetEntrypoint(self, kQuickAllocArray),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Array* obj = reinterpret_cast<mirror::Array*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
    EXPECT_EQ(obj->GetLength(), 10);
  }

  {
    // We can use null in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), 10U,
                            reinterpret_cast<size_t>(nullptr),
                            StubTest::GetEntrypoint(self, kQuickAllocArrayResolved),
                            self);
    EXPECT_FALSE(self->IsExceptionPending()) << PrettyTypeOf(self->GetException());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_TRUE(obj->IsArrayInstance());
    EXPECT_TRUE(obj->IsObjectArray());
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
    mirror::Array* array = reinterpret_cast<mirror::Array*>(result);
    EXPECT_EQ(array->GetLength(), 10);
  }

  // Failure tests.

  // Out-of-memory.
  {
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()),
                            GB,  // that should fail...
                            reinterpret_cast<size_t>(nullptr),
                            StubTest::GetEntrypoint(self, kQuickAllocArrayResolved),
                            self);

    EXPECT_TRUE(self->IsExceptionPending());
    self->ClearException();
    EXPECT_EQ(reinterpret_cast<size_t>(nullptr), result);
  }

  // Tests done.
#else
  LOG(INFO) << "Skipping alloc_array as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping alloc_array as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


TEST_F(StubTest, StringCompareTo) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || \
    defined(__mips__) || (defined(__x86_64__) && !defined(__APPLE__))
  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();

  const uintptr_t art_quick_string_compareto = StubTest::GetEntrypoint(self, kQuickStringCompareTo);

  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  // Create some strings
  // Use array so we can index into it and use a matrix for expected results
  // Setup: The first half is standard. The second half uses a non-zero offset.
  // TODO: Shared backing arrays.
  const char* c[] = { "", "", "a", "aa", "ab",
      "aacaacaacaacaacaac",  // This one's under the default limit to go to __memcmp16.
      "aacaacaacaacaacaacaacaacaacaacaacaac",     // This one's over.
      "aacaacaacaacaacaacaacaacaacaacaacaaca" };  // As is this one. We need a separate one to
                                                  // defeat object-equal optimizations.
  static constexpr size_t kStringCount = arraysize(c);

  StackHandleScope<kStringCount> hs(self);
  Handle<mirror::String> s[kStringCount];

  for (size_t i = 0; i < kStringCount; ++i) {
    s[i] = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), c[i]));
  }

  // TODO: wide characters

  // Matrix of expectations. First component is first parameter. Note we only check against the
  // sign, not the value. As we are testing random offsets, we need to compute this and need to
  // rely on String::CompareTo being correct.
  int32_t expected[kStringCount][kStringCount];
  for (size_t x = 0; x < kStringCount; ++x) {
    for (size_t y = 0; y < kStringCount; ++y) {
      expected[x][y] = s[x]->CompareTo(s[y].Get());
    }
  }

  // Play with it...

  for (size_t x = 0; x < kStringCount; ++x) {
    for (size_t y = 0; y < kStringCount; ++y) {
      // Test string_compareto x y
      size_t result = Invoke3(reinterpret_cast<size_t>(s[x].Get()),
                              reinterpret_cast<size_t>(s[y].Get()), 0U,
                              art_quick_string_compareto, self);

      EXPECT_FALSE(self->IsExceptionPending());

      // The result is a 32b signed integer
      union {
        size_t r;
        int32_t i;
      } conv;
      conv.r = result;
      int32_t e = expected[x][y];
      EXPECT_TRUE(e == 0 ? conv.i == 0 : true) << "x=" << c[x] << " y=" << c[y] << " res=" <<
          conv.r;
      EXPECT_TRUE(e < 0 ? conv.i < 0 : true)   << "x=" << c[x] << " y="  << c[y] << " res=" <<
          conv.r;
      EXPECT_TRUE(e > 0 ? conv.i > 0 : true)   << "x=" << c[x] << " y=" << c[y] << " res=" <<
          conv.r;
    }
  }

  // TODO: Deallocate things.

  // Tests done.
#else
  LOG(INFO) << "Skipping string_compareto as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping string_compareto as I don't know how to do that on " << kRuntimeISA <<
      std::endl;
#endif
}


static void GetSetBooleanStatic(ArtField* f, Thread* self,
                                ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  constexpr size_t num_values = 5;
  uint8_t values[num_values] = { 0, 1, 2, 128, 0xFF };

  for (size_t i = 0; i < num_values; ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              static_cast<size_t>(values[i]),
                              0U,
                              StubTest::GetEntrypoint(self, kQuickSet8Static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                           0U, 0U,
                                           StubTest::GetEntrypoint(self, kQuickGetBooleanStatic),
                                           self,
                                           referrer);
    // Boolean currently stores bools as uint8_t, be more zealous about asserting correct writes/gets.
    EXPECT_EQ(values[i], static_cast<uint8_t>(res)) << "Iteration " << i;
  }
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping set_boolean_static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_boolean_static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}
static void GetSetByteStatic(ArtField* f, Thread* self, ArtMethod* referrer,
                             StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  int8_t values[] = { -128, -64, 0, 64, 127 };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              static_cast<size_t>(values[i]),
                              0U,
                              StubTest::GetEntrypoint(self, kQuickSet8Static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                           0U, 0U,
                                           StubTest::GetEntrypoint(self, kQuickGetByteStatic),
                                           self,
                                           referrer);
    EXPECT_EQ(values[i], static_cast<int8_t>(res)) << "Iteration " << i;
  }
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping set_byte_static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_byte_static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


static void GetSetBooleanInstance(Handle<mirror::Object>* obj, ArtField* f, Thread* self,
                                  ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  uint8_t values[] = { 0, true, 2, 128, 0xFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              StubTest::GetEntrypoint(self, kQuickSet8Instance),
                              self,
                              referrer);

    uint8_t res = f->GetBoolean(obj->Get());
    EXPECT_EQ(values[i], res) << "Iteration " << i;

    f->SetBoolean<false>(obj->Get(), res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            StubTest::GetEntrypoint(self, kQuickGetBooleanInstance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<uint8_t>(res2));
  }
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping set_boolean_instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_boolean_instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}
static void GetSetByteInstance(Handle<mirror::Object>* obj, ArtField* f,
                             Thread* self, ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  int8_t values[] = { -128, -64, 0, 64, 127 };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              StubTest::GetEntrypoint(self, kQuickSet8Instance),
                              self,
                              referrer);

    int8_t res = f->GetByte(obj->Get());
    EXPECT_EQ(res, values[i]) << "Iteration " << i;
    f->SetByte<false>(obj->Get(), ++res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            StubTest::GetEntrypoint(self, kQuickGetByteInstance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<int8_t>(res2));
  }
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping set_byte_instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_byte_instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

static void GetSetCharStatic(ArtField* f, Thread* self, ArtMethod* referrer,
                             StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  uint16_t values[] = { 0, 1, 2, 255, 32768, 0xFFFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              static_cast<size_t>(values[i]),
                              0U,
                              StubTest::GetEntrypoint(self, kQuickSet16Static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                           0U, 0U,
                                           StubTest::GetEntrypoint(self, kQuickGetCharStatic),
                                           self,
                                           referrer);

    EXPECT_EQ(values[i], static_cast<uint16_t>(res)) << "Iteration " << i;
  }
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping set_char_static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_char_static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}
static void GetSetShortStatic(ArtField* f, Thread* self,
                              ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  int16_t values[] = { -0x7FFF, -32768, 0, 255, 32767, 0x7FFE };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              static_cast<size_t>(values[i]),
                              0U,
                              StubTest::GetEntrypoint(self, kQuickSet16Static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                           0U, 0U,
                                           StubTest::GetEntrypoint(self, kQuickGetShortStatic),
                                           self,
                                           referrer);

    EXPECT_EQ(static_cast<int16_t>(res), values[i]) << "Iteration " << i;
  }
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping set_short_static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_short_static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

static void GetSetCharInstance(Handle<mirror::Object>* obj, ArtField* f,
                               Thread* self, ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  uint16_t values[] = { 0, 1, 2, 255, 32768, 0xFFFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              StubTest::GetEntrypoint(self, kQuickSet16Instance),
                              self,
                              referrer);

    uint16_t res = f->GetChar(obj->Get());
    EXPECT_EQ(res, values[i]) << "Iteration " << i;
    f->SetChar<false>(obj->Get(), ++res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            StubTest::GetEntrypoint(self, kQuickGetCharInstance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<uint16_t>(res2));
  }
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping set_char_instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_char_instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}
static void GetSetShortInstance(Handle<mirror::Object>* obj, ArtField* f,
                             Thread* self, ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  int16_t values[] = { -0x7FFF, -32768, 0, 255, 32767, 0x7FFE };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              StubTest::GetEntrypoint(self, kQuickSet16Instance),
                              self,
                              referrer);

    int16_t res = f->GetShort(obj->Get());
    EXPECT_EQ(res, values[i]) << "Iteration " << i;
    f->SetShort<false>(obj->Get(), ++res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            StubTest::GetEntrypoint(self, kQuickGetShortInstance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<int16_t>(res2));
  }
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping set_short_instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set_short_instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

static void GetSet32Static(ArtField* f, Thread* self, ArtMethod* referrer,
                           StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  uint32_t values[] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              static_cast<size_t>(values[i]),
                              0U,
                              StubTest::GetEntrypoint(self, kQuickSet32Static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                           0U, 0U,
                                           StubTest::GetEntrypoint(self, kQuickGet32Static),
                                           self,
                                           referrer);

#if defined(__mips__) && defined(__LP64__)
    EXPECT_EQ(static_cast<uint32_t>(res), values[i]) << "Iteration " << i;
#else
    EXPECT_EQ(res, values[i]) << "Iteration " << i;
#endif
  }
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping set32static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set32static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


static void GetSet32Instance(Handle<mirror::Object>* obj, ArtField* f,
                             Thread* self, ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  uint32_t values[] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              StubTest::GetEntrypoint(self, kQuickSet32Instance),
                              self,
                              referrer);

    int32_t res = f->GetInt(obj->Get());
    EXPECT_EQ(res, static_cast<int32_t>(values[i])) << "Iteration " << i;

    res++;
    f->SetInt<false>(obj->Get(), res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            StubTest::GetEntrypoint(self, kQuickGet32Instance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<int32_t>(res2));
  }
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping set32instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set32instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))

static void set_and_check_static(uint32_t f_idx, mirror::Object* val, Thread* self,
                                 ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  test->Invoke3WithReferrer(static_cast<size_t>(f_idx),
                            reinterpret_cast<size_t>(val),
                            0U,
                            StubTest::GetEntrypoint(self, kQuickSetObjStatic),
                            self,
                            referrer);

  size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f_idx),
                                         0U, 0U,
                                         StubTest::GetEntrypoint(self, kQuickGetObjStatic),
                                         self,
                                         referrer);

  EXPECT_EQ(res, reinterpret_cast<size_t>(val)) << "Value " << val;
}
#endif

static void GetSetObjStatic(ArtField* f, Thread* self, ArtMethod* referrer,
                            StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  set_and_check_static(f->GetDexFieldIndex(), nullptr, self, referrer, test);

  // Allocate a string object for simplicity.
  mirror::String* str = mirror::String::AllocFromModifiedUtf8(self, "Test");
  set_and_check_static(f->GetDexFieldIndex(), str, self, referrer, test);

  set_and_check_static(f->GetDexFieldIndex(), nullptr, self, referrer, test);
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping setObjstatic as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping setObjstatic as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
static void set_and_check_instance(ArtField* f, mirror::Object* trg,
                                   mirror::Object* val, Thread* self, ArtMethod* referrer,
                                   StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                            reinterpret_cast<size_t>(trg),
                            reinterpret_cast<size_t>(val),
                            StubTest::GetEntrypoint(self, kQuickSetObjInstance),
                            self,
                            referrer);

  size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                         reinterpret_cast<size_t>(trg),
                                         0U,
                                         StubTest::GetEntrypoint(self, kQuickGetObjInstance),
                                         self,
                                         referrer);

  EXPECT_EQ(res, reinterpret_cast<size_t>(val)) << "Value " << val;

  EXPECT_EQ(val, f->GetObj(trg));
}
#endif

static void GetSetObjInstance(Handle<mirror::Object>* obj, ArtField* f,
                              Thread* self, ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  set_and_check_instance(f, obj->Get(), nullptr, self, referrer, test);

  // Allocate a string object for simplicity.
  mirror::String* str = mirror::String::AllocFromModifiedUtf8(self, "Test");
  set_and_check_instance(f, obj->Get(), str, self, referrer, test);

  set_and_check_instance(f, obj->Get(), nullptr, self, referrer, test);
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping setObjinstance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping setObjinstance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


// TODO: Complete these tests for 32b architectures

static void GetSet64Static(ArtField* f, Thread* self, ArtMethod* referrer,
                           StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if (defined(__x86_64__) && !defined(__APPLE__)) || (defined(__mips__) && defined(__LP64__)) \
    || defined(__aarch64__)
  uint64_t values[] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF, 0xFFFFFFFFFFFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    // 64 bit FieldSet stores the set value in the second register.
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              0U,
                              values[i],
                              StubTest::GetEntrypoint(self, kQuickSet64Static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                           0U, 0U,
                                           StubTest::GetEntrypoint(self, kQuickGet64Static),
                                           self,
                                           referrer);

    EXPECT_EQ(res, values[i]) << "Iteration " << i;
  }
#else
  UNUSED(f, self, referrer, test);
  LOG(INFO) << "Skipping set64static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set64static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


static void GetSet64Instance(Handle<mirror::Object>* obj, ArtField* f,
                             Thread* self, ArtMethod* referrer, StubTest* test)
    SHARED_REQUIRES(Locks::mutator_lock_) {
#if (defined(__x86_64__) && !defined(__APPLE__)) || (defined(__mips__) && defined(__LP64__)) || \
    defined(__aarch64__)
  uint64_t values[] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF, 0xFFFFFFFFFFFF };

  for (size_t i = 0; i < arraysize(values); ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              StubTest::GetEntrypoint(self, kQuickSet64Instance),
                              self,
                              referrer);

    int64_t res = f->GetLong(obj->Get());
    EXPECT_EQ(res, static_cast<int64_t>(values[i])) << "Iteration " << i;

    res++;
    f->SetLong<false>(obj->Get(), res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>(f->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            StubTest::GetEntrypoint(self, kQuickGet64Instance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<int64_t>(res2));
  }
#else
  UNUSED(obj, f, self, referrer, test);
  LOG(INFO) << "Skipping set64instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set64instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

static void TestFields(Thread* self, StubTest* test, Primitive::Type test_type) {
  // garbage is created during ClassLinker::Init

  JNIEnv* env = Thread::Current()->GetJniEnv();
  jclass jc = env->FindClass("AllFields");
  CHECK(jc != nullptr);
  jobject o = env->AllocObject(jc);
  CHECK(o != nullptr);

  ScopedObjectAccess soa(self);
  StackHandleScope<3> hs(self);
  Handle<mirror::Object> obj(hs.NewHandle(soa.Decode<mirror::Object*>(o)));
  Handle<mirror::Class> c(hs.NewHandle(obj->GetClass()));
  // Need a method as a referrer
  ArtMethod* m = c->GetDirectMethod(0, sizeof(void*));

  // Play with it...

  // Static fields.
  for (ArtField& f : c->GetSFields()) {
    Primitive::Type type = f.GetTypeAsPrimitiveType();
    if (test_type != type) {
     continue;
    }
    switch (type) {
      case Primitive::Type::kPrimBoolean:
        GetSetBooleanStatic(&f, self, m, test);
        break;
      case Primitive::Type::kPrimByte:
        GetSetByteStatic(&f, self, m, test);
        break;
      case Primitive::Type::kPrimChar:
        GetSetCharStatic(&f, self, m, test);
        break;
      case Primitive::Type::kPrimShort:
        GetSetShortStatic(&f, self, m, test);
        break;
      case Primitive::Type::kPrimInt:
        GetSet32Static(&f, self, m, test);
        break;
      case Primitive::Type::kPrimLong:
        GetSet64Static(&f, self, m, test);
        break;
      case Primitive::Type::kPrimNot:
        // Don't try array.
        if (f.GetTypeDescriptor()[0] != '[') {
          GetSetObjStatic(&f, self, m, test);
        }
        break;
      default:
        break;  // Skip.
    }
  }

  // Instance fields.
  for (ArtField& f : c->GetIFields()) {
    Primitive::Type type = f.GetTypeAsPrimitiveType();
    if (test_type != type) {
      continue;
    }
    switch (type) {
      case Primitive::Type::kPrimBoolean:
        GetSetBooleanInstance(&obj, &f, self, m, test);
        break;
      case Primitive::Type::kPrimByte:
        GetSetByteInstance(&obj, &f, self, m, test);
        break;
      case Primitive::Type::kPrimChar:
        GetSetCharInstance(&obj, &f, self, m, test);
        break;
      case Primitive::Type::kPrimShort:
        GetSetShortInstance(&obj, &f, self, m, test);
        break;
      case Primitive::Type::kPrimInt:
        GetSet32Instance(&obj, &f, self, m, test);
        break;
      case Primitive::Type::kPrimLong:
        GetSet64Instance(&obj, &f, self, m, test);
        break;
      case Primitive::Type::kPrimNot:
        // Don't try array.
        if (f.GetTypeDescriptor()[0] != '[') {
          GetSetObjInstance(&obj, &f, self, m, test);
        }
        break;
      default:
        break;  // Skip.
    }
  }

  // TODO: Deallocate things.
}

TEST_F(StubTest, Fields8) {
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimBoolean);
  TestFields(self, this, Primitive::Type::kPrimByte);
}

TEST_F(StubTest, Fields16) {
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimChar);
  TestFields(self, this, Primitive::Type::kPrimShort);
}

TEST_F(StubTest, Fields32) {
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimInt);
}

TEST_F(StubTest, FieldsObj) {
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimNot);
}

TEST_F(StubTest, Fields64) {
  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimLong);
}

// Disabled, b/27991555 .
// FIXME: Hacking the entry point to point to art_quick_to_interpreter_bridge is broken.
// The bridge calls through to GetCalleeSaveMethodCaller() which looks up the pre-header
// and gets a bogus OatQuickMethodHeader* pointing into our assembly code just before
// the bridge and uses that to check for inlined frames, crashing in the process.
TEST_F(StubTest, DISABLED_IMT) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
    (defined(__x86_64__) && !defined(__APPLE__))
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  StackHandleScope<7> hs(self);

  JNIEnv* env = Thread::Current()->GetJniEnv();

  // ArrayList

  // Load ArrayList and used methods (JNI).
  jclass arraylist_jclass = env->FindClass("java/util/ArrayList");
  ASSERT_NE(nullptr, arraylist_jclass);
  jmethodID arraylist_constructor = env->GetMethodID(arraylist_jclass, "<init>", "()V");
  ASSERT_NE(nullptr, arraylist_constructor);
  jmethodID contains_jmethod = env->GetMethodID(
      arraylist_jclass, "contains", "(Ljava/lang/Object;)Z");
  ASSERT_NE(nullptr, contains_jmethod);
  jmethodID add_jmethod = env->GetMethodID(arraylist_jclass, "add", "(Ljava/lang/Object;)Z");
  ASSERT_NE(nullptr, add_jmethod);

  // Get representation.
  ArtMethod* contains_amethod = soa.DecodeMethod(contains_jmethod);

  // Patch up ArrayList.contains.
  if (contains_amethod->GetEntryPointFromQuickCompiledCode() == nullptr) {
    contains_amethod->SetEntryPointFromQuickCompiledCode(reinterpret_cast<void*>(
        StubTest::GetEntrypoint(self, kQuickQuickToInterpreterBridge)));
  }

  // List

  // Load List and used methods (JNI).
  jclass list_jclass = env->FindClass("java/util/List");
  ASSERT_NE(nullptr, list_jclass);
  jmethodID inf_contains_jmethod = env->GetMethodID(
      list_jclass, "contains", "(Ljava/lang/Object;)Z");
  ASSERT_NE(nullptr, inf_contains_jmethod);

  // Get mirror representation.
  ArtMethod* inf_contains = soa.DecodeMethod(inf_contains_jmethod);

  // Object

  jclass obj_jclass = env->FindClass("java/lang/Object");
  ASSERT_NE(nullptr, obj_jclass);
  jmethodID obj_constructor = env->GetMethodID(obj_jclass, "<init>", "()V");
  ASSERT_NE(nullptr, obj_constructor);

  // Create instances.

  jobject jarray_list = env->NewObject(arraylist_jclass, arraylist_constructor);
  ASSERT_NE(nullptr, jarray_list);
  Handle<mirror::Object> array_list(hs.NewHandle(soa.Decode<mirror::Object*>(jarray_list)));

  jobject jobj = env->NewObject(obj_jclass, obj_constructor);
  ASSERT_NE(nullptr, jobj);
  Handle<mirror::Object> obj(hs.NewHandle(soa.Decode<mirror::Object*>(jobj)));

  // Invocation tests.

  // 1. imt_conflict

  // Contains.

  // We construct the ImtConflictTable ourselves, as we cannot go into the runtime stub
  // that will create it: the runtime stub expects to be called by compiled code.
  LinearAlloc* linear_alloc = Runtime::Current()->GetLinearAlloc();
  ArtMethod* conflict_method = Runtime::Current()->CreateImtConflictMethod(linear_alloc);
  ImtConflictTable* empty_conflict_table =
      Runtime::Current()->GetClassLinker()->CreateImtConflictTable(/*count*/0u, linear_alloc);
  void* data = linear_alloc->Alloc(
      self,
      ImtConflictTable::ComputeSizeWithOneMoreEntry(empty_conflict_table, sizeof(void*)));
  ImtConflictTable* new_table = new (data) ImtConflictTable(
      empty_conflict_table, inf_contains, contains_amethod, sizeof(void*));
  conflict_method->SetImtConflictTable(new_table, sizeof(void*));

  size_t result =
      Invoke3WithReferrerAndHidden(reinterpret_cast<size_t>(conflict_method),
                                   reinterpret_cast<size_t>(array_list.Get()),
                                   reinterpret_cast<size_t>(obj.Get()),
                                   StubTest::GetEntrypoint(self, kQuickQuickImtConflictTrampoline),
                                   self,
                                   contains_amethod,
                                   static_cast<size_t>(inf_contains->GetDexMethodIndex()));

  ASSERT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(static_cast<size_t>(JNI_FALSE), result);

  // Add object.

  env->CallBooleanMethod(jarray_list, add_jmethod, jobj);

  ASSERT_FALSE(self->IsExceptionPending()) << PrettyTypeOf(self->GetException());

  // Contains.

  result =
      Invoke3WithReferrerAndHidden(reinterpret_cast<size_t>(conflict_method),
                                   reinterpret_cast<size_t>(array_list.Get()),
                                   reinterpret_cast<size_t>(obj.Get()),
                                   StubTest::GetEntrypoint(self, kQuickQuickImtConflictTrampoline),
                                   self,
                                   contains_amethod,
                                   static_cast<size_t>(inf_contains->GetDexMethodIndex()));

  ASSERT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(static_cast<size_t>(JNI_TRUE), result);

  // 2. regular interface trampoline

  result = Invoke3WithReferrer(static_cast<size_t>(inf_contains->GetDexMethodIndex()),
                               reinterpret_cast<size_t>(array_list.Get()),
                               reinterpret_cast<size_t>(obj.Get()),
                               StubTest::GetEntrypoint(self,
                                   kQuickInvokeInterfaceTrampolineWithAccessCheck),
                               self, contains_amethod);

  ASSERT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(static_cast<size_t>(JNI_TRUE), result);

  result = Invoke3WithReferrer(
      static_cast<size_t>(inf_contains->GetDexMethodIndex()),
      reinterpret_cast<size_t>(array_list.Get()), reinterpret_cast<size_t>(array_list.Get()),
      StubTest::GetEntrypoint(self, kQuickInvokeInterfaceTrampolineWithAccessCheck), self,
      contains_amethod);

  ASSERT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(static_cast<size_t>(JNI_FALSE), result);
#else
  LOG(INFO) << "Skipping imt as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping imt as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, StringIndexOf) {
#if defined(__arm__) || defined(__aarch64__) || defined(__mips__)
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  // Create some strings
  // Use array so we can index into it and use a matrix for expected results
  // Setup: The first half is standard. The second half uses a non-zero offset.
  // TODO: Shared backing arrays.
  const char* c_str[] = { "", "a", "ba", "cba", "dcba", "edcba", "asdfghjkl" };
  static constexpr size_t kStringCount = arraysize(c_str);
  const char c_char[] = { 'a', 'b', 'c', 'd', 'e' };
  static constexpr size_t kCharCount = arraysize(c_char);

  StackHandleScope<kStringCount> hs(self);
  Handle<mirror::String> s[kStringCount];

  for (size_t i = 0; i < kStringCount; ++i) {
    s[i] = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), c_str[i]));
  }

  // Matrix of expectations. First component is first parameter. Note we only check against the
  // sign, not the value. As we are testing random offsets, we need to compute this and need to
  // rely on String::CompareTo being correct.
  static constexpr size_t kMaxLen = 9;
  DCHECK_LE(strlen(c_str[kStringCount-1]), kMaxLen) << "Please fix the indexof test.";

  // Last dimension: start, offset by 1.
  int32_t expected[kStringCount][kCharCount][kMaxLen + 3];
  for (size_t x = 0; x < kStringCount; ++x) {
    for (size_t y = 0; y < kCharCount; ++y) {
      for (size_t z = 0; z <= kMaxLen + 2; ++z) {
        expected[x][y][z] = s[x]->FastIndexOf(c_char[y], static_cast<int32_t>(z) - 1);
      }
    }
  }

  // Play with it...

  for (size_t x = 0; x < kStringCount; ++x) {
    for (size_t y = 0; y < kCharCount; ++y) {
      for (size_t z = 0; z <= kMaxLen + 2; ++z) {
        int32_t start = static_cast<int32_t>(z) - 1;

        // Test string_compareto x y
        size_t result = Invoke3(reinterpret_cast<size_t>(s[x].Get()), c_char[y], start,
                                StubTest::GetEntrypoint(self, kQuickIndexOf), self);

        EXPECT_FALSE(self->IsExceptionPending());

        // The result is a 32b signed integer
        union {
          size_t r;
          int32_t i;
        } conv;
        conv.r = result;

        EXPECT_EQ(expected[x][y][z], conv.i) << "Wrong result for " << c_str[x] << " / " <<
            c_char[y] << " @ " << start;
      }
    }
  }

  // TODO: Deallocate things.

  // Tests done.
#else
  LOG(INFO) << "Skipping indexof as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping indexof as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, ReadBarrier) {
#if defined(ART_USE_READ_BARRIER) && (defined(__i386__) || defined(__arm__) || \
      defined(__aarch64__) || defined(__mips__) || (defined(__x86_64__) && !defined(__APPLE__)))
  Thread* self = Thread::Current();

  const uintptr_t readBarrierSlow = StubTest::GetEntrypoint(self, kQuickReadBarrierSlow);

  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));

  // Build an object instance
  Handle<mirror::Object> obj(hs.NewHandle(c->AllocObject(soa.Self())));

  EXPECT_FALSE(self->IsExceptionPending());

  size_t result = Invoke3(0U, reinterpret_cast<size_t>(obj.Get()),
                          mirror::Object::ClassOffset().SizeValue(), readBarrierSlow, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
  mirror::Class* klass = reinterpret_cast<mirror::Class*>(result);
  EXPECT_EQ(klass, obj->GetClass());

  // Tests done.
#else
  LOG(INFO) << "Skipping read_barrier_slow";
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping read_barrier_slow" << std::endl;
#endif
}

TEST_F(StubTest, ReadBarrierForRoot) {
#if defined(ART_USE_READ_BARRIER) && (defined(__i386__) || defined(__arm__) || \
      defined(__aarch64__) || defined(__mips__) || (defined(__x86_64__) && !defined(__APPLE__)))
  Thread* self = Thread::Current();

  const uintptr_t readBarrierForRootSlow =
      StubTest::GetEntrypoint(self, kQuickReadBarrierForRootSlow);

  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<1> hs(soa.Self());

  Handle<mirror::String> obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));

  EXPECT_FALSE(self->IsExceptionPending());

  GcRoot<mirror::Class>& root = mirror::String::java_lang_String_;
  size_t result = Invoke3(reinterpret_cast<size_t>(&root), 0U, 0U, readBarrierForRootSlow, self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
  mirror::Class* klass = reinterpret_cast<mirror::Class*>(result);
  EXPECT_EQ(klass, obj->GetClass());

  // Tests done.
#else
  LOG(INFO) << "Skipping read_barrier_for_root_slow";
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping read_barrier_for_root_slow" << std::endl;
#endif
}

}  // namespace art
