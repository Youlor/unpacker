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
#include "base/hex_dump.h"
#include "globals.h"
#include "base/logging.h"
#include "base/hex_dump.h"
#include "thread.h"
#include "thread-inl.h"

//
// ARM specific fault handler functions.
//

namespace art {

extern "C" void art_quick_throw_null_pointer_exception();
extern "C" void art_quick_throw_stack_overflow();
extern "C" void art_quick_implicit_suspend();

// Get the size of a thumb2 instruction in bytes.
static uint32_t GetInstructionSize(uint8_t* pc) {
  uint16_t instr = pc[0] | pc[1] << 8;
  bool is_32bit = ((instr & 0xF000) == 0xF000) || ((instr & 0xF800) == 0xE800);
  uint32_t instr_size = is_32bit ? 4 : 2;
  return instr_size;
}

void FaultManager::HandleNestedSignal(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                                      void* context) {
  // Note that in this handler we set up the registers and return to
  // longjmp directly rather than going through an assembly language stub.  The
  // reason for this is that longjmp is (currently) in ARM mode and that would
  // require switching modes in the stub - incurring an unwanted relocation.

  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  Thread* self = Thread::Current();
  CHECK(self != nullptr);  // This will cause a SIGABRT if self is null.

  sc->arm_r0 = reinterpret_cast<uintptr_t>(*self->GetNestedSignalState());
  sc->arm_r1 = 1;
  sc->arm_pc = reinterpret_cast<uintptr_t>(longjmp);
  VLOG(signals) << "longjmp address: " << reinterpret_cast<void*>(sc->arm_pc);
}

void FaultManager::GetMethodAndReturnPcAndSp(siginfo_t* siginfo ATTRIBUTE_UNUSED, void* context,
                                             ArtMethod** out_method,
                                             uintptr_t* out_return_pc, uintptr_t* out_sp) {
  struct ucontext* uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  *out_sp = static_cast<uintptr_t>(sc->arm_sp);
  VLOG(signals) << "sp: " << std::hex << *out_sp;
  if (*out_sp == 0) {
    return;
  }

  // In the case of a stack overflow, the stack is not valid and we can't
  // get the method from the top of the stack.  However it's in r0.
  uintptr_t* fault_addr = reinterpret_cast<uintptr_t*>(sc->fault_address);
  uintptr_t* overflow_addr = reinterpret_cast<uintptr_t*>(
      reinterpret_cast<uint8_t*>(*out_sp) - GetStackOverflowReservedBytes(kArm));
  if (overflow_addr == fault_addr) {
    *out_method = reinterpret_cast<ArtMethod*>(sc->arm_r0);
  } else {
    // The method is at the top of the stack.
    *out_method = reinterpret_cast<ArtMethod*>(reinterpret_cast<uintptr_t*>(*out_sp)[0]);
  }

  // Work out the return PC.  This will be the address of the instruction
  // following the faulting ldr/str instruction.  This is in thumb mode so
  // the instruction might be a 16 or 32 bit one.  Also, the GC map always
  // has the bottom bit of the PC set so we also need to set that.

  // Need to work out the size of the instruction that caused the exception.
  uint8_t* ptr = reinterpret_cast<uint8_t*>(sc->arm_pc);
  VLOG(signals) << "pc: " << std::hex << static_cast<void*>(ptr);

  if (ptr == nullptr) {
    // Somebody jumped to 0x0. Definitely not ours, and will definitely segfault below.
    *out_method = nullptr;
    return;
  }

  uint32_t instr_size = GetInstructionSize(ptr);

  *out_return_pc = (sc->arm_pc + instr_size) | 1;
}

bool NullPointerHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                                void* context) {
  // The code that looks for the catch location needs to know the value of the
  // ARM PC at the point of call.  For Null checks we insert a GC map that is immediately after
  // the load/store instruction that might cause the fault.  However the mapping table has
  // the low bits set for thumb mode so we need to set the bottom bit for the LR
  // register in order to find the mapping.

  // Need to work out the size of the instruction that caused the exception.
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  uint8_t* ptr = reinterpret_cast<uint8_t*>(sc->arm_pc);

  uint32_t instr_size = GetInstructionSize(ptr);
  sc->arm_lr = (sc->arm_pc + instr_size) | 1;      // LR needs to point to gc map location
  sc->arm_pc = reinterpret_cast<uintptr_t>(art_quick_throw_null_pointer_exception);
  VLOG(signals) << "Generating null pointer exception";
  return true;
}

// A suspend check is done using the following instruction sequence:
// 0xf723c0b2: f8d902c0  ldr.w   r0, [r9, #704]  ; suspend_trigger_
// .. some intervening instruction
// 0xf723c0b6: 6800      ldr     r0, [r0, #0]

// The offset from r9 is Thread::ThreadSuspendTriggerOffset().
// To check for a suspend check, we examine the instructions that caused
// the fault (at PC-4 and PC).
bool SuspensionHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                               void* context) {
  // These are the instructions to check for.  The first one is the ldr r0,[r9,#xxx]
  // where xxx is the offset of the suspend trigger.
  uint32_t checkinst1 = 0xf8d90000 + Thread::ThreadSuspendTriggerOffset<4>().Int32Value();
  uint16_t checkinst2 = 0x6800;

  struct ucontext* uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  uint8_t* ptr2 = reinterpret_cast<uint8_t*>(sc->arm_pc);
  uint8_t* ptr1 = ptr2 - 4;
  VLOG(signals) << "checking suspend";

  uint16_t inst2 = ptr2[0] | ptr2[1] << 8;
  VLOG(signals) << "inst2: " << std::hex << inst2 << " checkinst2: " << checkinst2;
  if (inst2 != checkinst2) {
    // Second instruction is not good, not ours.
    return false;
  }

  // The first instruction can a little bit up the stream due to load hoisting
  // in the compiler.
  uint8_t* limit = ptr1 - 40;   // Compiler will hoist to a max of 20 instructions.
  bool found = false;
  while (ptr1 > limit) {
    uint32_t inst1 = ((ptr1[0] | ptr1[1] << 8) << 16) | (ptr1[2] | ptr1[3] << 8);
    VLOG(signals) << "inst1: " << std::hex << inst1 << " checkinst1: " << checkinst1;
    if (inst1 == checkinst1) {
      found = true;
      break;
    }
    ptr1 -= 2;      // Min instruction size is 2 bytes.
  }
  if (found) {
    VLOG(signals) << "suspend check match";
    // This is a suspend check.  Arrange for the signal handler to return to
    // art_quick_implicit_suspend.  Also set LR so that after the suspend check it
    // will resume the instruction (current PC + 2).  PC points to the
    // ldr r0,[r0,#0] instruction (r0 will be 0, set by the trigger).

    // NB: remember that we need to set the bottom bit of the LR register
    // to switch to thumb mode.
    VLOG(signals) << "arm lr: " << std::hex << sc->arm_lr;
    VLOG(signals) << "arm pc: " << std::hex << sc->arm_pc;
    sc->arm_lr = sc->arm_pc + 3;      // +2 + 1 (for thumb)
    sc->arm_pc = reinterpret_cast<uintptr_t>(art_quick_implicit_suspend);

    // Now remove the suspend trigger that caused this fault.
    Thread::Current()->RemoveSuspendTrigger();
    VLOG(signals) << "removed suspend trigger invoking test suspend";
    return true;
  }
  return false;
}

// Stack overflow fault handler.
//
// This checks that the fault address is equal to the current stack pointer
// minus the overflow region size (16K typically).  The instruction sequence
// that generates this signal is:
//
// sub r12,sp,#16384
// ldr.w r12,[r12,#0]
//
// The second instruction will fault if r12 is inside the protected region
// on the stack.
//
// If we determine this is a stack overflow we need to move the stack pointer
// to the overflow region below the protected region.

bool StackOverflowHandler::Action(int sig ATTRIBUTE_UNUSED, siginfo_t* info ATTRIBUTE_UNUSED,
                                  void* context) {
  struct ucontext* uc = reinterpret_cast<struct ucontext*>(context);
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  VLOG(signals) << "stack overflow handler with sp at " << std::hex << &uc;
  VLOG(signals) << "sigcontext: " << std::hex << sc;

  uintptr_t sp = sc->arm_sp;
  VLOG(signals) << "sp: " << std::hex << sp;

  uintptr_t fault_addr = sc->fault_address;
  VLOG(signals) << "fault_addr: " << std::hex << fault_addr;
  VLOG(signals) << "checking for stack overflow, sp: " << std::hex << sp <<
    ", fault_addr: " << fault_addr;

  uintptr_t overflow_addr = sp - GetStackOverflowReservedBytes(kArm);

  // Check that the fault address is the value expected for a stack overflow.
  if (fault_addr != overflow_addr) {
    VLOG(signals) << "Not a stack overflow";
    return false;
  }

  VLOG(signals) << "Stack overflow found";

  // Now arrange for the signal handler to return to art_quick_throw_stack_overflow_from.
  // The value of LR must be the same as it was when we entered the code that
  // caused this fault.  This will be inserted into a callee save frame by
  // the function to which this handler returns (art_quick_throw_stack_overflow).
  sc->arm_pc = reinterpret_cast<uintptr_t>(art_quick_throw_stack_overflow);

  // The kernel will now return to the address in sc->arm_pc.
  return true;
}
}       // namespace art
