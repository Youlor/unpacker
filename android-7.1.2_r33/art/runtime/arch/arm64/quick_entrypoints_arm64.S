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

#include "asm_support_arm64.S"

#include "arch/quick_alloc_entrypoints.S"


    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    adrp xIP0, :got:_ZN3art7Runtime9instance_E
    ldr xIP0, [xIP0, #:got_lo12:_ZN3art7Runtime9instance_E]

    // Our registers aren't intermixed - just spill in order.
    ldr xIP0, [xIP0]  // xIP0 = & (art::Runtime * art::Runtime.instance_) .

    // xIP0 = (ArtMethod*) Runtime.instance_.callee_save_methods[kRefAndArgs]  .
    // Loads appropriate callee-save-method.
    ldr xIP0, [xIP0, RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET ]

    sub sp, sp, #176
    .cfi_adjust_cfa_offset 176

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_SAVE_ALL_CALLEE_SAVE != 176)
#error "SAVE_ALL_CALLEE_SAVE_FRAME(ARM64) size not as expected."
#endif

    // Stack alignment filler [sp, #8].
    // FP callee-saves.
    stp d8, d9,   [sp, #16]
    stp d10, d11, [sp, #32]
    stp d12, d13, [sp, #48]
    stp d14, d15, [sp, #64]

    // GP callee-saves
    stp x19, x20, [sp, #80]
    .cfi_rel_offset x19, 80
    .cfi_rel_offset x20, 88

    stp x21, x22, [sp, #96]
    .cfi_rel_offset x21, 96
    .cfi_rel_offset x22, 104

    stp x23, x24, [sp, #112]
    .cfi_rel_offset x23, 112
    .cfi_rel_offset x24, 120

    stp x25, x26, [sp, #128]
    .cfi_rel_offset x25, 128
    .cfi_rel_offset x26, 136

    stp x27, x28, [sp, #144]
    .cfi_rel_offset x27, 144
    .cfi_rel_offset x28, 152

    stp x29, xLR, [sp, #160]
    .cfi_rel_offset x29, 160
    .cfi_rel_offset x30, 168

    // Store ArtMethod* Runtime::callee_save_methods_[kRefsAndArgs].
    str xIP0, [sp]
    // Place sp in Thread::Current()->top_quick_frame.
    mov xIP0, sp
    str xIP0, [xSELF, # THREAD_TOP_QUICK_FRAME_OFFSET]
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly).
     */
.macro SETUP_REFS_ONLY_CALLEE_SAVE_FRAME
    adrp xIP0, :got:_ZN3art7Runtime9instance_E
    ldr xIP0, [xIP0, #:got_lo12:_ZN3art7Runtime9instance_E]

    // Our registers aren't intermixed - just spill in order.
    ldr xIP0, [xIP0]  // xIP0 = & (art::Runtime * art::Runtime.instance_) .

    // xIP0 = (ArtMethod*) Runtime.instance_.callee_save_methods[kRefOnly]  .
    // Loads appropriate callee-save-method.
    ldr xIP0, [xIP0, RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET ]

    sub sp, sp, #96
    .cfi_adjust_cfa_offset 96

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_REFS_ONLY_CALLEE_SAVE != 96)
#error "REFS_ONLY_CALLEE_SAVE_FRAME(ARM64) size not as expected."
#endif

    // GP callee-saves.
    // x20 paired with ArtMethod* - see below.
    stp x21, x22, [sp, #16]
    .cfi_rel_offset x21, 16
    .cfi_rel_offset x22, 24

    stp x23, x24, [sp, #32]
    .cfi_rel_offset x23, 32
    .cfi_rel_offset x24, 40

    stp x25, x26, [sp, #48]
    .cfi_rel_offset x25, 48
    .cfi_rel_offset x26, 56

    stp x27, x28, [sp, #64]
    .cfi_rel_offset x27, 64
    .cfi_rel_offset x28, 72

    stp x29, xLR, [sp, #80]
    .cfi_rel_offset x29, 80
    .cfi_rel_offset x30, 88

    // Store ArtMethod* Runtime::callee_save_methods_[kRefsOnly].
    stp xIP0, x20, [sp]
    .cfi_rel_offset x20, 8

    // Place sp in Thread::Current()->top_quick_frame.
    mov xIP0, sp
    str xIP0, [xSELF, # THREAD_TOP_QUICK_FRAME_OFFSET]
.endm

// TODO: Probably no need to restore registers preserved by aapcs64.
.macro RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    // Callee-saves.
    ldr x20, [sp, #8]
    .cfi_restore x20

    ldp x21, x22, [sp, #16]
    .cfi_restore x21
    .cfi_restore x22

    ldp x23, x24, [sp, #32]
    .cfi_restore x23
    .cfi_restore x24

    ldp x25, x26, [sp, #48]
    .cfi_restore x25
    .cfi_restore x26

    ldp x27, x28, [sp, #64]
    .cfi_restore x27
    .cfi_restore x28

    ldp x29, xLR, [sp, #80]
    .cfi_restore x29
    .cfi_restore x30

    add sp, sp, #96
    .cfi_adjust_cfa_offset -96
.endm

.macro POP_REFS_ONLY_CALLEE_SAVE_FRAME
    add sp, sp, #96
    .cfi_adjust_cfa_offset - 96
.endm

.macro RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    ret
.endm


.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    sub sp, sp, #224
    .cfi_adjust_cfa_offset 224

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE != 224)
#error "REFS_AND_ARGS_CALLEE_SAVE_FRAME(ARM64) size not as expected."
#endif

    // Stack alignment filler [sp, #8].
    // FP args.
    stp d0, d1, [sp, #16]
    stp d2, d3, [sp, #32]
    stp d4, d5, [sp, #48]
    stp d6, d7, [sp, #64]

    // Core args.
    stp x1, x2, [sp, #80]
    .cfi_rel_offset x1, 80
    .cfi_rel_offset x2, 88

    stp x3, x4, [sp, #96]
    .cfi_rel_offset x3, 96
    .cfi_rel_offset x4, 104

    stp x5, x6, [sp, #112]
    .cfi_rel_offset x5, 112
    .cfi_rel_offset x6, 120

    // x7, Callee-saves.
    stp x7, x20, [sp, #128]
    .cfi_rel_offset x7, 128
    .cfi_rel_offset x20, 136

    stp x21, x22, [sp, #144]
    .cfi_rel_offset x21, 144
    .cfi_rel_offset x22, 152

    stp x23, x24, [sp, #160]
    .cfi_rel_offset x23, 160
    .cfi_rel_offset x24, 168

    stp x25, x26, [sp, #176]
    .cfi_rel_offset x25, 176
    .cfi_rel_offset x26, 184

    stp x27, x28, [sp, #192]
    .cfi_rel_offset x27, 192
    .cfi_rel_offset x28, 200

    // x29(callee-save) and LR.
    stp x29, xLR, [sp, #208]
    .cfi_rel_offset x29, 208
    .cfi_rel_offset x30, 216

.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs).
     *
     * TODO This is probably too conservative - saving FP & LR.
     */
.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    adrp xIP0, :got:_ZN3art7Runtime9instance_E
    ldr xIP0, [xIP0, #:got_lo12:_ZN3art7Runtime9instance_E]

    // Our registers aren't intermixed - just spill in order.
    ldr xIP0, [xIP0]  // xIP0 = & (art::Runtime * art::Runtime.instance_) .

    // xIP0 = (ArtMethod*) Runtime.instance_.callee_save_methods[kRefAndArgs]  .
    ldr xIP0, [xIP0, RUNTIME_REFS_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET ]

    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL

    str xIP0, [sp]    // Store ArtMethod* Runtime::callee_save_methods_[kRefsAndArgs]
    // Place sp in Thread::Current()->top_quick_frame.
    mov xIP0, sp
    str xIP0, [xSELF, # THREAD_TOP_QUICK_FRAME_OFFSET]
.endm

.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_WITH_METHOD_IN_X0
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_INTERNAL
    str x0, [sp, #0]  // Store ArtMethod* to bottom of stack.
    // Place sp in Thread::Current()->top_quick_frame.
    mov xIP0, sp
    str xIP0, [xSELF, # THREAD_TOP_QUICK_FRAME_OFFSET]
.endm

// TODO: Probably no need to restore registers preserved by aapcs64.
.macro RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    // FP args.
    ldp d0, d1, [sp, #16]
    ldp d2, d3, [sp, #32]
    ldp d4, d5, [sp, #48]
    ldp d6, d7, [sp, #64]

    // Core args.
    ldp x1, x2, [sp, #80]
    .cfi_restore x1
    .cfi_restore x2

    ldp x3, x4, [sp, #96]
    .cfi_restore x3
    .cfi_restore x4

    ldp x5, x6, [sp, #112]
    .cfi_restore x5
    .cfi_restore x6

    // x7, Callee-saves.
    ldp x7, x20, [sp, #128]
    .cfi_restore x7
    .cfi_restore x20

    ldp x21, x22, [sp, #144]
    .cfi_restore x21
    .cfi_restore x22

    ldp x23, x24, [sp, #160]
    .cfi_restore x23
    .cfi_restore x24

    ldp x25, x26, [sp, #176]
    .cfi_restore x25
    .cfi_restore x26

    ldp x27, x28, [sp, #192]
    .cfi_restore x27
    .cfi_restore x28

    // x29(callee-save) and LR.
    ldp x29, xLR, [sp, #208]
    .cfi_restore x29
    .cfi_restore x30

    add sp, sp, #224
    .cfi_adjust_cfa_offset -224
.endm

.macro RETURN_IF_RESULT_IS_ZERO
    cbnz x0, 1f                // result non-zero branch over
    ret                        // return
1:
.endm

.macro RETURN_IF_RESULT_IS_NON_ZERO
    cbz x0, 1f                 // result zero branch over
    ret                        // return
1:
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    mov x0, xSELF

    // Point of no return.
    b artDeliverPendingExceptionFromCode  // artDeliverPendingExceptionFromCode(Thread*)
    brk 0  // Unreached
.endm

.macro RETURN_OR_DELIVER_PENDING_EXCEPTION_REG reg
    ldr \reg, [xSELF, # THREAD_EXCEPTION_OFFSET]   // Get exception field.
    cbnz \reg, 1f
    ret
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_OR_DELIVER_PENDING_EXCEPTION
    RETURN_OR_DELIVER_PENDING_EXCEPTION_REG xIP0
.endm

// Same as above with x1. This is helpful in stubs that want to avoid clobbering another register.
.macro RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
    RETURN_OR_DELIVER_PENDING_EXCEPTION_REG x1
.endm

.macro RETURN_IF_W0_IS_ZERO_OR_DELIVER
    cbnz w0, 1f                // result non-zero branch over
    ret                        // return
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro NO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov x0, xSELF                     // pass Thread::Current
    b   \cxx_name                     // \cxx_name(Thread*)
END \c_name
.endm

.macro ONE_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context.
    mov x1, xSELF                     // pass Thread::Current.
    b   \cxx_name                     // \cxx_name(arg, Thread*).
    brk 0
END \c_name
.endm

.macro TWO_ARG_RUNTIME_EXCEPTION c_name, cxx_name
    .extern \cxx_name
ENTRY \c_name
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov x2, xSELF                     // pass Thread::Current
    b   \cxx_name                     // \cxx_name(arg1, arg2, Thread*)
    brk 0
END \c_name
.endm

    /*
     * Called by managed code, saves callee saves and then calls artThrowException
     * that will place a mock Method* at the bottom of the stack. Arg1 holds the exception.
     */
ONE_ARG_RUNTIME_EXCEPTION art_quick_deliver_exception, artDeliverExceptionFromCode

    /*
     * Called by managed code to create and deliver a NullPointerException.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_null_pointer_exception, artThrowNullPointerExceptionFromCode

    /*
     * Called by managed code to create and deliver an ArithmeticException.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_div_zero, artThrowDivZeroFromCode

    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException. Arg1 holds
     * index, arg2 holds limit.
     */
TWO_ARG_RUNTIME_EXCEPTION art_quick_throw_array_bounds, artThrowArrayBoundsFromCode

    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
NO_ARG_RUNTIME_EXCEPTION art_quick_throw_stack_overflow, artThrowStackOverflowFromCode

    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
ONE_ARG_RUNTIME_EXCEPTION art_quick_throw_no_such_method, artThrowNoSuchMethodFromCode

    /*
     * All generated callsites for interface invokes and invocation slow paths will load arguments
     * as usual - except instead of loading arg0/x0 with the target Method*, arg0/x0 will contain
     * the method_idx.  This wrapper will save arg1-arg3, and call the appropriate C helper.
     * NOTE: "this" is first visible argument of the target, and so can be found in arg1/x1.
     *
     * The helper will attempt to locate the target and return a 128-bit result in x0/x1 consisting
     * of the target Method* in x0 and method->code_ in x1.
     *
     * If unsuccessful, the helper will return null/????. There will be a pending exception in the
     * thread and we branch to another stub to deliver it.
     *
     * On success this wrapper will restore arguments and *jump* to the target, leaving the lr
     * pointing back to the original caller.
     *
     * Adapted from ARM32 code.
     *
     * Clobbers xIP0.
     */
.macro INVOKE_TRAMPOLINE_BODY cxx_name
    .extern \cxx_name
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME  // save callee saves in case allocation triggers GC
    // Helper signature is always
    // (method_idx, *this_object, *caller_method, *self, sp)

    mov    x2, xSELF                      // pass Thread::Current
    mov    x3, sp
    bl     \cxx_name                      // (method_idx, this, Thread*, SP)
    mov    xIP0, x1                       // save Method*->code_
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    cbz    x0, 1f                         // did we find the target? if not go to exception delivery
    br     xIP0                           // tail call to target
1:
    DELIVER_PENDING_EXCEPTION
.endm
.macro INVOKE_TRAMPOLINE c_name, cxx_name
ENTRY \c_name
    INVOKE_TRAMPOLINE_BODY \cxx_name
END \c_name
.endm

INVOKE_TRAMPOLINE art_quick_invoke_interface_trampoline_with_access_check, artInvokeInterfaceTrampolineWithAccessCheck

INVOKE_TRAMPOLINE art_quick_invoke_static_trampoline_with_access_check, artInvokeStaticTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_direct_trampoline_with_access_check, artInvokeDirectTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_super_trampoline_with_access_check, artInvokeSuperTrampolineWithAccessCheck
INVOKE_TRAMPOLINE art_quick_invoke_virtual_trampoline_with_access_check, artInvokeVirtualTrampolineWithAccessCheck


.macro INVOKE_STUB_CREATE_FRAME

SAVE_SIZE=15*8   // x4, x5, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, SP, LR, FP saved.
SAVE_SIZE_AND_METHOD=SAVE_SIZE+8


    mov x9, sp                             // Save stack pointer.
    .cfi_register sp,x9

    add x10, x2, # SAVE_SIZE_AND_METHOD    // calculate size of frame.
    sub x10, sp, x10                       // Calculate SP position - saves + ArtMethod* + args
    and x10, x10, # ~0xf                   // Enforce 16 byte stack alignment.
    mov sp, x10                            // Set new SP.

    sub x10, x9, #SAVE_SIZE                // Calculate new FP (later). Done here as we must move SP
    .cfi_def_cfa_register x10              // before this.
    .cfi_adjust_cfa_offset SAVE_SIZE

    str x28, [x10, #112]
    .cfi_rel_offset x28, 112

    stp x26, x27, [x10, #96]
    .cfi_rel_offset x26, 96
    .cfi_rel_offset x27, 104

    stp x24, x25, [x10, #80]
    .cfi_rel_offset x24, 80
    .cfi_rel_offset x25, 88

    stp x22, x23, [x10, #64]
    .cfi_rel_offset x22, 64
    .cfi_rel_offset x23, 72

    stp x20, x21, [x10, #48]
    .cfi_rel_offset x20, 48
    .cfi_rel_offset x21, 56

    stp x9, x19, [x10, #32]                // Save old stack pointer and x19.
    .cfi_rel_offset sp, 32
    .cfi_rel_offset x19, 40

    stp x4, x5, [x10, #16]                 // Save result and shorty addresses.
    .cfi_rel_offset x4, 16
    .cfi_rel_offset x5, 24

    stp xFP, xLR, [x10]                    // Store LR & FP.
    .cfi_rel_offset x29, 0
    .cfi_rel_offset x30, 8

    mov xFP, x10                           // Use xFP now, as it's callee-saved.
    .cfi_def_cfa_register x29
    mov xSELF, x3                          // Move thread pointer into SELF register.

    // Copy arguments into stack frame.
    // Use simple copy routine for now.
    // 4 bytes per slot.
    // X1 - source address
    // W2 - args length
    // X9 - destination address.
    // W10 - temporary
    add x9, sp, #8                         // Destination address is bottom of stack + null.

    // Copy parameters into the stack. Use numeric label as this is a macro and Clang's assembler
    // does not have unique-id variables.
1:
    cmp w2, #0
    beq 2f
    sub w2, w2, #4      // Need 65536 bytes of range.
    ldr w10, [x1, x2]
    str w10, [x9, x2]

    b 1b

2:
    // Store null into ArtMethod* at bottom of frame.
    str xzr, [sp]
.endm

.macro INVOKE_STUB_CALL_AND_RETURN

    // load method-> METHOD_QUICK_CODE_OFFSET
    ldr x9, [x0, #ART_METHOD_QUICK_CODE_OFFSET_64]
    // Branch to method.
    blr x9

    // Restore return value address and shorty address.
    ldp x4,x5, [xFP, #16]
    .cfi_restore x4
    .cfi_restore x5

    ldr x28, [xFP, #112]
    .cfi_restore x28

    ldp x26, x27, [xFP, #96]
    .cfi_restore x26
    .cfi_restore x27

    ldp x24, x25, [xFP, #80]
    .cfi_restore x24
    .cfi_restore x25

    ldp x22, x23, [xFP, #64]
    .cfi_restore x22
    .cfi_restore x23

    ldp x20, x21, [xFP, #48]
    .cfi_restore x20
    .cfi_restore x21

    // Store result (w0/x0/s0/d0) appropriately, depending on resultType.
    ldrb w10, [x5]

    // Check the return type and store the correct register into the jvalue in memory.
    // Use numeric label as this is a macro and Clang's assembler does not have unique-id variables.

    // Don't set anything for a void type.
    cmp w10, #'V'
    beq 3f

    // Is it a double?
    cmp w10, #'D'
    bne 1f
    str d0, [x4]
    b 3f

1:  // Is it a float?
    cmp w10, #'F'
    bne 2f
    str s0, [x4]
    b 3f

2:  // Just store x0. Doesn't matter if it is 64 or 32 bits.
    str x0, [x4]

3:  // Finish up.
    ldp x2, x19, [xFP, #32]   // Restore stack pointer and x19.
    .cfi_restore x19
    mov sp, x2
    .cfi_restore sp

    ldp xFP, xLR, [xFP]    // Restore old frame pointer and link register.
    .cfi_restore x29
    .cfi_restore x30

    ret

.endm


/*
 *  extern"C" void art_quick_invoke_stub(ArtMethod *method,   x0
 *                                       uint32_t  *args,     x1
 *                                       uint32_t argsize,    w2
 *                                       Thread *self,        x3
 *                                       JValue *result,      x4
 *                                       char   *shorty);     x5
 *  +----------------------+
 *  |                      |
 *  |  C/C++ frame         |
 *  |       LR''           |
 *  |       FP''           | <- SP'
 *  +----------------------+
 *  +----------------------+
 *  |        x28           | <- TODO: Remove callee-saves.
 *  |         :            |
 *  |        x19           |
 *  |        SP'           |
 *  |        X5            |
 *  |        X4            |        Saved registers
 *  |        LR'           |
 *  |        FP'           | <- FP
 *  +----------------------+
 *  | uint32_t out[n-1]    |
 *  |    :      :          |        Outs
 *  | uint32_t out[0]      |
 *  | ArtMethod*           | <- SP  value=null
 *  +----------------------+
 *
 * Outgoing registers:
 *  x0    - Method*
 *  x1-x7 - integer parameters.
 *  d0-d7 - Floating point parameters.
 *  xSELF = self
 *  SP = & of ArtMethod*
 *  x1 = "this" pointer.
 *
 */
ENTRY art_quick_invoke_stub
    // Spill registers as per AACPS64 calling convention.
    INVOKE_STUB_CREATE_FRAME

    // Fill registers x/w1 to x/w7 and s/d0 to s/d7 with parameters.
    // Parse the passed shorty to determine which register to load.
    // Load addresses for routines that load WXSD registers.
    adr  x11, .LstoreW2
    adr  x12, .LstoreX2
    adr  x13, .LstoreS0
    adr  x14, .LstoreD0

    // Initialize routine offsets to 0 for integers and floats.
    // x8 for integers, x15 for floating point.
    mov x8, #0
    mov x15, #0

    add x10, x5, #1         // Load shorty address, plus one to skip return value.
    ldr w1, [x9],#4         // Load "this" parameter, and increment arg pointer.

    // Loop to fill registers.
.LfillRegisters:
    ldrb w17, [x10], #1       // Load next character in signature, and increment.
    cbz w17, .LcallFunction   // Exit at end of signature. Shorty 0 terminated.

    cmp  w17, #'F' // is this a float?
    bne .LisDouble

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .Ladvance4

    add x17, x13, x15       // Calculate subroutine to jump to.
    br  x17

.LisDouble:
    cmp w17, #'D'           // is this a double?
    bne .LisLong

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .Ladvance8

    add x17, x14, x15       // Calculate subroutine to jump to.
    br x17

.LisLong:
    cmp w17, #'J'           // is this a long?
    bne .LisOther

    cmp x8, # 6*12          // Skip this load if all registers full.
    beq .Ladvance8

    add x17, x12, x8        // Calculate subroutine to jump to.
    br x17

.LisOther:                  // Everything else takes one vReg.
    cmp x8, # 6*12          // Skip this load if all registers full.
    beq .Ladvance4

    add x17, x11, x8        // Calculate subroutine to jump to.
    br x17

.Ladvance4:
    add x9, x9, #4
    b .LfillRegisters

.Ladvance8:
    add x9, x9, #8
    b .LfillRegisters

// Macro for loading a parameter into a register.
//  counter - the register with offset into these tables
//  size - the size of the register - 4 or 8 bytes.
//  register - the name of the register to be loaded.
.macro LOADREG counter size register return
    ldr \register , [x9], #\size
    add \counter, \counter, 12
    b \return
.endm

// Store ints.
.LstoreW2:
    LOADREG x8 4 w2 .LfillRegisters
    LOADREG x8 4 w3 .LfillRegisters
    LOADREG x8 4 w4 .LfillRegisters
    LOADREG x8 4 w5 .LfillRegisters
    LOADREG x8 4 w6 .LfillRegisters
    LOADREG x8 4 w7 .LfillRegisters

// Store longs.
.LstoreX2:
    LOADREG x8 8 x2 .LfillRegisters
    LOADREG x8 8 x3 .LfillRegisters
    LOADREG x8 8 x4 .LfillRegisters
    LOADREG x8 8 x5 .LfillRegisters
    LOADREG x8 8 x6 .LfillRegisters
    LOADREG x8 8 x7 .LfillRegisters

// Store singles.
.LstoreS0:
    LOADREG x15 4 s0 .LfillRegisters
    LOADREG x15 4 s1 .LfillRegisters
    LOADREG x15 4 s2 .LfillRegisters
    LOADREG x15 4 s3 .LfillRegisters
    LOADREG x15 4 s4 .LfillRegisters
    LOADREG x15 4 s5 .LfillRegisters
    LOADREG x15 4 s6 .LfillRegisters
    LOADREG x15 4 s7 .LfillRegisters

// Store doubles.
.LstoreD0:
    LOADREG x15 8 d0 .LfillRegisters
    LOADREG x15 8 d1 .LfillRegisters
    LOADREG x15 8 d2 .LfillRegisters
    LOADREG x15 8 d3 .LfillRegisters
    LOADREG x15 8 d4 .LfillRegisters
    LOADREG x15 8 d5 .LfillRegisters
    LOADREG x15 8 d6 .LfillRegisters
    LOADREG x15 8 d7 .LfillRegisters


.LcallFunction:

    INVOKE_STUB_CALL_AND_RETURN

END art_quick_invoke_stub

/*  extern"C"
 *     void art_quick_invoke_static_stub(ArtMethod *method,   x0
 *                                       uint32_t  *args,     x1
 *                                       uint32_t argsize,    w2
 *                                       Thread *self,        x3
 *                                       JValue *result,      x4
 *                                       char   *shorty);     x5
 */
ENTRY art_quick_invoke_static_stub
    // Spill registers as per AACPS64 calling convention.
    INVOKE_STUB_CREATE_FRAME

    // Fill registers x/w1 to x/w7 and s/d0 to s/d7 with parameters.
    // Parse the passed shorty to determine which register to load.
    // Load addresses for routines that load WXSD registers.
    adr  x11, .LstoreW1_2
    adr  x12, .LstoreX1_2
    adr  x13, .LstoreS0_2
    adr  x14, .LstoreD0_2

    // Initialize routine offsets to 0 for integers and floats.
    // x8 for integers, x15 for floating point.
    mov x8, #0
    mov x15, #0

    add x10, x5, #1     // Load shorty address, plus one to skip return value.

    // Loop to fill registers.
.LfillRegisters2:
    ldrb w17, [x10], #1         // Load next character in signature, and increment.
    cbz w17, .LcallFunction2    // Exit at end of signature. Shorty 0 terminated.

    cmp  w17, #'F'          // is this a float?
    bne .LisDouble2

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .Ladvance4_2

    add x17, x13, x15       // Calculate subroutine to jump to.
    br  x17

.LisDouble2:
    cmp w17, #'D'           // is this a double?
    bne .LisLong2

    cmp x15, # 8*12         // Skip this load if all registers full.
    beq .Ladvance8_2

    add x17, x14, x15       // Calculate subroutine to jump to.
    br x17

.LisLong2:
    cmp w17, #'J'           // is this a long?
    bne .LisOther2

    cmp x8, # 7*12          // Skip this load if all registers full.
    beq .Ladvance8_2

    add x17, x12, x8        // Calculate subroutine to jump to.
    br x17

.LisOther2:                 // Everything else takes one vReg.
    cmp x8, # 7*12          // Skip this load if all registers full.
    beq .Ladvance4_2

    add x17, x11, x8        // Calculate subroutine to jump to.
    br x17

.Ladvance4_2:
    add x9, x9, #4
    b .LfillRegisters2

.Ladvance8_2:
    add x9, x9, #8
    b .LfillRegisters2

// Store ints.
.LstoreW1_2:
    LOADREG x8 4 w1 .LfillRegisters2
    LOADREG x8 4 w2 .LfillRegisters2
    LOADREG x8 4 w3 .LfillRegisters2
    LOADREG x8 4 w4 .LfillRegisters2
    LOADREG x8 4 w5 .LfillRegisters2
    LOADREG x8 4 w6 .LfillRegisters2
    LOADREG x8 4 w7 .LfillRegisters2

// Store longs.
.LstoreX1_2:
    LOADREG x8 8 x1 .LfillRegisters2
    LOADREG x8 8 x2 .LfillRegisters2
    LOADREG x8 8 x3 .LfillRegisters2
    LOADREG x8 8 x4 .LfillRegisters2
    LOADREG x8 8 x5 .LfillRegisters2
    LOADREG x8 8 x6 .LfillRegisters2
    LOADREG x8 8 x7 .LfillRegisters2

// Store singles.
.LstoreS0_2:
    LOADREG x15 4 s0 .LfillRegisters2
    LOADREG x15 4 s1 .LfillRegisters2
    LOADREG x15 4 s2 .LfillRegisters2
    LOADREG x15 4 s3 .LfillRegisters2
    LOADREG x15 4 s4 .LfillRegisters2
    LOADREG x15 4 s5 .LfillRegisters2
    LOADREG x15 4 s6 .LfillRegisters2
    LOADREG x15 4 s7 .LfillRegisters2

// Store doubles.
.LstoreD0_2:
    LOADREG x15 8 d0 .LfillRegisters2
    LOADREG x15 8 d1 .LfillRegisters2
    LOADREG x15 8 d2 .LfillRegisters2
    LOADREG x15 8 d3 .LfillRegisters2
    LOADREG x15 8 d4 .LfillRegisters2
    LOADREG x15 8 d5 .LfillRegisters2
    LOADREG x15 8 d6 .LfillRegisters2
    LOADREG x15 8 d7 .LfillRegisters2


.LcallFunction2:

    INVOKE_STUB_CALL_AND_RETURN

END art_quick_invoke_static_stub



/*  extern"C" void art_quick_osr_stub(void** stack,                x0
 *                                    size_t stack_size_in_bytes,  x1
 *                                    const uin8_t* native_pc,     x2
 *                                    JValue *result,              x3
 *                                    char   *shorty,              x4
 *                                    Thread *self)                x5
 */
ENTRY art_quick_osr_stub
SAVE_SIZE=15*8   // x3, x4, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, SP, LR, FP saved.
    mov x9, sp                             // Save stack pointer.
    .cfi_register sp,x9

    sub x10, sp, # SAVE_SIZE
    and x10, x10, # ~0xf                   // Enforce 16 byte stack alignment.
    mov sp, x10                            // Set new SP.

    str x28, [sp, #112]
    stp x26, x27, [sp, #96]
    stp x24, x25, [sp, #80]
    stp x22, x23, [sp, #64]
    stp x20, x21, [sp, #48]
    stp x9, x19, [sp, #32]                // Save old stack pointer and x19.
    stp x3, x4, [sp, #16]                 // Save result and shorty addresses.
    stp xFP, xLR, [sp]                    // Store LR & FP.
    mov xSELF, x5                         // Move thread pointer into SELF register.

    sub sp, sp, #16
    str xzr, [sp]                         // Store null for ArtMethod* slot
    // Branch to stub.
    bl .Losr_entry
    add sp, sp, #16

    // Restore return value address and shorty address.
    ldp x3,x4, [sp, #16]
    ldr x28, [sp, #112]
    ldp x26, x27, [sp, #96]
    ldp x24, x25, [sp, #80]
    ldp x22, x23, [sp, #64]
    ldp x20, x21, [sp, #48]

    // Store result (w0/x0/s0/d0) appropriately, depending on resultType.
    ldrb w10, [x4]

    // Check the return type and store the correct register into the jvalue in memory.

    // Don't set anything for a void type.
    cmp w10, #'V'
    beq .Losr_exit

    // Is it a double?
    cmp w10, #'D'
    bne .Lno_double
    str d0, [x3]
    b .Losr_exit

.Lno_double:  // Is it a float?
    cmp w10, #'F'
    bne .Lno_float
    str s0, [x3]
    b .Losr_exit

.Lno_float:  // Just store x0. Doesn't matter if it is 64 or 32 bits.
    str x0, [x3]

.Losr_exit:  // Finish up.
    ldp x2, x19, [sp, #32]   // Restore stack pointer and x19.
    ldp xFP, xLR, [sp]    // Restore old frame pointer and link register.
    mov sp, x2
    ret

.Losr_entry:
    // Update stack pointer for the callee
    sub sp, sp, x1

    // Update link register slot expected by the callee.
    sub w1, w1, #8
    str lr, [sp, x1]

    // Copy arguments into stack frame.
    // Use simple copy routine for now.
    // 4 bytes per slot.
    // X0 - source address
    // W1 - args length
    // SP - destination address.
    // W10 - temporary
.Losr_loop_entry:
    cmp w1, #0
    beq .Losr_loop_exit
    sub w1, w1, #4
    ldr w10, [x0, x1]
    str w10, [sp, x1]
    b .Losr_loop_entry

.Losr_loop_exit:
    // Branch to the OSR entry point.
    br x2

END art_quick_osr_stub

    /*
     * On entry x0 is uintptr_t* gprs_ and x1 is uint64_t* fprs_
     */

ENTRY art_quick_do_long_jump
    // Load FPRs
    ldp d0, d1, [x1], #16
    ldp d2, d3, [x1], #16
    ldp d4, d5, [x1], #16
    ldp d6, d7, [x1], #16
    ldp d8, d9, [x1], #16
    ldp d10, d11, [x1], #16
    ldp d12, d13, [x1], #16
    ldp d14, d15, [x1], #16
    ldp d16, d17, [x1], #16
    ldp d18, d19, [x1], #16
    ldp d20, d21, [x1], #16
    ldp d22, d23, [x1], #16
    ldp d24, d25, [x1], #16
    ldp d26, d27, [x1], #16
    ldp d28, d29, [x1], #16
    ldp d30, d31, [x1]

    // Load GPRs
    // TODO: lots of those are smashed, could optimize.
    add x0, x0, #30*8
    ldp x30, x1, [x0], #-16          // LR & SP
    ldp x28, x29, [x0], #-16
    ldp x26, x27, [x0], #-16
    ldp x24, x25, [x0], #-16
    ldp x22, x23, [x0], #-16
    ldp x20, x21, [x0], #-16
    ldp x18, x19, [x0], #-16
    ldp x16, x17, [x0], #-16
    ldp x14, x15, [x0], #-16
    ldp x12, x13, [x0], #-16
    ldp x10, x11, [x0], #-16
    ldp x8, x9, [x0], #-16
    ldp x6, x7, [x0], #-16
    ldp x4, x5, [x0], #-16
    ldp x2, x3, [x0], #-16
    mov sp, x1

    // Need to load PC, it's at the end (after the space for the unused XZR). Use x1.
    ldr x1, [x0, #33*8]
    // And the value of x0.
    ldr x0, [x0]

    br  x1
END art_quick_do_long_jump

    /*
     * Entry from managed code that calls artLockObjectFromCode, may block for GC. x0 holds the
     * possibly null object to lock.
     *
     * Derived from arm32 code.
     */
    .extern artLockObjectFromCode
ENTRY art_quick_lock_object
    cbz    w0, .Lslow_lock
    add    x4, x0, #MIRROR_OBJECT_LOCK_WORD_OFFSET  // exclusive load/store has no immediate anymore
.Lretry_lock:
    ldr    w2, [xSELF, #THREAD_ID_OFFSET] // TODO: Can the thread ID really change during the loop?
    ldxr   w1, [x4]
    mov    x3, x1
    and    w3, w3, #LOCK_WORD_READ_BARRIER_STATE_MASK_TOGGLED  // zero the read barrier bits
    cbnz   w3, .Lnot_unlocked         // already thin locked
    // unlocked case - x1: original lock word that's zero except for the read barrier bits.
    orr    x2, x1, x2                 // x2 holds thread id with count of 0 with preserved read barrier bits
    stxr   w3, w2, [x4]
    cbnz   w3, .Llock_stxr_fail       // store failed, retry
    dmb    ishld                      // full (LoadLoad|LoadStore) memory barrier
    ret
.Lnot_unlocked:  // x1: original lock word
    lsr    w3, w1, LOCK_WORD_STATE_SHIFT
    cbnz   w3, .Lslow_lock            // if either of the top two bits are set, go slow path
    eor    w2, w1, w2                 // lock_word.ThreadId() ^ self->ThreadId()
    uxth   w2, w2                     // zero top 16 bits
    cbnz   w2, .Lslow_lock            // lock word and self thread id's match -> recursive lock
                                      // else contention, go to slow path
    mov    x3, x1                     // copy the lock word to check count overflow.
    and    w3, w3, #LOCK_WORD_READ_BARRIER_STATE_MASK_TOGGLED  // zero the read barrier bits.
    add    w2, w3, #LOCK_WORD_THIN_LOCK_COUNT_ONE  // increment count in lock word placing in w2 to check overflow
    lsr    w3, w2, LOCK_WORD_READ_BARRIER_STATE_SHIFT  // if either of the upper two bits (28-29) are set, we overflowed.
    cbnz   w3, .Lslow_lock            // if we overflow the count go slow path
    add    w2, w1, #LOCK_WORD_THIN_LOCK_COUNT_ONE  // increment count for real
    stxr   w3, w2, [x4]
    cbnz   w3, .Llock_stxr_fail       // store failed, retry
    ret
.Llock_stxr_fail:
    b      .Lretry_lock               // retry
.Lslow_lock:
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case we block
    mov    x1, xSELF                  // pass Thread::Current
    bl     artLockObjectFromCode      // (Object* obj, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_W0_IS_ZERO_OR_DELIVER
END art_quick_lock_object

ENTRY art_quick_lock_object_no_inline
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case we block
    mov    x1, xSELF                  // pass Thread::Current
    bl     artLockObjectFromCode      // (Object* obj, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_W0_IS_ZERO_OR_DELIVER
END art_quick_lock_object_no_inline

    /*
     * Entry from managed code that calls artUnlockObjectFromCode and delivers exception on failure.
     * x0 holds the possibly null object to lock.
     *
     * Derived from arm32 code.
     */
    .extern artUnlockObjectFromCode
ENTRY art_quick_unlock_object
    cbz    x0, .Lslow_unlock
    add    x4, x0, #MIRROR_OBJECT_LOCK_WORD_OFFSET  // exclusive load/store has no immediate anymore
.Lretry_unlock:
#ifndef USE_READ_BARRIER
    ldr    w1, [x4]
#else
    ldxr   w1, [x4]                   // Need to use atomic instructions for read barrier
#endif
    lsr    w2, w1, LOCK_WORD_STATE_SHIFT
    cbnz   w2, .Lslow_unlock          // if either of the top two bits are set, go slow path
    ldr    w2, [xSELF, #THREAD_ID_OFFSET]
    mov    x3, x1                     // copy lock word to check thread id equality
    and    w3, w3, #LOCK_WORD_READ_BARRIER_STATE_MASK_TOGGLED  // zero the read barrier bits
    eor    w3, w3, w2                 // lock_word.ThreadId() ^ self->ThreadId()
    uxth   w3, w3                     // zero top 16 bits
    cbnz   w3, .Lslow_unlock          // do lock word and self thread id's match?
    mov    x3, x1                     // copy lock word to detect transition to unlocked
    and    w3, w3, #LOCK_WORD_READ_BARRIER_STATE_MASK_TOGGLED  // zero the read barrier bits
    cmp    w3, #LOCK_WORD_THIN_LOCK_COUNT_ONE
    bpl    .Lrecursive_thin_unlock
    // transition to unlocked
    mov    x3, x1
    and    w3, w3, #LOCK_WORD_READ_BARRIER_STATE_MASK  // w3: zero except for the preserved read barrier bits
    dmb    ish                        // full (LoadStore|StoreStore) memory barrier
#ifndef USE_READ_BARRIER
    str    w3, [x4]
#else
    stxr   w2, w3, [x4]               // Need to use atomic instructions for read barrier
    cbnz   w2, .Lunlock_stxr_fail     // store failed, retry
#endif
    ret
.Lrecursive_thin_unlock:  // w1: original lock word
    sub    w1, w1, #LOCK_WORD_THIN_LOCK_COUNT_ONE  // decrement count
#ifndef USE_READ_BARRIER
    str    w1, [x4]
#else
    stxr   w2, w1, [x4]               // Need to use atomic instructions for read barrier
    cbnz   w2, .Lunlock_stxr_fail     // store failed, retry
#endif
    ret
.Lunlock_stxr_fail:
    b      .Lretry_unlock               // retry
.Lslow_unlock:
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case exception allocation triggers GC
    mov    x1, xSELF                  // pass Thread::Current
    bl     artUnlockObjectFromCode    // (Object* obj, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_W0_IS_ZERO_OR_DELIVER
END art_quick_unlock_object

ENTRY art_quick_unlock_object_no_inline
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case exception allocation triggers GC
    mov    x1, xSELF                  // pass Thread::Current
    bl     artUnlockObjectFromCode    // (Object* obj, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_W0_IS_ZERO_OR_DELIVER
END art_quick_unlock_object_no_inline

    /*
     * Entry from managed code that calls artIsAssignableFromCode and on failure calls
     * artThrowClassCastException.
     */
    .extern artThrowClassCastException
ENTRY art_quick_check_cast
    // Store arguments and link register
    // Stack needs to be 16B aligned on calls.
    stp x0, x1, [sp,#-32]!
    .cfi_adjust_cfa_offset 32
    .cfi_rel_offset x0, 0
    .cfi_rel_offset x1, 8
    str xLR, [sp, #24]
    .cfi_rel_offset x30, 24

    // Call runtime code
    bl artIsAssignableFromCode

    // Check for exception
    cbz x0, .Lthrow_class_cast_exception

    // Restore and return
    ldr xLR, [sp, #24]
    .cfi_restore x30
    ldp x0, x1, [sp], #32
    .cfi_restore x0
    .cfi_restore x1
    .cfi_adjust_cfa_offset -32
    ret

    .cfi_adjust_cfa_offset 32         // Reset unwind info so following code unwinds.

.Lthrow_class_cast_exception:
    // Restore
    ldr xLR, [sp, #24]
    .cfi_restore x30
    ldp x0, x1, [sp], #32
    .cfi_restore x0
    .cfi_restore x1
    .cfi_adjust_cfa_offset -32

    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME  // save all registers as basis for long jump context
    mov x2, xSELF                     // pass Thread::Current
    b artThrowClassCastException      // (Class*, Class*, Thread*)
    brk 0                             // We should not return here...
END art_quick_check_cast

// Restore xReg's value from [sp, #offset] if xReg is not the same as xExclude.
.macro POP_REG_NE xReg, offset, xExclude
    .ifnc \xReg, \xExclude
        ldr \xReg, [sp, #\offset]     // restore xReg
        .cfi_restore \xReg
    .endif
.endm

    /*
     * Macro to insert read barrier, only used in art_quick_aput_obj.
     * xDest, wDest and xObj are registers, offset is a defined literal such as
     * MIRROR_OBJECT_CLASS_OFFSET. Dest needs both x and w versions of the same register to handle
     * name mismatch between instructions. This macro uses the lower 32b of register when possible.
     * TODO: When read barrier has a fast path, add heap unpoisoning support for the fast path.
     */
.macro READ_BARRIER xDest, wDest, xObj, offset
#ifdef USE_READ_BARRIER
    // Store registers used in art_quick_aput_obj (x0-x4, LR), stack is 16B aligned.
    stp x0, x1, [sp, #-48]!
    .cfi_adjust_cfa_offset 48
    .cfi_rel_offset x0, 0
    .cfi_rel_offset x1, 8
    stp x2, x3, [sp, #16]
    .cfi_rel_offset x2, 16
    .cfi_rel_offset x3, 24
    stp x4, xLR, [sp, #32]
    .cfi_rel_offset x4, 32
    .cfi_rel_offset x30, 40

    // mov x0, \xRef                // pass ref in x0 (no-op for now since parameter ref is unused)
    .ifnc \xObj, x1
        mov x1, \xObj               // pass xObj
    .endif
    mov w2, #\offset                // pass offset
    bl artReadBarrierSlow           // artReadBarrierSlow(ref, xObj, offset)
    // No need to unpoison return value in w0, artReadBarrierSlow() would do the unpoisoning.
    .ifnc \wDest, w0
        mov \wDest, w0              // save return value in wDest
    .endif

    // Conditionally restore saved registers
    POP_REG_NE x0, 0, \xDest
    POP_REG_NE x1, 8, \xDest
    POP_REG_NE x2, 16, \xDest
    POP_REG_NE x3, 24, \xDest
    POP_REG_NE x4, 32, \xDest
    ldr xLR, [sp, #40]
    .cfi_restore x30
    add sp, sp, #48
    .cfi_adjust_cfa_offset -48
#else
    ldr \wDest, [\xObj, #\offset]   // Heap reference = 32b. This also zero-extends to \xDest.
    UNPOISON_HEAP_REF \wDest
#endif  // USE_READ_BARRIER
.endm

    /*
     * Entry from managed code for array put operations of objects where the value being stored
     * needs to be checked for compatibility.
     * x0 = array, x1 = index, x2 = value
     *
     * Currently all values should fit into w0/w1/w2, and w1 always will as indices are 32b. We
     * assume, though, that the upper 32b are zeroed out. At least for x1/w1 we can do better by
     * using index-zero-extension in load/stores.
     *
     * Temporaries: x3, x4
     * TODO: x4 OK? ip seems wrong here.
     */
ENTRY art_quick_aput_obj_with_null_and_bound_check
    tst x0, x0
    bne art_quick_aput_obj_with_bound_check
    b art_quick_throw_null_pointer_exception
END art_quick_aput_obj_with_null_and_bound_check

ENTRY art_quick_aput_obj_with_bound_check
    ldr w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]
    cmp w3, w1
    bhi art_quick_aput_obj
    mov x0, x1
    mov x1, x3
    b art_quick_throw_array_bounds
END art_quick_aput_obj_with_bound_check

#ifdef USE_READ_BARRIER
    .extern artReadBarrierSlow
#endif
ENTRY art_quick_aput_obj
    cbz x2, .Ldo_aput_null
    READ_BARRIER x3, w3, x0, MIRROR_OBJECT_CLASS_OFFSET     // Heap reference = 32b
                                                         // This also zero-extends to x3
    READ_BARRIER x4, w4, x2, MIRROR_OBJECT_CLASS_OFFSET     // Heap reference = 32b
                                                         // This also zero-extends to x4
    READ_BARRIER x3, w3, x3, MIRROR_CLASS_COMPONENT_TYPE_OFFSET // Heap reference = 32b
                                                         // This also zero-extends to x3
    cmp w3, w4  // value's type == array's component type - trivial assignability
    bne .Lcheck_assignability
.Ldo_aput:
    add x3, x0, #MIRROR_OBJECT_ARRAY_DATA_OFFSET
                                                         // "Compress" = do nothing
    POISON_HEAP_REF w2
    str w2, [x3, x1, lsl #2]                             // Heap reference = 32b
    ldr x3, [xSELF, #THREAD_CARD_TABLE_OFFSET]
    lsr x0, x0, #7
    strb w3, [x3, x0]
    ret
.Ldo_aput_null:
    add x3, x0, #MIRROR_OBJECT_ARRAY_DATA_OFFSET
                                                         // "Compress" = do nothing
    str w2, [x3, x1, lsl #2]                             // Heap reference = 32b
    ret
.Lcheck_assignability:
    // Store arguments and link register
    stp x0, x1, [sp,#-32]!
    .cfi_adjust_cfa_offset 32
    .cfi_rel_offset x0, 0
    .cfi_rel_offset x1, 8
    stp x2, xLR, [sp, #16]
    .cfi_rel_offset x2, 16
    .cfi_rel_offset x30, 24

    // Call runtime code
    mov x0, x3              // Heap reference, 32b, "uncompress" = do nothing, already zero-extended
    mov x1, x4              // Heap reference, 32b, "uncompress" = do nothing, already zero-extended
    bl artIsAssignableFromCode

    // Check for exception
    cbz x0, .Lthrow_array_store_exception

    // Restore
    ldp x2, x30, [sp, #16]
    .cfi_restore x2
    .cfi_restore x30
    ldp x0, x1, [sp], #32
    .cfi_restore x0
    .cfi_restore x1
    .cfi_adjust_cfa_offset -32

    add x3, x0, #MIRROR_OBJECT_ARRAY_DATA_OFFSET
                                                          // "Compress" = do nothing
    POISON_HEAP_REF w2
    str w2, [x3, x1, lsl #2]                              // Heap reference = 32b
    ldr x3, [xSELF, #THREAD_CARD_TABLE_OFFSET]
    lsr x0, x0, #7
    strb w3, [x3, x0]
    ret
    .cfi_adjust_cfa_offset 32  // 4 restores after cbz for unwinding.
.Lthrow_array_store_exception:
    ldp x2, x30, [sp, #16]
    .cfi_restore x2
    .cfi_restore x30
    ldp x0, x1, [sp], #32
    .cfi_restore x0
    .cfi_restore x1
    .cfi_adjust_cfa_offset -32

    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    mov x1, x2                    // Pass value.
    mov x2, xSELF                 // Pass Thread::Current.
    b artThrowArrayStoreException // (Object*, Object*, Thread*).
    brk 0                         // Unreached.
END art_quick_aput_obj

// Macro to facilitate adding new allocation entrypoints.
.macro ONE_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME // save callee saves in case of GC
    mov    x1, xSELF                  // pass Thread::Current
    bl     \entrypoint                // (uint32_t type_idx, Method* method, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
END \name
.endm

// Macro to facilitate adding new allocation entrypoints.
.macro TWO_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME // save callee saves in case of GC
    mov    x2, xSELF                  // pass Thread::Current
    bl     \entrypoint                // (uint32_t type_idx, Method* method, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
END \name
.endm

// Macro to facilitate adding new allocation entrypoints.
.macro THREE_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME // save callee saves in case of GC
    mov    x3, xSELF                  // pass Thread::Current
    bl     \entrypoint
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
END \name
.endm

// Macro to facilitate adding new allocation entrypoints.
.macro FOUR_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME // save callee saves in case of GC
    mov    x4, xSELF                  // pass Thread::Current
    bl     \entrypoint                //
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
    DELIVER_PENDING_EXCEPTION
END \name
.endm

// Macros taking opportunity of code similarities for downcalls with referrer.
.macro ONE_ARG_REF_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case of GC
    ldr    x1, [sp, #FRAME_SIZE_REFS_ONLY_CALLEE_SAVE] // Load referrer
    mov    x2, xSELF                  // pass Thread::Current
    bl     \entrypoint                // (uint32_t type_idx, Method* method, Thread*, SP)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
END \name
.endm

.macro TWO_ARG_REF_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case of GC
    ldr    x2, [sp, #FRAME_SIZE_REFS_ONLY_CALLEE_SAVE] // Load referrer
    mov    x3, xSELF                  // pass Thread::Current
    bl     \entrypoint
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
END \name
.endm

.macro THREE_ARG_REF_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case of GC
    ldr    x3, [sp, #FRAME_SIZE_REFS_ONLY_CALLEE_SAVE] // Load referrer
    mov    x4, xSELF                  // pass Thread::Current
    bl     \entrypoint
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    \return
END \name
.endm

.macro RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
    cbz w0, 1f                 // result zero branch over
    ret                        // return
1:
    DELIVER_PENDING_EXCEPTION
.endm

    /*
     * Entry from managed code that calls artHandleFillArrayDataFromCode and delivers exception on
     * failure.
     */
TWO_ARG_REF_DOWNCALL art_quick_handle_fill_data, artHandleFillArrayDataFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER

    /*
     * Entry from managed code when uninitialized static storage, this stub will run the class
     * initializer and deliver the exception on error. On success the static storage base is
     * returned.
     */
ONE_ARG_DOWNCALL art_quick_initialize_static_storage, artInitializeStaticStorageFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

ONE_ARG_DOWNCALL art_quick_initialize_type, artInitializeTypeFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
ONE_ARG_DOWNCALL art_quick_initialize_type_and_verify_access, artInitializeTypeAndVerifyAccessFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

ONE_ARG_REF_DOWNCALL art_quick_get_boolean_static, artGetBooleanStaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
ONE_ARG_REF_DOWNCALL art_quick_get_byte_static, artGetByteStaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
ONE_ARG_REF_DOWNCALL art_quick_get_char_static, artGetCharStaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
ONE_ARG_REF_DOWNCALL art_quick_get_short_static, artGetShortStaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
ONE_ARG_REF_DOWNCALL art_quick_get32_static, artGet32StaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
ONE_ARG_REF_DOWNCALL art_quick_get64_static, artGet64StaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
ONE_ARG_REF_DOWNCALL art_quick_get_obj_static, artGetObjStaticFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1

TWO_ARG_REF_DOWNCALL art_quick_get_boolean_instance, artGetBooleanInstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
TWO_ARG_REF_DOWNCALL art_quick_get_byte_instance, artGetByteInstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
TWO_ARG_REF_DOWNCALL art_quick_get_char_instance, artGetCharInstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
TWO_ARG_REF_DOWNCALL art_quick_get_short_instance, artGetShortInstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
TWO_ARG_REF_DOWNCALL art_quick_get32_instance, artGet32InstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
TWO_ARG_REF_DOWNCALL art_quick_get64_instance, artGet64InstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1
TWO_ARG_REF_DOWNCALL art_quick_get_obj_instance, artGetObjInstanceFromCode, RETURN_OR_DELIVER_PENDING_EXCEPTION_X1

TWO_ARG_REF_DOWNCALL art_quick_set8_static, artSet8StaticFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
TWO_ARG_REF_DOWNCALL art_quick_set16_static, artSet16StaticFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
TWO_ARG_REF_DOWNCALL art_quick_set32_static, artSet32StaticFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
TWO_ARG_REF_DOWNCALL art_quick_set_obj_static, artSetObjStaticFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER

THREE_ARG_REF_DOWNCALL art_quick_set8_instance, artSet8InstanceFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
THREE_ARG_REF_DOWNCALL art_quick_set16_instance, artSet16InstanceFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
THREE_ARG_REF_DOWNCALL art_quick_set32_instance, artSet32InstanceFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
THREE_ARG_REF_DOWNCALL art_quick_set64_instance, artSet64InstanceFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER
THREE_ARG_REF_DOWNCALL art_quick_set_obj_instance, artSetObjInstanceFromCode, RETURN_IF_W0_IS_ZERO_OR_DELIVER

// This is separated out as the argument order is different.
    .extern artSet64StaticFromCode
ENTRY art_quick_set64_static
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME  // save callee saves in case of GC
    ldr    x1, [sp, #FRAME_SIZE_REFS_ONLY_CALLEE_SAVE] // Load referrer
                                      // x2 contains the parameter
    mov    x3, xSELF                  // pass Thread::Current
    bl     artSet64StaticFromCode
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_W0_IS_ZERO_OR_DELIVER
END art_quick_set64_static

    /*
     * Entry from managed code to resolve a string, this stub will allocate a String and deliver an
     * exception on error. On success the String is returned. w0 holds the string index. The fast
     * path check for hit in strings cache has already been performed.
     */
ONE_ARG_DOWNCALL art_quick_resolve_string, artResolveStringFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

// Generate the allocation entrypoints for each allocator.
GENERATE_ALLOC_ENTRYPOINTS_FOR_EACH_ALLOCATOR

// A hand-written override for GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_rosalloc, RosAlloc).
ENTRY art_quick_alloc_object_rosalloc
    // Fast path rosalloc allocation.
    // x0: type_idx/return value, x1: ArtMethod*, xSELF(x19): Thread::Current
    // x2-x7: free.
    ldr    x2, [x1, #ART_METHOD_DEX_CACHE_TYPES_OFFSET_64]    // Load dex cache resolved types array
                                                              // Load the class (x2)
    ldr    w2, [x2, x0, lsl #COMPRESSED_REFERENCE_SIZE_SHIFT]
    cbz    x2, .Lart_quick_alloc_object_rosalloc_slow_path    // Check null class
                                                              // Check class status.
    ldr    w3, [x2, #MIRROR_CLASS_STATUS_OFFSET]
    cmp    x3, #MIRROR_CLASS_STATUS_INITIALIZED
    bne    .Lart_quick_alloc_object_rosalloc_slow_path
                                                              // Add a fake dependence from the
                                                              // following access flag and size
                                                              // loads to the status load.
                                                              // This is to prevent those loads
                                                              // from being reordered above the
                                                              // status load and reading wrong
                                                              // values (an alternative is to use
                                                              // a load-acquire for the status).
    eor    x3, x3, x3
    add    x2, x2, x3
                                                              // Check access flags has
                                                              // kAccClassIsFinalizable
    ldr    w3, [x2, #MIRROR_CLASS_ACCESS_FLAGS_OFFSET]
    tst    x3, #ACCESS_FLAGS_CLASS_IS_FINALIZABLE
    bne    .Lart_quick_alloc_object_rosalloc_slow_path
    ldr    x3, [xSELF, #THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET]  // Check if the thread local
                                                              // allocation stack has room.
                                                              // ldp won't work due to large offset.
    ldr    x4, [xSELF, #THREAD_LOCAL_ALLOC_STACK_END_OFFSET]
    cmp    x3, x4
    bhs    .Lart_quick_alloc_object_rosalloc_slow_path
    ldr    w3, [x2, #MIRROR_CLASS_OBJECT_SIZE_OFFSET]         // Load the object size (x3)
    cmp    x3, #ROSALLOC_MAX_THREAD_LOCAL_BRACKET_SIZE        // Check if the size is for a thread
                                                              // local allocation
    bhs    .Lart_quick_alloc_object_rosalloc_slow_path
                                                              // Compute the rosalloc bracket index
                                                              // from the size.
                                                              // Align up the size by the rosalloc
                                                              // bracket quantum size and divide
                                                              // by the quantum size and subtract
                                                              // by 1. This code is a shorter but
                                                              // equivalent version.
    sub    x3, x3, #1
    lsr    x3, x3, #ROSALLOC_BRACKET_QUANTUM_SIZE_SHIFT
                                                              // Load the rosalloc run (x4)
    add    x4, xSELF, x3, lsl #POINTER_SIZE_SHIFT
    ldr    x4, [x4, #THREAD_ROSALLOC_RUNS_OFFSET]
                                                              // Load the free list head (x3). This
                                                              // will be the return val.
    ldr    x3, [x4, #(ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_HEAD_OFFSET)]
    cbz    x3, .Lart_quick_alloc_object_rosalloc_slow_path
    // "Point of no slow path". Won't go to the slow path from here on. OK to clobber x0 and x1.
    ldr    x1, [x3, #ROSALLOC_SLOT_NEXT_OFFSET]               // Load the next pointer of the head
                                                              // and update the list head with the
                                                              // next pointer.
    str    x1, [x4, #(ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_HEAD_OFFSET)]
                                                              // Store the class pointer in the
                                                              // header. This also overwrites the
                                                              // next pointer. The offsets are
                                                              // asserted to match.
#if ROSALLOC_SLOT_NEXT_OFFSET != MIRROR_OBJECT_CLASS_OFFSET
#error "Class pointer needs to overwrite next pointer."
#endif
    POISON_HEAP_REF w2
    str    w2, [x3, #MIRROR_OBJECT_CLASS_OFFSET]
                                                              // Fence. This is "ish" not "ishst" so
                                                              // that it also ensures ordering of
                                                              // the class status load with respect
                                                              // to later accesses to the class
                                                              // object. Alternatively we could use
                                                              // "ishst" if we use load-acquire for
                                                              // the class status load.)
                                                              // Needs to be done before pushing on
                                                              // allocation since Heap::VisitObjects
                                                              // relies on seeing the class pointer.
                                                              // b/28790624
    dmb    ish
                                                              // Push the new object onto the thread
                                                              // local allocation stack and
                                                              // increment the thread local
                                                              // allocation stack top.
    ldr    x1, [xSELF, #THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET]
    str    w3, [x1], #COMPRESSED_REFERENCE_SIZE               // (Increment x1 as a side effect.)
    str    x1, [xSELF, #THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET]
                                                              // Decrement the size of the free list
    ldr    w1, [x4, #(ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_SIZE_OFFSET)]
    sub    x1, x1, #1
                                                              // TODO: consider combining this store
                                                              // and the list head store above using
                                                              // strd.
    str    w1, [x4, #(ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_SIZE_OFFSET)]

    mov    x0, x3                                             // Set the return value and return.
    ret
.Lart_quick_alloc_object_rosalloc_slow_path:
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME      // save callee saves in case of GC
    mov    x2, xSELF                       // pass Thread::Current
    bl     artAllocObjectFromCodeRosAlloc  // (uint32_t type_idx, Method* method, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
END art_quick_alloc_object_rosalloc

// The common fast path code for art_quick_alloc_object_tlab and art_quick_alloc_object_region_tlab.
//
// x0: type_idx/return value, x1: ArtMethod*, x2: Class*, xSELF(x19): Thread::Current
// x3-x7: free.
// Need to preserve x0 and x1 to the slow path.
.macro ALLOC_OBJECT_TLAB_FAST_PATH slowPathLabel
    cbz    x2, \slowPathLabel                                 // Check null class
                                                              // Check class status.
    ldr    w3, [x2, #MIRROR_CLASS_STATUS_OFFSET]
    cmp    x3, #MIRROR_CLASS_STATUS_INITIALIZED
    bne    \slowPathLabel
                                                              // Add a fake dependence from the
                                                              // following access flag and size
                                                              // loads to the status load.
                                                              // This is to prevent those loads
                                                              // from being reordered above the
                                                              // status load and reading wrong
                                                              // values (an alternative is to use
                                                              // a load-acquire for the status).
    eor    x3, x3, x3
    add    x2, x2, x3
                                                              // Check access flags has
                                                              // kAccClassIsFinalizable.
    ldr    w3, [x2, #MIRROR_CLASS_ACCESS_FLAGS_OFFSET]
    tbnz   x3, #ACCESS_FLAGS_CLASS_IS_FINALIZABLE_BIT, \slowPathLabel
                                                              // Load thread_local_pos (x4) and
                                                              // thread_local_end (x5).
    ldr    x4, [xSELF, #THREAD_LOCAL_POS_OFFSET]
    ldr    x5, [xSELF, #THREAD_LOCAL_END_OFFSET]
    sub    x6, x5, x4                                         // Compute the remaining buf size.
    ldr    w7, [x2, #MIRROR_CLASS_OBJECT_SIZE_OFFSET]         // Load the object size (x7).
    cmp    x7, x6                                             // Check if it fits. OK to do this
                                                              // before rounding up the object size
                                                              // assuming the buf size alignment.
    bhi    \slowPathLabel
    // "Point of no slow path". Won't go to the slow path from here on. OK to clobber x0 and x1.
                                                              // Round up the object size by the
                                                              // object alignment. (addr + 7) & ~7.
    add    x7, x7, #OBJECT_ALIGNMENT_MASK
    and    x7, x7, #OBJECT_ALIGNMENT_MASK_TOGGLED
                                                              // Move old thread_local_pos to x0
                                                              // for the return value.
    mov    x0, x4
    add    x5, x0, x7
    str    x5, [xSELF, #THREAD_LOCAL_POS_OFFSET]              // Store new thread_local_pos.
    ldr    x5, [xSELF, #THREAD_LOCAL_OBJECTS_OFFSET]          // Increment thread_local_objects.
    add    x5, x5, #1
    str    x5, [xSELF, #THREAD_LOCAL_OBJECTS_OFFSET]
    POISON_HEAP_REF w2
    str    w2, [x0, #MIRROR_OBJECT_CLASS_OFFSET]              // Store the class pointer.
                                                              // Fence. This is "ish" not "ishst" so
                                                              // that the code after this allocation
                                                              // site will see the right values in
                                                              // the fields of the class.
                                                              // Alternatively we could use "ishst"
                                                              // if we use load-acquire for the
                                                              // class status load.)
    dmb    ish
    ret
.endm

// A hand-written override for GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_tlab, TLAB).
ENTRY art_quick_alloc_object_tlab
    // Fast path tlab allocation.
    // x0: type_idx/return value, x1: ArtMethod*, xSELF(x19): Thread::Current
    // x2-x7: free.
#if defined(USE_READ_BARRIER)
    mvn    x0, xzr                                            // Read barrier not supported here.
    ret                                                       // Return -1.
#endif
    ldr    x2, [x1, #ART_METHOD_DEX_CACHE_TYPES_OFFSET_64]    // Load dex cache resolved types array
                                                              // Load the class (x2)
    ldr    w2, [x2, x0, lsl #COMPRESSED_REFERENCE_SIZE_SHIFT]
    ALLOC_OBJECT_TLAB_FAST_PATH .Lart_quick_alloc_object_tlab_slow_path
.Lart_quick_alloc_object_tlab_slow_path:
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    // Save callee saves in case of GC.
    mov    x2, xSELF                     // Pass Thread::Current.
    bl     artAllocObjectFromCodeTLAB    // (uint32_t type_idx, Method* method, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
END art_quick_alloc_object_tlab

// A hand-written override for GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_region_tlab, RegionTLAB)
ENTRY art_quick_alloc_object_region_tlab
    // Fast path region tlab allocation.
    // x0: type_idx/return value, x1: ArtMethod*, xSELF(x19): Thread::Current
    // x2-x7: free.
#if !defined(USE_READ_BARRIER)
    mvn    x0, xzr                                            // Read barrier must be enabled here.
    ret                                                       // Return -1.
#endif
    ldr    x2, [x1, #ART_METHOD_DEX_CACHE_TYPES_OFFSET_64]    // Load dex cache resolved types array
                                                              // Load the class (x2)
    ldr    w2, [x2, x0, lsl #COMPRESSED_REFERENCE_SIZE_SHIFT]
                                                              // Read barrier for class load.
    ldr    w3, [xSELF, #THREAD_IS_GC_MARKING_OFFSET]
    cbnz   x3, .Lart_quick_alloc_object_region_tlab_class_load_read_barrier_slow_path
.Lart_quick_alloc_object_region_tlab_class_load_read_barrier_slow_path_exit:
    ALLOC_OBJECT_TLAB_FAST_PATH .Lart_quick_alloc_object_region_tlab_slow_path
.Lart_quick_alloc_object_region_tlab_class_load_read_barrier_slow_path:
                                                              // The read barrier slow path. Mark
                                                              // the class.
    stp    x0, x1, [sp, #-32]!                                // Save registers (x0, x1, lr).
    str    xLR, [sp, #16]                                     // Align sp by 16 bytes.
    mov    x0, x2                                             // Pass the class as the first param.
    bl     artReadBarrierMark
    mov    x2, x0                                             // Get the (marked) class back.
    ldp    x0, x1, [sp, #0]                                   // Restore registers.
    ldr    xLR, [sp, #16]
    add    sp, sp, #32
    b      .Lart_quick_alloc_object_region_tlab_class_load_read_barrier_slow_path_exit
.Lart_quick_alloc_object_region_tlab_slow_path:
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME          // Save callee saves in case of GC.
    mov    x2, xSELF                           // Pass Thread::Current.
    bl     artAllocObjectFromCodeRegionTLAB    // (uint32_t type_idx, Method* method, Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
END art_quick_alloc_object_region_tlab

    /*
     * Called by managed code when the thread has been asked to suspend.
     */
    .extern artTestSuspendFromCode
ENTRY art_quick_test_suspend
    ldrh   w0, [xSELF, #THREAD_FLAGS_OFFSET]  // get xSELF->state_and_flags.as_struct.flags
    cbnz   w0, .Lneed_suspend                 // check flags == 0
    ret                                       // return if flags == 0
.Lneed_suspend:
    mov    x0, xSELF
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME          // save callee saves for stack crawl
    bl     artTestSuspendFromCode             // (Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
END art_quick_test_suspend

ENTRY art_quick_implicit_suspend
    mov    x0, xSELF
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME          // save callee saves for stack crawl
    bl     artTestSuspendFromCode             // (Thread*)
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
END art_quick_implicit_suspend

     /*
     * Called by managed code that is attempting to call a method on a proxy class. On entry
     * x0 holds the proxy method and x1 holds the receiver; The frame size of the invoked proxy
     * method agrees with a ref and args callee save frame.
     */
     .extern artQuickProxyInvokeHandler
ENTRY art_quick_proxy_invoke_handler
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_WITH_METHOD_IN_X0
    mov     x2, xSELF                   // pass Thread::Current
    mov     x3, sp                      // pass SP
    bl      artQuickProxyInvokeHandler  // (Method* proxy method, receiver, Thread*, SP)
    ldr     x2, [xSELF, THREAD_EXCEPTION_OFFSET]
    cbnz    x2, .Lexception_in_proxy    // success if no exception is pending
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME // Restore frame
    fmov    d0, x0                      // Store result in d0 in case it was float or double
    ret                                 // return on success
.Lexception_in_proxy:
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    DELIVER_PENDING_EXCEPTION
END art_quick_proxy_invoke_handler

    /*
     * Called to resolve an imt conflict.
     * x0 is the conflict ArtMethod.
     * xIP1 is a hidden argument that holds the target interface method's dex method index.
     *
     * Note that this stub writes to xIP0, xIP1, and x0.
     */
    .extern artInvokeInterfaceTrampoline
ENTRY art_quick_imt_conflict_trampoline
    ldr xIP0, [sp, #0]  // Load referrer
    ldr xIP0, [xIP0, #ART_METHOD_DEX_CACHE_METHODS_OFFSET_64]   // Load dex cache methods array
    ldr xIP0, [xIP0, xIP1, lsl #POINTER_SIZE_SHIFT]  // Load interface method
    ldr xIP1, [x0, #ART_METHOD_JNI_OFFSET_64]  // Load ImtConflictTable
    ldr x0, [xIP1]  // Load first entry in ImtConflictTable.
.Limt_table_iterate:
    cmp x0, xIP0
    // Branch if found. Benchmarks have shown doing a branch here is better.
    beq .Limt_table_found
    // If the entry is null, the interface method is not in the ImtConflictTable.
    cbz x0, .Lconflict_trampoline
    // Iterate over the entries of the ImtConflictTable.
    ldr x0, [xIP1, #(2 * __SIZEOF_POINTER__)]!
    b .Limt_table_iterate
.Limt_table_found:
    // We successfully hit an entry in the table. Load the target method
    // and jump to it.
    ldr x0, [xIP1, #__SIZEOF_POINTER__]
    ldr xIP0, [x0, #ART_METHOD_QUICK_CODE_OFFSET_64]
    br xIP0
.Lconflict_trampoline:
    // Call the runtime stub to populate the ImtConflictTable and jump to the
    // resolved method.
    INVOKE_TRAMPOLINE_BODY artInvokeInterfaceTrampoline
END art_quick_imt_conflict_trampoline

ENTRY art_quick_resolution_trampoline
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    mov x2, xSELF
    mov x3, sp
    bl artQuickResolutionTrampoline  // (called, receiver, Thread*, SP)
    cbz x0, 1f
    mov xIP0, x0            // Remember returned code pointer in xIP0.
    ldr x0, [sp, #0]        // artQuickResolutionTrampoline puts called method in *SP.
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    br xIP0
1:
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    DELIVER_PENDING_EXCEPTION
END art_quick_resolution_trampoline

/*
 * Generic JNI frame layout:
 *
 * #-------------------#
 * |                   |
 * | caller method...  |
 * #-------------------#    <--- SP on entry
 * | Return X30/LR     |
 * | X29/FP            |    callee save
 * | X28               |    callee save
 * | X27               |    callee save
 * | X26               |    callee save
 * | X25               |    callee save
 * | X24               |    callee save
 * | X23               |    callee save
 * | X22               |    callee save
 * | X21               |    callee save
 * | X20               |    callee save
 * | X19               |    callee save
 * | X7                |    arg7
 * | X6                |    arg6
 * | X5                |    arg5
 * | X4                |    arg4
 * | X3                |    arg3
 * | X2                |    arg2
 * | X1                |    arg1
 * | D7                |    float arg 8
 * | D6                |    float arg 7
 * | D5                |    float arg 6
 * | D4                |    float arg 5
 * | D3                |    float arg 4
 * | D2                |    float arg 3
 * | D1                |    float arg 2
 * | D0                |    float arg 1
 * | Method*           | <- X0
 * #-------------------#
 * | local ref cookie  | // 4B
 * | handle scope size | // 4B
 * #-------------------#
 * | JNI Call Stack    |
 * #-------------------#    <--- SP on native call
 * |                   |
 * | Stack for Regs    |    The trampoline assembly will pop these values
 * |                   |    into registers for native call
 * #-------------------#
 * | Native code ptr   |
 * #-------------------#
 * | Free scratch      |
 * #-------------------#
 * | Ptr to (1)        |    <--- SP
 * #-------------------#
 */
    /*
     * Called to do a generic JNI down-call
     */
ENTRY art_quick_generic_jni_trampoline
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_WITH_METHOD_IN_X0

    // Save SP , so we can have static CFI info.
    mov x28, sp
    .cfi_def_cfa_register x28

    // This looks the same, but is different: this will be updated to point to the bottom
    // of the frame when the handle scope is inserted.
    mov xFP, sp

    mov xIP0, #5120
    sub sp, sp, xIP0

    // prepare for artQuickGenericJniTrampoline call
    // (Thread*,  SP)
    //    x0      x1   <= C calling convention
    //   xSELF    xFP  <= where they are

    mov x0, xSELF   // Thread*
    mov x1, xFP
    bl artQuickGenericJniTrampoline  // (Thread*, sp)

    // The C call will have registered the complete save-frame on success.
    // The result of the call is:
    // x0: pointer to native code, 0 on error.
    // x1: pointer to the bottom of the used area of the alloca, can restore stack till there.

    // Check for error = 0.
    cbz x0, .Lexception_in_native

    // Release part of the alloca.
    mov sp, x1

    // Save the code pointer
    mov xIP0, x0

    // Load parameters from frame into registers.
    // TODO Check with artQuickGenericJniTrampoline.
    //      Also, check again APPCS64 - the stack arguments are interleaved.
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]

    ldp d0, d1, [sp, #64]
    ldp d2, d3, [sp, #80]
    ldp d4, d5, [sp, #96]
    ldp d6, d7, [sp, #112]

    add sp, sp, #128

    blr xIP0        // native call.

    // result sign extension is handled in C code
    // prepare for artQuickGenericJniEndTrampoline call
    // (Thread*, result, result_f)
    //    x0       x1       x2        <= C calling convention
    mov x1, x0      // Result (from saved).
    mov x0, xSELF   // Thread register.
    fmov x2, d0     // d0 will contain floating point result, but needs to go into x2

    bl artQuickGenericJniEndTrampoline

    // Pending exceptions possible.
    ldr x2, [xSELF, THREAD_EXCEPTION_OFFSET]
    cbnz x2, .Lexception_in_native

    // Tear down the alloca.
    mov sp, x28
    .cfi_def_cfa_register sp

    // Tear down the callee-save frame.
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME

    // store into fpr, for when it's a fpr return...
    fmov d0, x0
    ret

.Lexception_in_native:
    // Move to x1 then sp to please assembler.
    ldr x1, [xSELF, # THREAD_TOP_QUICK_FRAME_OFFSET]
    mov sp, x1
    .cfi_def_cfa_register sp
    # This will create a new save-all frame, required by the runtime.
    DELIVER_PENDING_EXCEPTION
END art_quick_generic_jni_trampoline

/*
 * Called to bridge from the quick to interpreter ABI. On entry the arguments match those
 * of a quick call:
 * x0 = method being called/to bridge to.
 * x1..x7, d0..d7 = arguments to that method.
 */
ENTRY art_quick_to_interpreter_bridge
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME   // Set up frame and save arguments.

    //  x0 will contain mirror::ArtMethod* method.
    mov x1, xSELF                          // How to get Thread::Current() ???
    mov x2, sp

    // uint64_t artQuickToInterpreterBridge(mirror::ArtMethod* method, Thread* self,
    //                                      mirror::ArtMethod** sp)
    bl   artQuickToInterpreterBridge

    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME  // TODO: no need to restore arguments in this case.

    fmov d0, x0

    RETURN_OR_DELIVER_PENDING_EXCEPTION
END art_quick_to_interpreter_bridge


//
// Instrumentation-related stubs
//
    .extern artInstrumentationMethodEntryFromCode
ENTRY art_quick_instrumentation_entry
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME

    mov   x20, x0             // Preserve method reference in a callee-save.

    mov   x2, xSELF
    mov   x3, xLR
    bl    artInstrumentationMethodEntryFromCode  // (Method*, Object*, Thread*, LR)

    mov   xIP0, x0            // x0 = result of call.
    mov   x0, x20             // Reload method reference.

    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME  // Note: will restore xSELF
    adr   xLR, art_quick_instrumentation_exit
    br    xIP0                // Tail-call method with lr set to art_quick_instrumentation_exit.
END art_quick_instrumentation_entry

    .extern artInstrumentationMethodExitFromCode
ENTRY art_quick_instrumentation_exit
    mov   xLR, #0             // Clobber LR for later checks.

    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME

    // We need to save x0 and d0. We could use a callee-save from SETUP_REF_ONLY, but then
    // we would need to fully restore it. As there are a lot of callee-save registers, it seems
    // easier to have an extra small stack area.

    str x0, [sp, #-16]!       // Save integer result.
    .cfi_adjust_cfa_offset 16
    str d0,  [sp, #8]         // Save floating-point result.

    add   x1, sp, #16         // Pass SP.
    mov   x2, x0              // Pass integer result.
    fmov  x3, d0              // Pass floating-point result.
    mov   x0, xSELF           // Pass Thread.
    bl   artInstrumentationMethodExitFromCode    // (Thread*, SP, gpr_res, fpr_res)

    mov   xIP0, x0            // Return address from instrumentation call.
    mov   xLR, x1             // r1 is holding link register if we're to bounce to deoptimize

    ldr   d0, [sp, #8]        // Restore floating-point result.
    ldr   x0, [sp], 16        // Restore integer result, and drop stack area.
    .cfi_adjust_cfa_offset 16

    POP_REFS_ONLY_CALLEE_SAVE_FRAME

    br    xIP0                // Tail-call out.
END art_quick_instrumentation_exit

    /*
     * Instrumentation has requested that we deoptimize into the interpreter. The deoptimization
     * will long jump to the upcall with a special exception of -1.
     */
    .extern artDeoptimize
ENTRY art_quick_deoptimize
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    mov    x0, xSELF          // Pass thread.
    bl     artDeoptimize      // artDeoptimize(Thread*)
    brk 0
END art_quick_deoptimize

    /*
     * Compiled code has requested that we deoptimize into the interpreter. The deoptimization
     * will long jump to the upcall with a special exception of -1.
     */
    .extern artDeoptimizeFromCompiledCode
ENTRY art_quick_deoptimize_from_compiled_code
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    mov    x0, xSELF                      // Pass thread.
    bl     artDeoptimizeFromCompiledCode  // artDeoptimizeFromCompiledCode(Thread*)
    brk 0
END art_quick_deoptimize_from_compiled_code


    /*
     * String's indexOf.
     *
     * TODO: Not very optimized.
     * On entry:
     *    x0:   string object (known non-null)
     *    w1:   char to match (known <= 0xFFFF)
     *    w2:   Starting offset in string data
     */
ENTRY art_quick_indexof
    ldr   w3, [x0, #MIRROR_STRING_COUNT_OFFSET]
    add   x0, x0, #MIRROR_STRING_VALUE_OFFSET

    /* Clamp start to [0..count] */
    cmp   w2, #0
    csel  w2, wzr, w2, lt
    cmp   w2, w3
    csel  w2, w3, w2, gt

    /* Save a copy to compute result */
    mov   x5, x0

    /* Build pointer to start of data to compare and pre-bias */
    add   x0, x0, x2, lsl #1
    sub   x0, x0, #2

    /* Compute iteration count */
    sub   w2, w3, w2

    /*
     * At this point we have:
     *  x0: start of the data to test
     *  w1: char to compare
     *  w2: iteration count
     *  x5: original start of string data
     */

    subs  w2, w2, #4
    b.lt  .Lindexof_remainder

.Lindexof_loop4:
    ldrh  w6, [x0, #2]!
    ldrh  w7, [x0, #2]!
    ldrh  wIP0, [x0, #2]!
    ldrh  wIP1, [x0, #2]!
    cmp   w6, w1
    b.eq  .Lmatch_0
    cmp   w7, w1
    b.eq  .Lmatch_1
    cmp   wIP0, w1
    b.eq  .Lmatch_2
    cmp   wIP1, w1
    b.eq  .Lmatch_3
    subs  w2, w2, #4
    b.ge  .Lindexof_loop4

.Lindexof_remainder:
    adds  w2, w2, #4
    b.eq  .Lindexof_nomatch

.Lindexof_loop1:
    ldrh  w6, [x0, #2]!
    cmp   w6, w1
    b.eq  .Lmatch_3
    subs  w2, w2, #1
    b.ne  .Lindexof_loop1

.Lindexof_nomatch:
    mov   x0, #-1
    ret

.Lmatch_0:
    sub   x0, x0, #6
    sub   x0, x0, x5
    asr   x0, x0, #1
    ret
.Lmatch_1:
    sub   x0, x0, #4
    sub   x0, x0, x5
    asr   x0, x0, #1
    ret
.Lmatch_2:
    sub   x0, x0, #2
    sub   x0, x0, x5
    asr   x0, x0, #1
    ret
.Lmatch_3:
    sub   x0, x0, x5
    asr   x0, x0, #1
    ret
END art_quick_indexof

   /*
     * String's compareTo.
     *
     * TODO: Not very optimized.
     *
     * On entry:
     *    x0:   this object pointer
     *    x1:   comp object pointer
     *
     */
    .extern __memcmp16
ENTRY art_quick_string_compareto
    mov    x2, x0         // x0 is return, use x2 for first input.
    sub    x0, x2, x1     // Same string object?
    cbnz   x0,1f
    ret
1:                        // Different string objects.

    ldr    w4, [x2, #MIRROR_STRING_COUNT_OFFSET]
    ldr    w3, [x1, #MIRROR_STRING_COUNT_OFFSET]
    add    x2, x2, #MIRROR_STRING_VALUE_OFFSET
    add    x1, x1, #MIRROR_STRING_VALUE_OFFSET

    /*
     * Now:           Data*  Count
     *    first arg    x2      w4
     *   second arg    x1      w3
     */

    // x0 := str1.length(w4) - str2.length(w3). ldr zero-extended w3/w4 into x3/x4.
    subs x0, x4, x3
    // Min(count1, count2) into w3.
    csel x3, x3, x4, ge

    // TODO: Tune this value.
    // Check for long string, do memcmp16 for them.
    cmp w3, #28  // Constant from arm32.
    bgt .Ldo_memcmp16

    /*
     * Now:
     *   x2: *first string data
     *   x1: *second string data
     *   w3: iteration count
     *   x0: return value if comparison equal
     *   x4, x5, x6, x7: free
     */

    // Do a simple unrolled loop.
.Lloop:
    // At least two more elements?
    subs w3, w3, #2
    b.lt .Lremainder_or_done

    ldrh w4, [x2], #2
    ldrh w5, [x1], #2

    ldrh w6, [x2], #2
    ldrh w7, [x1], #2

    subs w4, w4, w5
    b.ne .Lw4_result

    subs w6, w6, w7
    b.ne .Lw6_result

    b .Lloop

.Lremainder_or_done:
    adds w3, w3, #1
    b.eq .Lremainder
    ret

.Lremainder:
    ldrh w4, [x2], #2
    ldrh w5, [x1], #2
    subs w4, w4, w5
    b.ne .Lw4_result
    ret

// Result is in w4
.Lw4_result:
    sxtw x0, w4
    ret

// Result is in w6
.Lw6_result:
    sxtw x0, w6
    ret

.Ldo_memcmp16:
    mov x14, x0                  // Save x0 and LR. __memcmp16 does not use these temps.
    mov x15, xLR                 //                 TODO: Codify and check that?

    mov x0, x2
    uxtw x2, w3
    bl __memcmp16

    mov xLR, x15                 // Restore LR.

    cmp x0, #0                   // Check the memcmp difference.
    csel x0, x0, x14, ne         // x0 := x0 != 0 ? x14(prev x0=length diff) : x1.
    ret
END art_quick_string_compareto
