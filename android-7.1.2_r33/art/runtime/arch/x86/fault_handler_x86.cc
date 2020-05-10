/*
 * Copyright (C) 2008 The Android Open Source Project
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


#include "fault_handler.h"

#include <sys/ucontext.h>

#include "art_method-inl.h"
#include "base/macros.h"
#include "globals.h"
#include "base/logging.h"
#include "base/hex_dump.h"
#include "thread.h"
#include "thread-inl.h"

#if defined(__APPLE__)
#define ucontext __darwin_ucontext

#if defined(__x86_64__)
// 64 bit mac build.
#define CTX_ESP uc_mcontext->__ss.__rsp
#define CTX_EIP uc_mcontext->__ss.__rip
#define CTX_EAX uc_mcontext->__ss.__rax
#define CTX_METHOD uc_mcontext->__ss.__rdi
#define CTX_JMP_BUF uc_mcontext->__ss.__rdi
#else
// 32 bit mac build.
#define CTX_ESP uc_mcontext->__ss.__esp
#define CTX_EIP uc_mcontext->__ss.__eip
#define CTX_EAX uc_mcontext->__ss.__eax
#define CTX_METHOD uc_mcontext->__ss.__eax
#define CTX_JMP_BUF uc_mcontext->__ss.__eax
#endif

#elif defined(__x86_64__)
// 64 bit linux build.
#define CTX_ESP uc_mcontext.gregs[REG_RSP]
#define CTX_EIP uc_mcontext.gregs[REG_RIP]
#define CTX_EAX uc_mcontext.gregs[REG_RAX]
#define CTX_METHOD uc_mcontext.gregs[REG_RDI]
#define CTX_RDI uc_mcontext.gregs[REG_RDI]
#define CTX_JMP_BUF uc_mcontext.gregs[REG_RDI]
#else
// 32 bit linux build.
#define CTX_ESP uc_mcontext.gregs[REG_ESP]
#define CTX_EIP uc_mcontext.gregs[REG_EIP]
#define CTX_EAX uc_mcontext.gregs[REG_EAX]
#define CTX_METHOD uc_mcontext.gregs[REG_EAX]
#define CTX_JMP_BUF uc_mcontext.gregs[REG_EAX]
#endif

//
// X86 (and X86_64) specific fault handler functions.
//

namespace art {

#if defined(__APPLE__) && defined(__x86_64__)
// mac symbols have a prefix of _ on x86_64
extern "C" void _art_quick_throw_null_pointer_exception();
extern "C" void _art_quick_throw_stack_overflow();
extern "C" void _art_quick_test_suspend();
#define EXT_SYM(sym) _ ## sym
#else
extern "C" void art_quick_throw_null_pointer_exception();
extern "C" void art_quick_throw_stack_overflow();
extern "C" void art_quick_test_suspend();
#define EXT_SYM(sym) sym
#endif

// Note this is different from the others (no underscore on 64 bit mac) due to
// the way the symbol is defined in the .S file.
// TODO: fix the symbols for 64 bit mac - there is a double underscore prefix for some
// of them.
extern "C" void art_nested_signal_return();

// Get the size of an instruction in bytes.
// Return 0 if the instruction is not handled.
static uint32_t GetInstructionSize(const uint8_t* pc) {
#if defined(__x86_64)
  const bool x86_64 = true;
#else
  const bool x86_64 = false;
#endif

  const uint8_t* startpc = pc;

  uint8_t opcode = *pc++;
  uint8_t modrm;
  bool has_modrm = false;
  bool two_byte = false;
  uint32_t displacement_size = 0;
  uint32_t immediate_size = 0;
  bool operand_size_prefix = false;

  // Prefixes.
  while (true) {
    bool prefix_present = false;
    switch (opcode) {
      // Group 3
      case 0x66:
        operand_size_prefix = true;
        FALLTHROUGH_INTENDED;

      // Group 1
      case 0xf0:
      case 0xf2:
      case 0xf3:

      // Group 2
      case 0x2e:
      case 0x36:
      case 0x3e:
      case 0x26:
      case 0x64:
      case 0x65:

      // Group 4
      case 0x67:
        opcode = *pc++;
        prefix_present = true;
        break;
    }
    if (!prefix_present) {
      break;
    }
  }

  if (x86_64 && opcode >= 0x40 && opcode <= 0x4f) {
    opcode = *pc++;
  }

  if (opcode == 0x0f) {
    // Two byte opcode
    two_byte = true;
    opcode = *pc++;
  }

  bool unhandled_instruction = false;

  if (two_byte) {
    switch (opcode) {
      case 0x10:        // vmovsd/ss
      case 0x11:        // vmovsd/ss
      case 0xb6:        // movzx
      case 0xb7:
      case 0xbe:        // movsx
      case 0xbf:
        modrm = *pc++;
        has_modrm = true;
        break;
      default:
        unhandled_instruction = true;
        break;
    }
  } else {
    switch (opcode) {
      case 0x88:        // mov byte
      case 0x89:        // mov
      case 0x8b:
      case 0x38:        // cmp with memory.
      case 0x39:
      case 0x3a:
      case 0x3b:
      case 0x3c:
      case 0x3d:
      case 0x85:        // test.
        modrm = *pc++;
        has_modrm = true;
        break;

      case 0x80:        // group 1, byte immediate.
      case 0x83:
      case 0xc6:
        modrm = *pc++;
        has_modrm = true;
        immediate_size = 1;
        break;

      case 0x81:        // group 1, word immediate.
      case 0xc7:        // mov
        modrm = *pc++;
        has_modrm = true;
        immediate_size = operand_size_prefix ? 2 : 4;
        break;

      default:
        unhandled_instruction = true;
        break;
    }
  }

  if (unhandled_instruction) {
    VLOG(signals) << "Unhandled x86 instruction with opcode " << static_cast<int>(opcode);
    return 0;
  }

  if (has_modrm) {
    uint8_t mod = (modrm >> 6) & 3U /* 0b11 */;

    // Check for SIB.
    if (mod != 3U /* 0b11 */ && (modrm & 7U /* 0b111 */) == 4) {
      ++pc;     // SIB
    }

    switch (mod) {
      case 0U /* 0b00 */: break;
      case 1U /* 0b01 */: displacement_size = 1; break;
      case 2U /* 0b10 */: displacement_size = 4; break;
      case 3U /* 0b11 */:
        break;
    }
  }

  // Skip displacement and immediate.
  pc += displacement_size + immediate_size;

  VLOG(signals) << "x86 instruction length calculated as " << (pc - startpc);
  return pc - startpc;
}

void FaultManager::HandleNestedSignal(int, siginfo_t*, void* context) {
  // For the Intel architectures we need to go to an assembly language
  // stub.  This is because the 32 bit call to longjmp is much different
  // from the 64 bit ABI call and pushing things onto the stack inside this
  // handler was unwieldy and ugly.  The use of the stub means we can keep
  // this code the same for both 32 and 64 bit.

  Thread* self = Thread::Current();
  CHECK(self != nullptr);  // This will cause a SIGABRT if self is null.

  struct ucontext* uc = reinterpret_cast<struct ucontext*>(context);
  uc->CTX_JMP_BUF = reinterpret_cast<uintptr_t>(*self->GetNestedSignalState());
  uc->CTX_EIP = reinterpret_cast<uintptr_t>(art_nested_signal_return);
}

void FaultManager::GetMethodAndReturnPcAndSp(siginfo_t* siginfo, void* context,
                                             ArtMethod** out_method,
                                             uintptr_t* out_return_pc, uintptr_t* out_sp) {
  struct ucontext* uc = reinterpret_cast<struct ucontext*>(context);
  *out_sp = static_cast<uintptr_t>(uc->CTX_ESP);
  VLOG(signals) << "sp: " << std::hex << *out_sp;
  if (*out_sp == 0) {
    return;
  }

  // In the case of a stack overflow, the stack is not valid and we can't
  // get the method from the top of the stack.  However it's in EAX(x86)/RDI(x86_64).
  uintptr_t* fault_addr = reinterpret_cast<uintptr_t*>(siginfo->si_addr);
  uintptr_t* overflow_addr = reinterpret_cast<uintptr_t*>(
#if defined(__x86_64__)
      reinterpret_cast<uint8_t*>(*out_sp) - GetStackOverflowReservedBytes(kX86_64));
#else
      reinterpret_cast<uint8_t*>(*out_sp) - GetStackOverflowReservedBytes(kX86));
#endif
  if (overflow_addr == fault_addr) {
    *out_method = reinterpret_cast<ArtMethod*>(uc->CTX_METHOD);
  } else {
    // The method is at the top of the stack.
    *out_method = *reinterpret_cast<ArtMethod**>(*out_sp);
  }

  uint8_t* pc = reinterpret_cast<uint8_t*>(uc->CTX_EIP);
  VLOG(signals) << HexDump(pc, 32, true, "PC ");

  if (pc == nullptr) {
    // Somebody jumped to 0x0. Definitely not ours, and will definitely segfault below.
    *out_method = nullptr;
    return;
  }

  uint32_t instr_size = GetInstructionSize(pc);
  if (instr_size == 0) {
    // Unknown instruction, tell caller it's not ours.
    *out_method = nullptr;
    return;
  }
  *out_return_pc = reinterpret_cast<uintptr_t>(pc + instr_size);
}

bool NullPointerHandler::Action(int, siginfo_t*, void* context) {
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uint8_t* pc = reinterpret_cast<uint8_t*>(uc->CTX_EIP);
  uint8_t* sp = reinterpret_cast<uint8_t*>(uc->CTX_ESP);

  uint32_t instr_size = GetInstructionSize(pc);
  if (instr_size == 0) {
    // Unknown instruction, can't really happen.
    return false;
  }

  // We need to arrange for the signal handler to return to the null pointer
  // exception generator.  The return address must be the address of the
  // next instruction (this instruction + instruction size).  The return address
  // is on the stack at the top address of the current frame.

  // Push the return address onto the stack.
  uintptr_t retaddr = reinterpret_cast<uintptr_t>(pc + instr_size);
  uintptr_t* next_sp = reinterpret_cast<uintptr_t*>(sp - sizeof(uintptr_t));
  *next_sp = retaddr;
  uc->CTX_ESP = reinterpret_cast<uintptr_t>(next_sp);

  uc->CTX_EIP = reinterpret_cast<uintptr_t>(EXT_SYM(art_quick_throw_null_pointer_exception));
  VLOG(signals) << "Generating null pointer exception";
  return true;
}

// A suspend check is done using the following instruction sequence:
// (x86)
// 0xf720f1df:         648B058C000000      mov     eax, fs:[0x8c]  ; suspend_trigger
// .. some intervening instructions.
// 0xf720f1e6:                   8500      test    eax, [eax]
// (x86_64)
// 0x7f579de45d9e: 65488B0425A8000000      movq    rax, gs:[0xa8]  ; suspend_trigger
// .. some intervening instructions.
// 0x7f579de45da7:               8500      test    eax, [eax]

// The offset from fs is Thread::ThreadSuspendTriggerOffset().
// To check for a suspend check, we examine the instructions that caused
// the fault.
bool SuspensionHandler::Action(int, siginfo_t*, void* context) {
  // These are the instructions to check for.  The first one is the mov eax, fs:[xxx]
  // where xxx is the offset of the suspend trigger.
#if defined(__x86_64__)
  uint32_t trigger = Thread::ThreadSuspendTriggerOffset<8>().Int32Value();
#else
  uint32_t trigger = Thread::ThreadSuspendTriggerOffset<4>().Int32Value();
#endif

  VLOG(signals) << "Checking for suspension point";
#if defined(__x86_64__)
  uint8_t checkinst1[] = {0x65, 0x48, 0x8b, 0x04, 0x25, static_cast<uint8_t>(trigger & 0xff),
      static_cast<uint8_t>((trigger >> 8) & 0xff), 0, 0};
#else
  uint8_t checkinst1[] = {0x64, 0x8b, 0x05, static_cast<uint8_t>(trigger & 0xff),
      static_cast<uint8_t>((trigger >> 8) & 0xff), 0, 0};
#endif
  uint8_t checkinst2[] = {0x85, 0x00};

  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uint8_t* pc = reinterpret_cast<uint8_t*>(uc->CTX_EIP);
  uint8_t* sp = reinterpret_cast<uint8_t*>(uc->CTX_ESP);

  if (pc[0] != checkinst2[0] || pc[1] != checkinst2[1]) {
    // Second instruction is not correct (test eax,[eax]).
    VLOG(signals) << "Not a suspension point";
    return false;
  }

  // The first instruction can a little bit up the stream due to load hoisting
  // in the compiler.
  uint8_t* limit = pc - 100;   // Compiler will hoist to a max of 20 instructions.
  uint8_t* ptr = pc - sizeof(checkinst1);
  bool found = false;
  while (ptr > limit) {
    if (memcmp(ptr, checkinst1, sizeof(checkinst1)) == 0) {
      found = true;
      break;
    }
    ptr -= 1;
  }

  if (found) {
    VLOG(signals) << "suspend check match";

    // We need to arrange for the signal handler to return to the null pointer
    // exception generator.  The return address must be the address of the
    // next instruction (this instruction + 2).  The return address
    // is on the stack at the top address of the current frame.

    // Push the return address onto the stack.
    uintptr_t retaddr = reinterpret_cast<uintptr_t>(pc + 2);
    uintptr_t* next_sp = reinterpret_cast<uintptr_t*>(sp - sizeof(uintptr_t));
    *next_sp = retaddr;
    uc->CTX_ESP = reinterpret_cast<uintptr_t>(next_sp);

    uc->CTX_EIP = reinterpret_cast<uintptr_t>(EXT_SYM(art_quick_test_suspend));

    // Now remove the suspend trigger that caused this fault.
    Thread::Current()->RemoveSuspendTrigger();
    VLOG(signals) << "removed suspend trigger invoking test suspend";
    return true;
  }
  VLOG(signals) << "Not a suspend check match, first instruction mismatch";
  return false;
}

// The stack overflow check is done using the following instruction:
// test eax, [esp+ -xxx]
// where 'xxx' is the size of the overflow area.
//
// This is done before any frame is established in the method.  The return
// address for the previous method is on the stack at ESP.

bool StackOverflowHandler::Action(int, siginfo_t* info, void* context) {
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uintptr_t sp = static_cast<uintptr_t>(uc->CTX_ESP);

  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
  VLOG(signals) << "fault_addr: " << std::hex << fault_addr;
  VLOG(signals) << "checking for stack overflow, sp: " << std::hex << sp <<
    ", fault_addr: " << fault_addr;

#if defined(__x86_64__)
  uintptr_t overflow_addr = sp - GetStackOverflowReservedBytes(kX86_64);
#else
  uintptr_t overflow_addr = sp - GetStackOverflowReservedBytes(kX86);
#endif

  // Check that the fault address is the value expected for a stack overflow.
  if (fault_addr != overflow_addr) {
    VLOG(signals) << "Not a stack overflow";
    return false;
  }

  VLOG(signals) << "Stack overflow found";

  // Since the compiler puts the implicit overflow
  // check before the callee save instructions, the SP is already pointing to
  // the previous frame.

  // Now arrange for the signal handler to return to art_quick_throw_stack_overflow.
  uc->CTX_EIP = reinterpret_cast<uintptr_t>(EXT_SYM(art_quick_throw_stack_overflow));

  return true;
}
}       // namespace art
