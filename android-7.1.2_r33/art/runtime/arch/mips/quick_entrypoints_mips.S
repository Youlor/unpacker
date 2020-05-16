/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "asm_support_mips.S"

#include "arch/quick_alloc_entrypoints.S"

    .set noreorder
    .balign 4

    /* Deliver the given exception */
    .extern artDeliverExceptionFromCode
    /* Deliver an exception pending on a thread */
    .extern artDeliverPendingExceptionFromCode

#define ARG_SLOT_SIZE   32    // space for a0-a3 plus 4 more words

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kSaveAll)
     * Callee-save: $s0-$s8 + $gp + $ra, 11 total + 1 word for Method*
     * Clobbers $t0 and $sp
     * Allocates ARG_SLOT_SIZE bytes at the bottom of the stack for arg slots.
     * Reserves FRAME_SIZE_SAVE_ALL_CALLEE_SAVE + ARG_SLOT_SIZE bytes on the stack
     */
.macro SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, -96
    .cfi_adjust_cfa_offset 96

     // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_SAVE_ALL_CALLEE_SAVE != 96)
#error "SAVE_ALL_CALLEE_SAVE_FRAME(MIPS) size not as expected."
#endif

    sw     $ra, 92($sp)
    .cfi_rel_offset 31, 92
    sw     $s8, 88($sp)
    .cfi_rel_offset 30, 88
    sw     $gp, 84($sp)
    .cfi_rel_offset 28, 84
    sw     $s7, 80($sp)
    .cfi_rel_offset 23, 80
    sw     $s6, 76($sp)
    .cfi_rel_offset 22, 76
    sw     $s5, 72($sp)
    .cfi_rel_offset 21, 72
    sw     $s4, 68($sp)
    .cfi_rel_offset 20, 68
    sw     $s3, 64($sp)
    .cfi_rel_offset 19, 64
    sw     $s2, 60($sp)
    .cfi_rel_offset 18, 60
    sw     $s1, 56($sp)
    .cfi_rel_offset 17, 56
    sw     $s0, 52($sp)
    .cfi_rel_offset 16, 52

    SDu $f30, $f31, 44, $sp, $t1
    SDu $f28, $f29, 36, $sp, $t1
    SDu $f26, $f27, 28, $sp, $t1
    SDu $f24, $f25, 20, $sp, $t1
    SDu $f22, $f23, 12, $sp, $t1
    SDu $f20, $f21, 4,  $sp, $t1

    # 1 word for holding Method*

    lw $t0, %got(_ZN3art7Runtime9instance_E)($gp)
    lw $t0, 0($t0)
    lw $t0, RUNTIME_SAVE_ALL_CALLEE_SAVE_FRAME_OFFSET($t0)
    sw $t0, 0($sp)                                # Place Method* at bottom of stack.
    sw $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
    addiu  $sp, $sp, -ARG_SLOT_SIZE               # reserve argument slots on the stack
    .cfi_adjust_cfa_offset ARG_SLOT_SIZE
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsOnly). Restoration assumes non-moving GC.
     * Does not include rSUSPEND or rSELF
     * callee-save: $s2-$s8 + $gp + $ra, 9 total + 2 words padding + 1 word to hold Method*
     * Clobbers $t0 and $sp
     * Allocates ARG_SLOT_SIZE bytes at the bottom of the stack for arg slots.
     * Reserves FRAME_SIZE_REFS_ONLY_CALLEE_SAVE + ARG_SLOT_SIZE bytes on the stack
     */
.macro SETUP_REFS_ONLY_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, -48
    .cfi_adjust_cfa_offset 48

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_REFS_ONLY_CALLEE_SAVE != 48)
#error "REFS_ONLY_CALLEE_SAVE_FRAME(MIPS) size not as expected."
#endif

    sw     $ra, 44($sp)
    .cfi_rel_offset 31, 44
    sw     $s8, 40($sp)
    .cfi_rel_offset 30, 40
    sw     $gp, 36($sp)
    .cfi_rel_offset 28, 36
    sw     $s7, 32($sp)
    .cfi_rel_offset 23, 32
    sw     $s6, 28($sp)
    .cfi_rel_offset 22, 28
    sw     $s5, 24($sp)
    .cfi_rel_offset 21, 24
    sw     $s4, 20($sp)
    .cfi_rel_offset 20, 20
    sw     $s3, 16($sp)
    .cfi_rel_offset 19, 16
    sw     $s2, 12($sp)
    .cfi_rel_offset 18, 12
    # 2 words for alignment and bottom word will hold Method*

    lw $t0, %got(_ZN3art7Runtime9instance_E)($gp)
    lw $t0, 0($t0)
    lw $t0, RUNTIME_REFS_ONLY_CALLEE_SAVE_FRAME_OFFSET($t0)
    sw $t0, 0($sp)                                # Place Method* at bottom of stack.
    sw $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
    addiu  $sp, $sp, -ARG_SLOT_SIZE               # reserve argument slots on the stack
    .cfi_adjust_cfa_offset ARG_SLOT_SIZE
.endm

.macro RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, ARG_SLOT_SIZE                # remove argument slots on the stack
    .cfi_adjust_cfa_offset -ARG_SLOT_SIZE
    lw     $ra, 44($sp)
    .cfi_restore 31
    lw     $s8, 40($sp)
    .cfi_restore 30
    lw     $gp, 36($sp)
    .cfi_restore 28
    lw     $s7, 32($sp)
    .cfi_restore 23
    lw     $s6, 28($sp)
    .cfi_restore 22
    lw     $s5, 24($sp)
    .cfi_restore 21
    lw     $s4, 20($sp)
    .cfi_restore 20
    lw     $s3, 16($sp)
    .cfi_restore 19
    lw     $s2, 12($sp)
    .cfi_restore 18
    addiu  $sp, $sp, 48
    .cfi_adjust_cfa_offset -48
.endm

.macro RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    jalr   $zero, $ra
    nop
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs).
     * callee-save: $a1-$a3, $s2-$s8 + $gp + $ra, 12 total + 3 words padding + method*
     */
.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_REGISTERS_ONLY
    addiu  $sp, $sp, -80
    .cfi_adjust_cfa_offset 80

    // Ugly compile-time check, but we only have the preprocessor.
#if (FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE != 80)
#error "REFS_AND_ARGS_CALLEE_SAVE_FRAME(MIPS) size not as expected."
#endif

    sw     $ra, 76($sp)
    .cfi_rel_offset 31, 76
    sw     $s8, 72($sp)
    .cfi_rel_offset 30, 72
    sw     $gp, 68($sp)
    .cfi_rel_offset 28, 68
    sw     $s7, 64($sp)
    .cfi_rel_offset 23, 64
    sw     $s6, 60($sp)
    .cfi_rel_offset 22, 60
    sw     $s5, 56($sp)
    .cfi_rel_offset 21, 56
    sw     $s4, 52($sp)
    .cfi_rel_offset 20, 52
    sw     $s3, 48($sp)
    .cfi_rel_offset 19, 48
    sw     $s2, 44($sp)
    .cfi_rel_offset 18, 44
    sw     $a3, 40($sp)
    .cfi_rel_offset 7, 40
    sw     $a2, 36($sp)
    .cfi_rel_offset 6, 36
    sw     $a1, 32($sp)
    .cfi_rel_offset 5, 32
    SDu $f14, $f15, 24, $sp, $t0
    SDu $f12, $f13, 16, $sp, $t0
    # bottom will hold Method*
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs). Restoration assumes non-moving GC.
     * callee-save: $a1-$a3, $f12-$f15, $s2-$s8 + $gp + $ra, 12 total + 3 words padding + method*
     * Clobbers $t0 and $sp
     * Allocates ARG_SLOT_SIZE bytes at the bottom of the stack for arg slots.
     * Reserves FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE + ARG_SLOT_SIZE bytes on the stack
     */
.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_REGISTERS_ONLY
    lw $t0, %got(_ZN3art7Runtime9instance_E)($gp)
    lw $t0, 0($t0)
    lw $t0, RUNTIME_REFS_AND_ARGS_CALLEE_SAVE_FRAME_OFFSET($t0)
    sw $t0, 0($sp)                                # Place Method* at bottom of stack.
    sw $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
    addiu  $sp, $sp, -ARG_SLOT_SIZE               # reserve argument slots on the stack
    .cfi_adjust_cfa_offset ARG_SLOT_SIZE
.endm

    /*
     * Macro that sets up the callee save frame to conform with
     * Runtime::CreateCalleeSaveMethod(kRefsAndArgs). Restoration assumes non-moving GC.
     * callee-save: $a1-$a3, $f12-$f15, $s2-$s8 + $gp + $ra, 12 total + 3 words padding + method*
     * Clobbers $sp
     * Use $a0 as the Method* and loads it into bottom of stack.
     * Allocates ARG_SLOT_SIZE bytes at the bottom of the stack for arg slots.
     * Reserves FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE + ARG_SLOT_SIZE bytes on the stack
     */
.macro SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_WITH_METHOD_IN_A0
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_REGISTERS_ONLY
    sw $a0, 0($sp)                                # Place Method* at bottom of stack.
    sw $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)  # Place sp in Thread::Current()->top_quick_frame.
    addiu  $sp, $sp, -ARG_SLOT_SIZE               # reserve argument slots on the stack
    .cfi_adjust_cfa_offset ARG_SLOT_SIZE
.endm

.macro RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    addiu  $sp, $sp, ARG_SLOT_SIZE                # remove argument slots on the stack
    .cfi_adjust_cfa_offset -ARG_SLOT_SIZE
    lw     $ra, 76($sp)
    .cfi_restore 31
    lw     $s8, 72($sp)
    .cfi_restore 30
    lw     $gp, 68($sp)
    .cfi_restore 28
    lw     $s7, 64($sp)
    .cfi_restore 23
    lw     $s6, 60($sp)
    .cfi_restore 22
    lw     $s5, 56($sp)
    .cfi_restore 21
    lw     $s4, 52($sp)
    .cfi_restore 20
    lw     $s3, 48($sp)
    .cfi_restore 19
    lw     $s2, 44($sp)
    .cfi_restore 18
    lw     $a3, 40($sp)
    .cfi_restore 7
    lw     $a2, 36($sp)
    .cfi_restore 6
    lw     $a1, 32($sp)
    .cfi_restore 5
    LDu $f14, $f15, 24, $sp, $t1
    LDu $f12, $f13, 16, $sp, $t1
    addiu  $sp, $sp, 80           # pop frame
    .cfi_adjust_cfa_offset -80
.endm

    /*
     * Macro that set calls through to artDeliverPendingExceptionFromCode, where the pending
     * exception is Thread::Current()->exception_
     */
.macro DELIVER_PENDING_EXCEPTION
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME     # save callee saves for throw
    la      $t9, artDeliverPendingExceptionFromCode
    jalr    $zero, $t9                   # artDeliverPendingExceptionFromCode(Thread*)
    move    $a0, rSELF                   # pass Thread::Current
.endm

.macro RETURN_IF_NO_EXCEPTION
    lw     $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    bnez   $t0, 1f                       # success if no exception is pending
    nop
    jalr   $zero, $ra
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_IF_ZERO
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    bnez   $v0, 1f                       # success?
    nop
    jalr   $zero, $ra                    # return on success
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

.macro RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME
    beqz   $v0, 1f                       # success?
    nop
    jalr   $zero, $ra                    # return on success
    nop
1:
    DELIVER_PENDING_EXCEPTION
.endm

    /*
     * On stack replacement stub.
     * On entry:
     *   a0 = stack to copy
     *   a1 = size of stack
     *   a2 = pc to call
     *   a3 = JValue* result
     *   [sp + 16] = shorty
     *   [sp + 20] = thread
     */
ENTRY art_quick_osr_stub
    // Save callee general purpose registers, RA and GP.
    addiu  $sp, $sp, -48
    .cfi_adjust_cfa_offset 48
    sw     $ra, 44($sp)
    .cfi_rel_offset 31, 44
    sw     $s8, 40($sp)
    .cfi_rel_offset 30, 40
    sw     $gp, 36($sp)
    .cfi_rel_offset 28, 36
    sw     $s7, 32($sp)
    .cfi_rel_offset 23, 32
    sw     $s6, 28($sp)
    .cfi_rel_offset 22, 28
    sw     $s5, 24($sp)
    .cfi_rel_offset 21, 24
    sw     $s4, 20($sp)
    .cfi_rel_offset 20, 20
    sw     $s3, 16($sp)
    .cfi_rel_offset 19, 16
    sw     $s2, 12($sp)
    .cfi_rel_offset 18, 12
    sw     $s1, 8($sp)
    .cfi_rel_offset 17, 8
    sw     $s0, 4($sp)
    .cfi_rel_offset 16, 4

    move   $s8, $sp                        # Save the stack pointer
    move   $s7, $a1                        # Save size of stack
    move   $s6, $a2                        # Save the pc to call
    lw     rSELF, 48+20($sp)               # Save managed thread pointer into rSELF
    addiu  $t0, $sp, -12                   # Reserve space for stack pointer,
                                           #    JValue* result, and ArtMethod* slot.
    srl    $t0, $t0, 4                     # Align stack pointer to 16 bytes
    sll    $sp, $t0, 4                     # Update stack pointer
    sw     $s8, 4($sp)                     # Save old stack pointer
    sw     $a3, 8($sp)                     # Save JValue* result
    sw     $zero, 0($sp)                   # Store null for ArtMethod* at bottom of frame
    subu   $sp, $a1                        # Reserve space for callee stack
    move   $a2, $a1
    move   $a1, $a0
    move   $a0, $sp
    la     $t9, memcpy
    jalr   $t9                             # memcpy (dest a0, src a1, bytes a2)
    addiu  $sp, $sp, -16                   # make space for argument slots for memcpy
    bal    .Losr_entry                     # Call the method
    addiu  $sp, $sp, 16                    # restore stack after memcpy
    lw     $a2, 8($sp)                     # Restore JValue* result
    lw     $sp, 4($sp)                     # Restore saved stack pointer
    lw     $a0, 48+16($sp)                 # load shorty
    lbu    $a0, 0($a0)                     # load return type
    li     $a1, 'D'                        # put char 'D' into a1
    beq    $a0, $a1, .Losr_fp_result       # Test if result type char == 'D'
    li     $a1, 'F'                        # put char 'F' into a1
    beq    $a0, $a1, .Losr_fp_result       # Test if result type char == 'F'
    nop
    sw     $v0, 0($a2)
    b      .Losr_exit
    sw     $v1, 4($a2)                     # store v0/v1 into result
.Losr_fp_result:
    SDu    $f0, $f1, 0, $a2, $t0           # store f0/f1 into result
.Losr_exit:
    lw     $ra, 44($sp)
    .cfi_restore 31
    lw     $s8, 40($sp)
    .cfi_restore 30
    lw     $gp, 36($sp)
    .cfi_restore 28
    lw     $s7, 32($sp)
    .cfi_restore 23
    lw     $s6, 28($sp)
    .cfi_restore 22
    lw     $s5, 24($sp)
    .cfi_restore 21
    lw     $s4, 20($sp)
    .cfi_restore 20
    lw     $s3, 16($sp)
    .cfi_restore 19
    lw     $s2, 12($sp)
    .cfi_restore 18
    lw     $s1, 8($sp)
    .cfi_restore 17
    lw     $s0, 4($sp)
    .cfi_restore 16
    jalr   $zero, $ra
    addiu  $sp, $sp, 48
    .cfi_adjust_cfa_offset -48
.Losr_entry:
    addiu  $s7, $s7, -4
    addu   $t0, $s7, $sp
    move   $t9, $s6
    jalr   $zero, $t9
    sw     $ra, 0($t0)                     # Store RA per the compiler ABI
END art_quick_osr_stub

    /*
     * On entry $a0 is uint32_t* gprs_ and $a1 is uint32_t* fprs_
     * FIXME: just guessing about the shape of the jmpbuf.  Where will pc be?
     */
ENTRY art_quick_do_long_jump
    LDu  $f0,  $f1,   0*8, $a1, $t1
    LDu  $f2,  $f3,   1*8, $a1, $t1
    LDu  $f4,  $f5,   2*8, $a1, $t1
    LDu  $f6,  $f7,   3*8, $a1, $t1
    LDu  $f8,  $f9,   4*8, $a1, $t1
    LDu  $f10, $f11,  5*8, $a1, $t1
    LDu  $f12, $f13,  6*8, $a1, $t1
    LDu  $f14, $f15,  7*8, $a1, $t1
    LDu  $f16, $f17,  8*8, $a1, $t1
    LDu  $f18, $f19,  9*8, $a1, $t1
    LDu  $f20, $f21, 10*8, $a1, $t1
    LDu  $f22, $f23, 11*8, $a1, $t1
    LDu  $f24, $f25, 12*8, $a1, $t1
    LDu  $f26, $f27, 13*8, $a1, $t1
    LDu  $f28, $f29, 14*8, $a1, $t1
    LDu  $f30, $f31, 15*8, $a1, $t1

    .set push
    .set nomacro
    .set noat
    lw      $at, 4($a0)
    .set pop
    lw      $v0, 8($a0)
    lw      $v1, 12($a0)
    lw      $a1, 20($a0)
    lw      $a2, 24($a0)
    lw      $a3, 28($a0)
    lw      $t0, 32($a0)
    lw      $t1, 36($a0)
    lw      $t2, 40($a0)
    lw      $t3, 44($a0)
    lw      $t4, 48($a0)
    lw      $t5, 52($a0)
    lw      $t6, 56($a0)
    lw      $t7, 60($a0)
    lw      $s0, 64($a0)
    lw      $s1, 68($a0)
    lw      $s2, 72($a0)
    lw      $s3, 76($a0)
    lw      $s4, 80($a0)
    lw      $s5, 84($a0)
    lw      $s6, 88($a0)
    lw      $s7, 92($a0)
    lw      $t8, 96($a0)
    lw      $t9, 100($a0)
    lw      $gp, 112($a0)
    lw      $sp, 116($a0)
    lw      $fp, 120($a0)
    lw      $ra, 124($a0)
    lw      $a0, 16($a0)
    move    $v0, $zero          # clear result registers v0 and v1 (in branch delay slot)
    jalr    $zero, $t9          # do long jump
    move    $v1, $zero
END art_quick_do_long_jump

    /*
     * Called by managed code, saves most registers (forms basis of long jump context) and passes
     * the bottom of the stack. artDeliverExceptionFromCode will place the callee save Method* at
     * the bottom of the thread. On entry a0 holds Throwable*
     */
ENTRY art_quick_deliver_exception
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artDeliverExceptionFromCode
    jalr $zero, $t9                 # artDeliverExceptionFromCode(Throwable*, Thread*)
    move $a1, rSELF                 # pass Thread::Current
END art_quick_deliver_exception

    /*
     * Called by managed code to create and deliver a NullPointerException
     */
    .extern artThrowNullPointerExceptionFromCode
ENTRY art_quick_throw_null_pointer_exception
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artThrowNullPointerExceptionFromCode
    jalr $zero, $t9                 # artThrowNullPointerExceptionFromCode(Thread*)
    move $a0, rSELF                 # pass Thread::Current
END art_quick_throw_null_pointer_exception

    /*
     * Called by managed code to create and deliver an ArithmeticException
     */
    .extern artThrowDivZeroFromCode
ENTRY art_quick_throw_div_zero
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artThrowDivZeroFromCode
    jalr $zero, $t9                 # artThrowDivZeroFromCode(Thread*)
    move $a0, rSELF                 # pass Thread::Current
END art_quick_throw_div_zero

    /*
     * Called by managed code to create and deliver an ArrayIndexOutOfBoundsException
     */
    .extern artThrowArrayBoundsFromCode
ENTRY art_quick_throw_array_bounds
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artThrowArrayBoundsFromCode
    jalr $zero, $t9                 # artThrowArrayBoundsFromCode(index, limit, Thread*)
    move $a2, rSELF                 # pass Thread::Current
END art_quick_throw_array_bounds

    /*
     * Called by managed code to create and deliver a StackOverflowError.
     */
    .extern artThrowStackOverflowFromCode
ENTRY art_quick_throw_stack_overflow
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artThrowStackOverflowFromCode
    jalr $zero, $t9                 # artThrowStackOverflowFromCode(Thread*)
    move $a0, rSELF                 # pass Thread::Current
END art_quick_throw_stack_overflow

    /*
     * Called by managed code to create and deliver a NoSuchMethodError.
     */
    .extern artThrowNoSuchMethodFromCode
ENTRY art_quick_throw_no_such_method
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artThrowNoSuchMethodFromCode
    jalr $zero, $t9                 # artThrowNoSuchMethodFromCode(method_idx, Thread*)
    move $a1, rSELF                 # pass Thread::Current
END art_quick_throw_no_such_method

    /*
     * All generated callsites for interface invokes and invocation slow paths will load arguments
     * as usual - except instead of loading arg0/$a0 with the target Method*, arg0/$a0 will contain
     * the method_idx.  This wrapper will save arg1-arg3, and call the appropriate C helper.
     * NOTE: "this" is first visable argument of the target, and so can be found in arg1/$a1.
     *
     * The helper will attempt to locate the target and return a 64-bit result in $v0/$v1 consisting
     * of the target Method* in $v0 and method->code_ in $v1.
     *
     * If unsuccessful, the helper will return null/null. There will be a pending exception in the
     * thread and we branch to another stub to deliver it.
     *
     * On success this wrapper will restore arguments and *jump* to the target, leaving the lr
     * pointing back to the original caller.
     */
.macro INVOKE_TRAMPOLINE_BODY cxx_name
    .extern \cxx_name
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME  # save callee saves in case allocation triggers GC
    move  $a2, rSELF                       # pass Thread::Current
    la    $t9, \cxx_name
    jalr  $t9                              # (method_idx, this, Thread*, $sp)
    addiu $a3, $sp, ARG_SLOT_SIZE          # pass $sp (remove arg slots)
    move  $a0, $v0                         # save target Method*
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    beqz  $v0, 1f
    move  $t9, $v1                         # save $v0->code_
    jalr  $zero, $t9
    nop
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

.macro LOAD_WORD_TO_REG reg, next_arg, index, label
    lw    $\reg, -4($\next_arg)   # next_arg points to argument after the current one (offset is 4)
    b     \label
    addiu $\index, 1
.endm

.macro LOAD_LONG_TO_REG reg1, reg2, next_arg, index, label
    lw    $\reg1, -8($\next_arg)  # next_arg points to argument after the current one (offset is 8)
    lw    $\reg2, -4($\next_arg)
    b     \label
    li    $\index, 4              # long can be loaded only to a2_a3 pair so index will be always 4
.endm

.macro LOAD_FLOAT_TO_REG reg, next_arg, index, label
    lwc1  $\reg, -4($\next_arg)   # next_arg points to argument after the current one (offset is 4)
    b     \label
    addiu $\index, 1
.endm

.macro LOAD_DOUBLE_TO_REG reg1, reg2, next_arg, index, tmp, label
    LDu  $\reg1, $\reg2, -8, $\next_arg, $\tmp  # next_arg points to argument after the current one
                                                # (offset is 8)
    b     \label
    addiu $\index, 1
.endm

#define SPILL_SIZE    32

    /*
     * Invocation stub for quick code.
     * On entry:
     *   a0 = method pointer
     *   a1 = argument array or null for no argument methods
     *   a2 = size of argument array in bytes
     *   a3 = (managed) thread pointer
     *   [sp + 16] = JValue* result
     *   [sp + 20] = shorty
     */
ENTRY art_quick_invoke_stub
    sw    $a0, 0($sp)           # save out a0
    addiu $sp, $sp, -SPILL_SIZE # spill s0, s1, fp, ra and gp
    .cfi_adjust_cfa_offset SPILL_SIZE
    sw    $gp, 16($sp)
    sw    $ra, 12($sp)
    .cfi_rel_offset 31, 12
    sw    $fp, 8($sp)
    .cfi_rel_offset 30, 8
    sw    $s1, 4($sp)
    .cfi_rel_offset 17, 4
    sw    $s0, 0($sp)
    .cfi_rel_offset 16, 0
    move  $fp, $sp              # save sp in fp
    .cfi_def_cfa_register 30
    move  $s1, $a3              # move managed thread pointer into s1
    addiu $s0, $zero, SUSPEND_CHECK_INTERVAL  # reset s0 to suspend check interval
    addiu $t0, $a2, 4           # create space for ArtMethod* in frame.
    subu  $t0, $sp, $t0         # reserve & align *stack* to 16 bytes:
    srl   $t0, $t0, 4           #   native calling convention only aligns to 8B,
    sll   $sp, $t0, 4           #   so we have to ensure ART 16B alignment ourselves.
    addiu $a0, $sp, 4           # pass stack pointer + ArtMethod* as dest for memcpy
    la    $t9, memcpy
    jalr  $t9                   # (dest, src, bytes)
    addiu $sp, $sp, -16         # make space for argument slots for memcpy
    addiu $sp, $sp, 16          # restore stack after memcpy
    lw    $gp, 16($fp)          # restore $gp
    lw    $a0, SPILL_SIZE($fp)  # restore ArtMethod*
    lw    $a1, 4($sp)           # a1 = this*
    addiu $t0, $sp, 8           # t0 = pointer to the current argument (skip ArtMethod* and this*)
    li    $t3, 2                # t3 = gpr_index = 2 (skip A0 and A1)
    move  $t4, $zero            # t4 = fp_index = 0
    lw    $t1, 20 + SPILL_SIZE($fp)  # get shorty (20 is offset from the $sp on entry + SPILL_SIZE
                                # as the $fp is SPILL_SIZE bytes below the $sp on entry)
    addiu $t1, 1                # t1 = shorty + 1 (skip 1 for return type)
loop:
    lbu   $t2, 0($t1)           # t2 = shorty[i]
    beqz  $t2, loopEnd          # finish getting args when shorty[i] == '\0'
    addiu $t1, 1

    li    $t9, 'J'              # put char 'J' into t9
    beq   $t9, $t2, isLong      # branch if result type char == 'J'
    li    $t9, 'D'              # put char 'D' into t9
    beq   $t9, $t2, isDouble    # branch if result type char == 'D'
    li    $t9, 'F'              # put char 'F' into t9
    beq   $t9, $t2, isSingle    # branch if result type char == 'F'
    addiu $t0, 4                # next_arg = curr_arg + 4 (in branch delay slot,
                                # for both, int and single)

    li    $t5, 2                                   # skip a0 and a1 (ArtMethod* and this*)
    bne   $t5, $t3, 1f                             # if (gpr_index == 2)
    addiu $t5, 1
    LOAD_WORD_TO_REG a2, t0, t3, loop              #   a2 = current argument, gpr_index++
1:  bne   $t5, $t3, loop                           # else if (gpr_index == 3)
    nop
    LOAD_WORD_TO_REG a3, t0, t3, loop              #   a3 = current argument, gpr_index++

isLong:
    addiu $t0, 8                                   # next_arg = curr_arg + 8
    slti  $t5, $t3, 3
    beqz  $t5, 2f                                  # if (gpr_index < 3)
    nop
    LOAD_LONG_TO_REG a2, a3, t0, t3, loop          #   a2_a3 = curr_arg, gpr_index = 4
2:  b     loop                                     # else
    li    $t3, 4                                   #   gpr_index = 4

isDouble:
    addiu $t0, 8                                   # next_arg = curr_arg + 8
    li    $t5, 0
    bne   $t5, $t4, 3f                             # if (fp_index == 0)
    addiu $t5, 1
    LOAD_DOUBLE_TO_REG f12, f13, t0, t4, t9, loop  #   f12_f13 = curr_arg, fp_index++
3:  bne   $t5, $t4, loop                           # else if (fp_index == 1)
    nop
    LOAD_DOUBLE_TO_REG f14, f15, t0, t4, t9, loop  #   f14_f15 = curr_arg, fp_index++

isSingle:
    li    $t5, 0
    bne   $t5, $t4, 4f                             # if (fp_index == 0)
    addiu $t5, 1
    LOAD_FLOAT_TO_REG f12, t0, t4, loop            #   f12 = curr_arg, fp_index++
4:  bne   $t5, $t4, loop                           # else if (fp_index == 1)
    nop
    LOAD_FLOAT_TO_REG f14, t0, t4, loop            #   f14 = curr_arg, fp_index++

loopEnd:
    lw    $t9, ART_METHOD_QUICK_CODE_OFFSET_32($a0)  # get pointer to the code
    jalr  $t9                   # call the method
    sw    $zero, 0($sp)         # store null for ArtMethod* at bottom of frame
    move  $sp, $fp              # restore the stack
    lw    $s0, 0($sp)
    .cfi_restore 16
    lw    $s1, 4($sp)
    .cfi_restore 17
    lw    $fp, 8($sp)
    .cfi_restore 30
    lw    $ra, 12($sp)
    .cfi_restore 31
    addiu $sp, $sp, SPILL_SIZE
    .cfi_adjust_cfa_offset -SPILL_SIZE
    lw    $t0, 16($sp)          # get result pointer
    lw    $t1, 20($sp)          # get shorty
    lb    $t1, 0($t1)           # get result type char
    li    $t2, 'D'              # put char 'D' into t2
    beq   $t1, $t2, 5f          # branch if result type char == 'D'
    li    $t3, 'F'              # put char 'F' into t3
    beq   $t1, $t3, 5f          # branch if result type char == 'F'
    sw    $v0, 0($t0)           # store the result
    jalr  $zero, $ra
    sw    $v1, 4($t0)           # store the other half of the result
5:
    SDu   $f0, $f1, 0, $t0, $t1 # store floating point result
    jalr  $zero, $ra
    nop
END art_quick_invoke_stub

    /*
     * Invocation static stub for quick code.
     * On entry:
     *   a0 = method pointer
     *   a1 = argument array or null for no argument methods
     *   a2 = size of argument array in bytes
     *   a3 = (managed) thread pointer
     *   [sp + 16] = JValue* result
     *   [sp + 20] = shorty
     */
ENTRY art_quick_invoke_static_stub
    sw    $a0, 0($sp)           # save out a0
    addiu $sp, $sp, -SPILL_SIZE # spill s0, s1, fp, ra and gp
    .cfi_adjust_cfa_offset SPILL_SIZE
    sw    $gp, 16($sp)
    sw    $ra, 12($sp)
    .cfi_rel_offset 31, 12
    sw    $fp, 8($sp)
    .cfi_rel_offset 30, 8
    sw    $s1, 4($sp)
    .cfi_rel_offset 17, 4
    sw    $s0, 0($sp)
    .cfi_rel_offset 16, 0
    move  $fp, $sp              # save sp in fp
    .cfi_def_cfa_register 30
    move  $s1, $a3              # move managed thread pointer into s1
    addiu $s0, $zero, SUSPEND_CHECK_INTERVAL  # reset s0 to suspend check interval
    addiu $t0, $a2, 4           # create space for ArtMethod* in frame.
    subu  $t0, $sp, $t0         # reserve & align *stack* to 16 bytes:
    srl   $t0, $t0, 4           #   native calling convention only aligns to 8B,
    sll   $sp, $t0, 4           #   so we have to ensure ART 16B alignment ourselves.
    addiu $a0, $sp, 4           # pass stack pointer + ArtMethod* as dest for memcpy
    la    $t9, memcpy
    jalr  $t9                   # (dest, src, bytes)
    addiu $sp, $sp, -16         # make space for argument slots for memcpy
    addiu $sp, $sp, 16          # restore stack after memcpy
    lw    $gp, 16($fp)          # restore $gp
    lw    $a0, SPILL_SIZE($fp)  # restore ArtMethod*
    addiu $t0, $sp, 4           # t0 = pointer to the current argument (skip ArtMethod*)
    li    $t3, 1                # t3 = gpr_index = 1 (skip A0)
    move  $t4, $zero            # t4 = fp_index = 0
    lw    $t1, 20 + SPILL_SIZE($fp)  # get shorty (20 is offset from the $sp on entry + SPILL_SIZE
                                # as the $fp is SPILL_SIZE bytes below the $sp on entry)
    addiu $t1, 1                # t1 = shorty + 1 (skip 1 for return type)
loopS:
    lbu   $t2, 0($t1)           # t2 = shorty[i]
    beqz  $t2, loopEndS         # finish getting args when shorty[i] == '\0'
    addiu $t1, 1

    li    $t9, 'J'              # put char 'J' into t9
    beq   $t9, $t2, isLongS     # branch if result type char == 'J'
    li    $t9, 'D'              # put char 'D' into t9
    beq   $t9, $t2, isDoubleS   # branch if result type char == 'D'
    li    $t9, 'F'              # put char 'F' into t9
    beq   $t9, $t2, isSingleS   # branch if result type char == 'F'
    addiu $t0, 4                # next_arg = curr_arg + 4 (in branch delay slot,
                                # for both, int and single)

    li    $t5, 1                                    # skip a0 (ArtMethod*)
    bne   $t5, $t3, 1f                              # if (gpr_index == 1)
    addiu $t5, 1
    LOAD_WORD_TO_REG a1, t0, t3, loopS              #   a1 = current argument, gpr_index++
1:  bne   $t5, $t3, 2f                              # else if (gpr_index == 2)
    addiu $t5, 1
    LOAD_WORD_TO_REG a2, t0, t3, loopS              #   a2 = current argument, gpr_index++
2:  bne   $t5, $t3, loopS                           # else if (gpr_index == 3)
    nop
    LOAD_WORD_TO_REG a3, t0, t3, loopS              #   a3 = current argument, gpr_index++

isLongS:
    addiu $t0, 8                                    # next_arg = curr_arg + 8
    slti  $t5, $t3, 3
    beqz  $t5, 3f                                   # if (gpr_index < 3)
    nop
    LOAD_LONG_TO_REG a2, a3, t0, t3, loopS          #   a2_a3 = curr_arg, gpr_index = 4
3:  b     loopS                                     # else
    li    $t3, 4                                    #   gpr_index = 4

isDoubleS:
    addiu $t0, 8                                    # next_arg = curr_arg + 8
    li    $t5, 0
    bne   $t5, $t4, 4f                              # if (fp_index == 0)
    addiu $t5, 1
    LOAD_DOUBLE_TO_REG f12, f13, t0, t4, t9, loopS  #   f12_f13 = curr_arg, fp_index++
4:  bne   $t5, $t4, loopS                           # else if (fp_index == 1)
    nop
    LOAD_DOUBLE_TO_REG f14, f15, t0, t4, t9, loopS  #   f14_f15 = curr_arg, fp_index++

isSingleS:
    li    $t5, 0
    bne   $t5, $t4, 5f                              # if (fp_index == 0)
    addiu $t5, 1
    LOAD_FLOAT_TO_REG f12, t0, t4, loopS            #   f12 = curr_arg, fp_index++
5:  bne   $t5, $t4, loopS                           # else if (fp_index == 1)
    nop
    LOAD_FLOAT_TO_REG f14, t0, t4, loopS            #   f14 = curr_arg, fp_index++

loopEndS:
    lw    $t9, ART_METHOD_QUICK_CODE_OFFSET_32($a0)  # get pointer to the code
    jalr  $t9                   # call the method
    sw    $zero, 0($sp)         # store null for ArtMethod* at bottom of frame
    move  $sp, $fp              # restore the stack
    lw    $s0, 0($sp)
    .cfi_restore 16
    lw    $s1, 4($sp)
    .cfi_restore 17
    lw    $fp, 8($sp)
    .cfi_restore 30
    lw    $ra, 12($sp)
    .cfi_restore 31
    addiu $sp, $sp, SPILL_SIZE
    .cfi_adjust_cfa_offset -SPILL_SIZE
    lw    $t0, 16($sp)          # get result pointer
    lw    $t1, 20($sp)          # get shorty
    lb    $t1, 0($t1)           # get result type char
    li    $t2, 'D'              # put char 'D' into t2
    beq   $t1, $t2, 6f          # branch if result type char == 'D'
    li    $t3, 'F'              # put char 'F' into t3
    beq   $t1, $t3, 6f          # branch if result type char == 'F'
    sw    $v0, 0($t0)           # store the result
    jalr  $zero, $ra
    sw    $v1, 4($t0)           # store the other half of the result
6:
    SDu   $f0, $f1, 0, $t0, $t1 # store floating point result
    jalr  $zero, $ra
    nop
END art_quick_invoke_static_stub

#undef SPILL_SIZE

    /*
     * Entry from managed code that calls artHandleFillArrayDataFromCode and delivers exception on
     * failure.
     */
    .extern artHandleFillArrayDataFromCode
ENTRY art_quick_handle_fill_data
    lw     $a2, 0($sp)                    # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case exception allocation triggers GC
    la     $t9, artHandleFillArrayDataFromCode
    jalr   $t9                            # (payload offset, Array*, method, Thread*)
    move   $a3, rSELF                     # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_handle_fill_data

    /*
     * Entry from managed code that calls artLockObjectFromCode, may block for GC.
     */
    .extern artLockObjectFromCode
ENTRY art_quick_lock_object
    beqz    $a0, .Lart_quick_throw_null_pointer_exception_gp_set
    nop
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case we block
    la      $t9, artLockObjectFromCode
    jalr    $t9                           # (Object* obj, Thread*)
    move    $a1, rSELF                    # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_lock_object

ENTRY art_quick_lock_object_no_inline
    beqz    $a0, .Lart_quick_throw_null_pointer_exception_gp_set
    nop
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME     # save callee saves in case we block
    la      $t9, artLockObjectFromCode
    jalr    $t9                           # (Object* obj, Thread*)
    move    $a1, rSELF                    # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_lock_object_no_inline

    /*
     * Entry from managed code that calls artUnlockObjectFromCode and delivers exception on failure.
     */
    .extern artUnlockObjectFromCode
ENTRY art_quick_unlock_object
    beqz    $a0, .Lart_quick_throw_null_pointer_exception_gp_set
    nop
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME # save callee saves in case exception allocation triggers GC
    la      $t9, artUnlockObjectFromCode
    jalr    $t9                       # (Object* obj, Thread*)
    move    $a1, rSELF                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_unlock_object

ENTRY art_quick_unlock_object_no_inline
    beqz    $a0, .Lart_quick_throw_null_pointer_exception_gp_set
    nop
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME # save callee saves in case exception allocation triggers GC
    la      $t9, artUnlockObjectFromCode
    jalr    $t9                       # (Object* obj, Thread*)
    move    $a1, rSELF                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_unlock_object_no_inline

    /*
     * Entry from managed code that calls artCheckCastFromCode and delivers exception on failure.
     */
    .extern artThrowClassCastException
ENTRY art_quick_check_cast
    addiu  $sp, $sp, -32
    .cfi_adjust_cfa_offset 32
    sw     $gp, 16($sp)
    sw     $ra, 12($sp)
    .cfi_rel_offset 31, 12
    sw     $t9, 8($sp)
    sw     $a1, 4($sp)
    sw     $a0, 0($sp)
    la     $t9, artIsAssignableFromCode
    jalr   $t9
    addiu  $sp, $sp, -16             # reserve argument slots on the stack
    addiu  $sp, $sp, 16
    lw     $gp, 16($sp)
    beqz   $v0, .Lthrow_class_cast_exception
    lw     $ra, 12($sp)
    jalr   $zero, $ra
    addiu  $sp, $sp, 32
    .cfi_adjust_cfa_offset -32
.Lthrow_class_cast_exception:
    lw     $t9, 8($sp)
    lw     $a1, 4($sp)
    lw     $a0, 0($sp)
    addiu  $sp, $sp, 32
    .cfi_adjust_cfa_offset -32
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la   $t9, artThrowClassCastException
    jalr $zero, $t9                 # artThrowClassCastException (Class*, Class*, Thread*)
    move $a2, rSELF                 # pass Thread::Current
END art_quick_check_cast

    /*
     * Restore rReg's value from offset($sp) if rReg is not the same as rExclude.
     * nReg is the register number for rReg.
     */
.macro POP_REG_NE rReg, nReg, offset, rExclude
    .ifnc \rReg, \rExclude
        lw \rReg, \offset($sp)      # restore rReg
        .cfi_restore \nReg
    .endif
.endm

    /*
     * Macro to insert read barrier, only used in art_quick_aput_obj.
     * rObj and rDest are registers, offset is a defined literal such as MIRROR_OBJECT_CLASS_OFFSET.
     * TODO: When read barrier has a fast path, add heap unpoisoning support for the fast path.
     */
.macro READ_BARRIER rDest, rObj, offset
#ifdef USE_READ_BARRIER
    # saved registers used in art_quick_aput_obj: a0-a2, t0-t1, t9, ra. 8 words for 16B alignment.
    addiu  $sp, $sp, -32
    .cfi_adjust_cfa_offset 32
    sw     $ra, 28($sp)
    .cfi_rel_offset 31, 28
    sw     $t9, 24($sp)
    .cfi_rel_offset 25, 24
    sw     $t1, 20($sp)
    .cfi_rel_offset 9, 20
    sw     $t0, 16($sp)
    .cfi_rel_offset 8, 16
    sw     $a2, 8($sp)              # padding slot at offset 12 (padding can be any slot in the 32B)
    .cfi_rel_offset 6, 8
    sw     $a1, 4($sp)
    .cfi_rel_offset 5, 4
    sw     $a0, 0($sp)
    .cfi_rel_offset 4, 0

    # move $a0, \rRef               # pass ref in a0 (no-op for now since parameter ref is unused)
    .ifnc \rObj, $a1
        move $a1, \rObj             # pass rObj
    .endif
    addiu  $a2, $zero, \offset      # pass offset
    la     $t9, artReadBarrierSlow
    jalr   $t9                      # artReadBarrierSlow(ref, rObj, offset)
    addiu  $sp, $sp, -16            # Use branch delay slot to reserve argument slots on the stack
                                    # before the call to artReadBarrierSlow.
    addiu  $sp, $sp, 16             # restore stack after call to artReadBarrierSlow
    # No need to unpoison return value in v0, artReadBarrierSlow() would do the unpoisoning.
    move \rDest, $v0                # save return value in rDest
                                    # (rDest cannot be v0 in art_quick_aput_obj)

    lw     $a0, 0($sp)              # restore registers except rDest
                                    # (rDest can only be t0 or t1 in art_quick_aput_obj)
    .cfi_restore 4
    lw     $a1, 4($sp)
    .cfi_restore 5
    lw     $a2, 8($sp)
    .cfi_restore 6
    POP_REG_NE $t0, 8, 16, \rDest
    POP_REG_NE $t1, 9, 20, \rDest
    lw     $t9, 24($sp)
    .cfi_restore 25
    lw     $ra, 28($sp)             # restore $ra
    .cfi_restore 31
    addiu  $sp, $sp, 32
    .cfi_adjust_cfa_offset -32
#else
    lw     \rDest, \offset(\rObj)
    UNPOISON_HEAP_REF \rDest
#endif  // USE_READ_BARRIER
.endm

    /*
     * Entry from managed code for array put operations of objects where the value being stored
     * needs to be checked for compatibility.
     * a0 = array, a1 = index, a2 = value
     */
ENTRY art_quick_aput_obj_with_null_and_bound_check
    bnez    $a0, .Lart_quick_aput_obj_with_bound_check_gp_set
    nop
    b .Lart_quick_throw_null_pointer_exception_gp_set
    nop
END art_quick_aput_obj_with_null_and_bound_check

ENTRY art_quick_aput_obj_with_bound_check
    lw $t0, MIRROR_ARRAY_LENGTH_OFFSET($a0)
    sltu $t1, $a1, $t0
    bnez $t1, .Lart_quick_aput_obj_gp_set
    nop
    move $a0, $a1
    b .Lart_quick_throw_array_bounds_gp_set
    move $a1, $t0
END art_quick_aput_obj_with_bound_check

#ifdef USE_READ_BARRIER
    .extern artReadBarrierSlow
#endif
ENTRY art_quick_aput_obj
    beqz $a2, .Ldo_aput_null
    nop
    READ_BARRIER $t0, $a0, MIRROR_OBJECT_CLASS_OFFSET
    READ_BARRIER $t1, $a2, MIRROR_OBJECT_CLASS_OFFSET
    READ_BARRIER $t0, $t0, MIRROR_CLASS_COMPONENT_TYPE_OFFSET
    bne $t1, $t0, .Lcheck_assignability  # value's type == array's component type - trivial assignability
    nop
.Ldo_aput:
    sll $a1, $a1, 2
    add $t0, $a0, $a1
    POISON_HEAP_REF $a2
    sw  $a2, MIRROR_OBJECT_ARRAY_DATA_OFFSET($t0)
    lw  $t0, THREAD_CARD_TABLE_OFFSET(rSELF)
    srl $t1, $a0, 7
    add $t1, $t1, $t0
    sb  $t0, ($t1)
    jalr $zero, $ra
    nop
.Ldo_aput_null:
    sll $a1, $a1, 2
    add $t0, $a0, $a1
    sw  $a2, MIRROR_OBJECT_ARRAY_DATA_OFFSET($t0)
    jalr $zero, $ra
    nop
.Lcheck_assignability:
    addiu  $sp, $sp, -32
    .cfi_adjust_cfa_offset 32
    sw     $ra, 28($sp)
    .cfi_rel_offset 31, 28
    sw     $gp, 16($sp)
    sw     $t9, 12($sp)
    sw     $a2, 8($sp)
    sw     $a1, 4($sp)
    sw     $a0, 0($sp)
    move   $a1, $t1
    move   $a0, $t0
    la     $t9, artIsAssignableFromCode
    jalr   $t9               # (Class*, Class*)
    addiu  $sp, $sp, -16     # reserve argument slots on the stack
    addiu  $sp, $sp, 16
    lw     $ra, 28($sp)
    lw     $gp, 16($sp)
    lw     $t9, 12($sp)
    lw     $a2, 8($sp)
    lw     $a1, 4($sp)
    lw     $a0, 0($sp)
    addiu  $sp, 32
    .cfi_adjust_cfa_offset -32
    bnez   $v0, .Ldo_aput
    nop
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    move $a1, $a2
    la   $t9, artThrowArrayStoreException
    jalr $zero, $t9                 # artThrowArrayStoreException(Class*, Class*, Thread*)
    move $a2, rSELF                 # pass Thread::Current
END art_quick_aput_obj

    /*
     * Called by managed code to resolve a static field and load a boolean primitive value.
     */
    .extern artGetBooleanStaticFromCode
ENTRY art_quick_get_boolean_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetBooleanStaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_boolean_static
    /*
     * Called by managed code to resolve a static field and load a byte primitive value.
     */
    .extern artGetByteStaticFromCode
ENTRY art_quick_get_byte_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetByteStaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_byte_static

    /*
     * Called by managed code to resolve a static field and load a char primitive value.
     */
    .extern artGetCharStaticFromCode
ENTRY art_quick_get_char_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetCharStaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_char_static
    /*
     * Called by managed code to resolve a static field and load a short primitive value.
     */
    .extern artGetShortStaticFromCode
ENTRY art_quick_get_short_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetShortStaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_short_static

    /*
     * Called by managed code to resolve a static field and load a 32-bit primitive value.
     */
    .extern artGet32StaticFromCode
ENTRY art_quick_get32_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGet32StaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get32_static

    /*
     * Called by managed code to resolve a static field and load a 64-bit primitive value.
     */
    .extern artGet64StaticFromCode
ENTRY art_quick_get64_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGet64StaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get64_static

    /*
     * Called by managed code to resolve a static field and load an object reference.
     */
    .extern artGetObjStaticFromCode
ENTRY art_quick_get_obj_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetObjStaticFromCode
    jalr   $t9                           # (uint32_t field_idx, const Method* referrer, Thread*)
    move   $a2, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_obj_static

    /*
     * Called by managed code to resolve an instance field and load a boolean primitive value.
     */
    .extern artGetBooleanInstanceFromCode
ENTRY art_quick_get_boolean_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetBooleanInstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_boolean_instance
    /*
     * Called by managed code to resolve an instance field and load a byte primitive value.
     */
    .extern artGetByteInstanceFromCode
ENTRY art_quick_get_byte_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetByteInstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_byte_instance

    /*
     * Called by managed code to resolve an instance field and load a char primitive value.
     */
    .extern artGetCharInstanceFromCode
ENTRY art_quick_get_char_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetCharInstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_char_instance
    /*
     * Called by managed code to resolve an instance field and load a short primitive value.
     */
    .extern artGetShortInstanceFromCode
ENTRY art_quick_get_short_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetShortInstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_short_instance

    /*
     * Called by managed code to resolve an instance field and load a 32-bit primitive value.
     */
    .extern artGet32InstanceFromCode
ENTRY art_quick_get32_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGet32InstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get32_instance

    /*
     * Called by managed code to resolve an instance field and load a 64-bit primitive value.
     */
    .extern artGet64InstanceFromCode
ENTRY art_quick_get64_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGet64InstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get64_instance

    /*
     * Called by managed code to resolve an instance field and load an object reference.
     */
    .extern artGetObjInstanceFromCode
ENTRY art_quick_get_obj_instance
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artGetObjInstanceFromCode
    jalr   $t9                           # (field_idx, Object*, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_NO_EXCEPTION
END art_quick_get_obj_instance

    /*
     * Called by managed code to resolve a static field and store a 8-bit primitive value.
     */
    .extern artSet8StaticFromCode
ENTRY art_quick_set8_static
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet8StaticFromCode
    jalr   $t9                           # (field_idx, new_val, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set8_static

    /*
     * Called by managed code to resolve a static field and store a 16-bit primitive value.
     */
    .extern artSet16StaticFromCode
ENTRY art_quick_set16_static
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet16StaticFromCode
    jalr   $t9                           # (field_idx, new_val, referrer, Thread*, $sp)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set16_static

    /*
     * Called by managed code to resolve a static field and store a 32-bit primitive value.
     */
    .extern artSet32StaticFromCode
ENTRY art_quick_set32_static
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet32StaticFromCode
    jalr   $t9                           # (field_idx, new_val, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set32_static

    /*
     * Called by managed code to resolve a static field and store a 64-bit primitive value.
     */
    .extern artSet64StaticFromCode
ENTRY art_quick_set64_static
    lw     $a1, 0($sp)                   # pass referrer's Method*
                                         # 64 bit new_val is in a2:a3 pair
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet64StaticFromCode
    jalr   $t9                           # (field_idx, referrer, new_val, Thread*)
    sw     rSELF, 16($sp)                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set64_static

    /*
     * Called by managed code to resolve a static field and store an object reference.
     */
    .extern artSetObjStaticFromCode
ENTRY art_quick_set_obj_static
    lw     $a2, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSetObjStaticFromCode
    jalr   $t9                           # (field_idx, new_val, referrer, Thread*)
    move   $a3, rSELF                    # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set_obj_static

    /*
     * Called by managed code to resolve an instance field and store a 8-bit primitive value.
     */
    .extern artSet8InstanceFromCode
ENTRY art_quick_set8_instance
    lw     $a3, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet8InstanceFromCode
    jalr   $t9                           # (field_idx, Object*, new_val, referrer, Thread*)
    sw     rSELF, 16($sp)                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set8_instance

    /*
     * Called by managed code to resolve an instance field and store a 16-bit primitive value.
     */
    .extern artSet16InstanceFromCode
ENTRY art_quick_set16_instance
    lw     $a3, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet16InstanceFromCode
    jalr   $t9                           # (field_idx, Object*, new_val, referrer, Thread*)
    sw     rSELF, 16($sp)                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set16_instance

    /*
     * Called by managed code to resolve an instance field and store a 32-bit primitive value.
     */
    .extern artSet32InstanceFromCode
ENTRY art_quick_set32_instance
    lw     $a3, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSet32InstanceFromCode
    jalr   $t9                           # (field_idx, Object*, new_val, referrer, Thread*)
    sw     rSELF, 16($sp)                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set32_instance

    /*
     * Called by managed code to resolve an instance field and store a 64-bit primitive value.
     */
    .extern artSet64InstanceFromCode
ENTRY art_quick_set64_instance
    lw     $t1, 0($sp)                   # load referrer's Method*
                                         # 64 bit new_val is in a2:a3 pair
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    sw     rSELF, 20($sp)                # pass Thread::Current
    la     $t9, artSet64InstanceFromCode
    jalr   $t9                           # (field_idx, Object*, new_val, referrer, Thread*)
    sw     $t1, 16($sp)                  # pass referrer's Method*
    RETURN_IF_ZERO
END art_quick_set64_instance

    /*
     * Called by managed code to resolve an instance field and store an object reference.
     */
    .extern artSetObjInstanceFromCode
ENTRY art_quick_set_obj_instance
    lw     $a3, 0($sp)                   # pass referrer's Method*
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME    # save callee saves in case of GC
    la     $t9, artSetObjInstanceFromCode
    jalr   $t9                           # (field_idx, Object*, new_val, referrer, Thread*)
    sw     rSELF, 16($sp)                # pass Thread::Current
    RETURN_IF_ZERO
END art_quick_set_obj_instance

// Macro to facilitate adding new allocation entrypoints.
.macro ONE_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME # save callee saves in case of GC
    la      $t9, \entrypoint
    jalr    $t9
    move    $a1, rSELF                # pass Thread::Current
    \return
END \name
.endm

.macro TWO_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME # save callee saves in case of GC
    la      $t9, \entrypoint
    jalr    $t9
    move    $a2, rSELF                # pass Thread::Current
    \return
END \name
.endm

.macro THREE_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME # save callee saves in case of GC
    la      $t9, \entrypoint
    jalr    $t9
    move    $a3, rSELF                # pass Thread::Current
    \return
END \name
.endm

.macro FOUR_ARG_DOWNCALL name, entrypoint, return
    .extern \entrypoint
ENTRY \name
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME # save callee saves in case of GC
    la      $t9, \entrypoint
    jalr    $t9
    sw      rSELF, 16($sp)            # pass Thread::Current
    \return
END \name
.endm

// Generate the allocation entrypoints for each allocator.
GENERATE_ALLOC_ENTRYPOINTS_FOR_EACH_ALLOCATOR

// A hand-written override for GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_rosalloc, RosAlloc).
ENTRY art_quick_alloc_object_rosalloc

    # Fast path rosalloc allocation
    # a0: type_idx
    # a1: ArtMethod*
    # s1: Thread::Current
    # -----------------------------
    # t0: class
    # t1: object size
    # t2: rosalloc run
    # t3: thread stack top offset
    # t4: thread stack bottom offset
    # v0: free list head
    #
    # t5, t6 : temps

    lw    $t0, ART_METHOD_DEX_CACHE_TYPES_OFFSET_32($a1)       # Load dex cache resolved types
                                                               # array.

    sll   $t5, $a0, COMPRESSED_REFERENCE_SIZE_SHIFT            # Shift the value.
    addu  $t5, $t0, $t5                                        # Compute the index.
    lw    $t0, 0($t5)                                          # Load class (t0).
    beqz  $t0, .Lart_quick_alloc_object_rosalloc_slow_path

    li    $t6, MIRROR_CLASS_STATUS_INITIALIZED
    lw    $t5, MIRROR_CLASS_STATUS_OFFSET($t0)                 # Check class status.
    bne   $t5, $t6, .Lart_quick_alloc_object_rosalloc_slow_path

    # Add a fake dependence from the following access flag and size loads to the status load. This
    # is to prevent those loads from being reordered above the status load and reading wrong values.
    xor   $t5, $t5, $t5
    addu  $t0, $t0, $t5

    lw    $t5, MIRROR_CLASS_ACCESS_FLAGS_OFFSET($t0)           # Check if access flags has
    li    $t6, ACCESS_FLAGS_CLASS_IS_FINALIZABLE               # kAccClassIsFinalizable.
    and   $t6, $t5, $t6
    bnez  $t6, .Lart_quick_alloc_object_rosalloc_slow_path

    lw    $t3, THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET($s1)        # Check if thread local allocation
    lw    $t4, THREAD_LOCAL_ALLOC_STACK_END_OFFSET($s1)        # stack has any room left.
    bgeu  $t3, $t4, .Lart_quick_alloc_object_rosalloc_slow_path

    lw    $t1, MIRROR_CLASS_OBJECT_SIZE_OFFSET($t0)            # Load object size (t1).
    li    $t5, ROSALLOC_MAX_THREAD_LOCAL_BRACKET_SIZE          # Check if size is for a thread local
                                                               # allocation.
    bgtu  $t1, $t5, .Lart_quick_alloc_object_rosalloc_slow_path

    # Compute the rosalloc bracket index from the size. Allign up the size by the rosalloc bracket
    # quantum size and divide by the quantum size and subtract by 1.

    addiu $t1, $t1, -1                                         # Decrease obj size and shift right
    srl   $t1, $t1, ROSALLOC_BRACKET_QUANTUM_SIZE_SHIFT        # by quantum.

    sll   $t2, $t1, POINTER_SIZE_SHIFT
    addu  $t2, $t2, $s1
    lw    $t2, THREAD_ROSALLOC_RUNS_OFFSET($t2)                # Load rosalloc run (t2).

    # Load the free list head (v0).
    # NOTE: this will be the return val.

    lw    $v0, (ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_HEAD_OFFSET)($t2)
    beqz  $v0, .Lart_quick_alloc_object_rosalloc_slow_path
    nop

    # Load the next pointer of the head and update the list head with the next pointer.

    lw    $t5, ROSALLOC_SLOT_NEXT_OFFSET($v0)
    sw    $t5, (ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_HEAD_OFFSET)($t2)

    # Store the class pointer in the header. This also overwrites the first pointer. The offsets are
    # asserted to match.

#if ROSALLOC_SLOT_NEXT_OFFSET != MIRROR_OBJECT_CLASS_OFFSET
#error "Class pointer needs to overwrite next pointer."
#endif

    POISON_HEAP_REF $t0
    sw    $t0, MIRROR_OBJECT_CLASS_OFFSET($v0)

    # Push the new object onto the thread local allocation stack and increment the thread local
    # allocation stack top.

    sw    $v0, 0($t3)
    addiu $t3, $t3, COMPRESSED_REFERENCE_SIZE
    sw    $t3, THREAD_LOCAL_ALLOC_STACK_TOP_OFFSET($s1)

    # Decrement the size of the free list.

    lw    $t5, (ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_SIZE_OFFSET)($t2)
    addiu $t5, $t5, -1
    sw    $t5, (ROSALLOC_RUN_FREE_LIST_OFFSET + ROSALLOC_RUN_FREE_LIST_SIZE_OFFSET)($t2)

    sync                                                          # Fence.

    jalr  $zero, $ra
    nop

  .Lart_quick_alloc_object_rosalloc_slow_path:

    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME
    la    $t9, artAllocObjectFromCodeRosAlloc
    jalr  $t9
    move  $a2, $s1                                                # Pass self as argument.
    RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

END art_quick_alloc_object_rosalloc

GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_tlab, TLAB)
GENERATE_ALLOC_ENTRYPOINTS_ALLOC_OBJECT(_region_tlab, RegionTLAB)

    /*
     * Entry from managed code to resolve a string, this stub will allocate a String and deliver an
     * exception on error. On success the String is returned. A0 holds the string index. The fast
     * path check for hit in strings cache has already been performed.
     */
ONE_ARG_DOWNCALL art_quick_resolve_string, artResolveStringFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

    /*
     * Entry from managed code when uninitialized static storage, this stub will run the class
     * initializer and deliver the exception on error. On success the static storage base is
     * returned.
     */
ONE_ARG_DOWNCALL art_quick_initialize_static_storage, artInitializeStaticStorageFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

    /*
     * Entry from managed code when dex cache misses for a type_idx.
     */
ONE_ARG_DOWNCALL art_quick_initialize_type, artInitializeTypeFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

    /*
     * Entry from managed code when type_idx needs to be checked for access and dex cache may also
     * miss.
     */
ONE_ARG_DOWNCALL art_quick_initialize_type_and_verify_access, artInitializeTypeAndVerifyAccessFromCode, RETURN_IF_RESULT_IS_NON_ZERO_OR_DELIVER

    /*
     * Called by managed code when the value in rSUSPEND has been decremented to 0.
     */
    .extern artTestSuspendFromCode
ENTRY art_quick_test_suspend
    lh     $a0, THREAD_FLAGS_OFFSET(rSELF)
    bnez   $a0, 1f
    addiu  rSUSPEND, $zero, SUSPEND_CHECK_INTERVAL   # reset rSUSPEND to SUSPEND_CHECK_INTERVAL
    jalr   $zero, $ra
    nop
1:
    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME          # save callee saves for stack crawl
    la     $t9, artTestSuspendFromCode
    jalr   $t9                                 # (Thread*)
    move   $a0, rSELF
    RESTORE_REFS_ONLY_CALLEE_SAVE_FRAME_AND_RETURN
END art_quick_test_suspend

    /*
     * Called by managed code that is attempting to call a method on a proxy class. On entry
     * a0 holds the proxy method; a1, a2 and a3 may contain arguments.
     */
    .extern artQuickProxyInvokeHandler
ENTRY art_quick_proxy_invoke_handler
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_WITH_METHOD_IN_A0
    move    $a2, rSELF                  # pass Thread::Current
    la      $t9, artQuickProxyInvokeHandler
    jalr    $t9                         # (Method* proxy method, receiver, Thread*, SP)
    addiu   $a3, $sp, ARG_SLOT_SIZE     # pass $sp (remove arg slots)
    lw      $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    bnez    $t0, 1f
    # don't care if $v0 and/or $v1 are modified, when exception branch taken
    MTD     $v0, $v1, $f0, $f1          # move float value to return value
    jalr    $zero, $ra
    nop
1:
    DELIVER_PENDING_EXCEPTION
END art_quick_proxy_invoke_handler

    /*
     * Called to resolve an imt conflict.
     * a0 is the conflict ArtMethod.
     * t0 is a hidden argument that holds the target interface method's dex method index.
     *
     * Note that this stub writes to a0, t0 and t1.
     */
ENTRY art_quick_imt_conflict_trampoline
    lw      $t1, 0($sp)                                      # Load referrer.
    lw      $t1, ART_METHOD_DEX_CACHE_METHODS_OFFSET_32($t1) # Load dex cache methods array.
    sll     $t0, $t0, POINTER_SIZE_SHIFT                     # Calculate offset.
    addu    $t0, $t1, $t0                                    # Add offset to base.
    lw      $t0, 0($t0)                                      # Load interface method.
    lw      $a0, ART_METHOD_JNI_OFFSET_32($a0)               # Load ImtConflictTable.

.Limt_table_iterate:
    lw      $t1, 0($a0)                                      # Load next entry in ImtConflictTable.
    # Branch if found.
    beq     $t1, $t0, .Limt_table_found
    nop
    # If the entry is null, the interface method is not in the ImtConflictTable.
    beqz    $t1, .Lconflict_trampoline
    nop
    # Iterate over the entries of the ImtConflictTable.
    b       .Limt_table_iterate
    addiu   $a0, $a0, 2 * __SIZEOF_POINTER__                 # Iterate to the next entry.

.Limt_table_found:
    # We successfully hit an entry in the table. Load the target method and jump to it.
    lw      $a0, __SIZEOF_POINTER__($a0)
    lw      $t9, ART_METHOD_QUICK_CODE_OFFSET_32($a0)
    jr      $t9
    nop

.Lconflict_trampoline:
    # Call the runtime stub to populate the ImtConflictTable and jump to the resolved method.
    INVOKE_TRAMPOLINE_BODY artInvokeInterfaceTrampoline
END art_quick_imt_conflict_trampoline

    .extern artQuickResolutionTrampoline
ENTRY art_quick_resolution_trampoline
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    move    $a2, rSELF                    # pass Thread::Current
    la      $t9, artQuickResolutionTrampoline
    jalr    $t9                           # (Method* called, receiver, Thread*, SP)
    addiu   $a3, $sp, ARG_SLOT_SIZE       # pass $sp (remove arg slots)
    beqz    $v0, 1f
    lw      $a0, ARG_SLOT_SIZE($sp)       # load resolved method to $a0
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    move    $t9, $v0               # code pointer must be in $t9 to generate the global pointer
    jalr    $zero, $t9             # tail call to method
    nop
1:
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    DELIVER_PENDING_EXCEPTION
END art_quick_resolution_trampoline

    .extern artQuickGenericJniTrampoline
    .extern artQuickGenericJniEndTrampoline
ENTRY art_quick_generic_jni_trampoline
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME_WITH_METHOD_IN_A0
    move    $s8, $sp               # save $sp to $s8
    move    $s3, $gp               # save $gp to $s3

    # prepare for call to artQuickGenericJniTrampoline(Thread*, SP)
    move    $a0, rSELF                     # pass Thread::Current
    addiu   $a1, $sp, ARG_SLOT_SIZE        # save $sp (remove arg slots)
    la      $t9, artQuickGenericJniTrampoline
    jalr    $t9                            # (Thread*, SP)
    addiu   $sp, $sp, -5120                # reserve space on the stack

    # The C call will have registered the complete save-frame on success.
    # The result of the call is:
    # v0: ptr to native code, 0 on error.
    # v1: ptr to the bottom of the used area of the alloca, can restore stack till here.
    beq     $v0, $zero, 1f         # check entry error
    move    $t9, $v0               # save the code ptr
    move    $sp, $v1               # release part of the alloca

    # Load parameters from stack into registers
    lw      $a0,   0($sp)
    lw      $a1,   4($sp)
    lw      $a2,   8($sp)

    # Load FPRs the same as GPRs. Look at BuildNativeCallFrameStateMachine.
    jalr    $t9                    # native call
    lw      $a3,  12($sp)
    addiu   $sp, $sp, 16           # remove arg slots

    move    $gp, $s3               # restore $gp from $s3

    # result sign extension is handled in C code
    # prepare for call to artQuickGenericJniEndTrampoline(Thread*, result, result_f)
    move    $a0, rSELF             # pass Thread::Current
    move    $a2, $v0               # pass result
    move    $a3, $v1
    addiu   $sp, $sp, -24          # reserve arg slots
    la      $t9, artQuickGenericJniEndTrampoline
    jalr    $t9
    s.d     $f0, 16($sp)           # pass result_f

    lw      $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    bne     $t0, $zero, 1f         # check for pending exceptions

    move    $sp, $s8               # tear down the alloca

    # tear dpown the callee-save frame
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME

    MTD     $v0, $v1, $f0, $f1     # move float value to return value
    jalr    $zero, $ra
    nop

1:
    lw $sp, THREAD_TOP_QUICK_FRAME_OFFSET(rSELF)
    # This will create a new save-all frame, required by the runtime.
    DELIVER_PENDING_EXCEPTION
END art_quick_generic_jni_trampoline

    .extern artQuickToInterpreterBridge
ENTRY art_quick_to_interpreter_bridge
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    move    $a1, rSELF                          # pass Thread::Current
    la      $t9, artQuickToInterpreterBridge
    jalr    $t9                                 # (Method* method, Thread*, SP)
    addiu   $a2, $sp, ARG_SLOT_SIZE             # pass $sp (remove arg slots)
    lw      $t0, THREAD_EXCEPTION_OFFSET(rSELF) # load Thread::Current()->exception_
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    bnez    $t0, 1f
    # don't care if $v0 and/or $v1 are modified, when exception branch taken
    MTD     $v0, $v1, $f0, $f1                  # move float value to return value
    jalr    $zero, $ra
    nop
1:
    DELIVER_PENDING_EXCEPTION
END art_quick_to_interpreter_bridge

    /*
     * Routine that intercepts method calls and returns.
     */
    .extern artInstrumentationMethodEntryFromCode
    .extern artInstrumentationMethodExitFromCode
ENTRY art_quick_instrumentation_entry
    SETUP_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    sw       $a0, 28($sp)   # save arg0 in free arg slot
    move     $a3, $ra       # pass $ra
    la       $t9, artInstrumentationMethodEntryFromCode
    jalr     $t9            # (Method*, Object*, Thread*, LR)
    move     $a2, rSELF     # pass Thread::Current
    move     $t9, $v0       # $t9 holds reference to code
    lw       $a0, 28($sp)   # restore arg0 from free arg slot
    RESTORE_REFS_AND_ARGS_CALLEE_SAVE_FRAME
    jalr     $t9            # call method
    nop
END art_quick_instrumentation_entry
    /* intentional fallthrough */
    .global art_quick_instrumentation_exit
art_quick_instrumentation_exit:
    .cfi_startproc
    addiu    $t9, $ra, 4    # put current address into $t9 to rebuild $gp
    .cpload  $t9
    move     $ra, $zero     # link register is to here, so clobber with 0 for later checks

    SETUP_REFS_ONLY_CALLEE_SAVE_FRAME
    addiu    $sp, $sp, -16  # allocate temp storage on the stack
    .cfi_adjust_cfa_offset 16
    sw       $v0, ARG_SLOT_SIZE+12($sp)
    .cfi_rel_offset 2, ARG_SLOT_SIZE+12
    sw       $v1, ARG_SLOT_SIZE+8($sp)
    .cfi_rel_offset 3, ARG_SLOT_SIZE+8
    s.d      $f0, ARG_SLOT_SIZE($sp)
    s.d      $f0, 16($sp)   # pass fpr result
    move     $a2, $v0       # pass gpr result
    move     $a3, $v1
    addiu    $a1, $sp, ARG_SLOT_SIZE+16   # pass $sp (remove arg slots and temp storage)
    la       $t9, artInstrumentationMethodExitFromCode
    jalr     $t9            # (Thread*, SP, gpr_res, fpr_res)
    move     $a0, rSELF     # pass Thread::Current
    move     $t9, $v0       # set aside returned link register
    move     $ra, $v1       # set link register for deoptimization
    lw       $v0, ARG_SLOT_SIZE+12($sp)   # restore return values
    lw       $v1, ARG_SLOT_SIZE+8($sp)
    l.d      $f0, ARG_SLOT_SIZE($sp)
    jalr     $zero, $t9     # return
    addiu    $sp, $sp, ARG_SLOT_SIZE+FRAME_SIZE_REFS_ONLY_CALLEE_SAVE+16  # restore stack
    .cfi_adjust_cfa_offset -(ARG_SLOT_SIZE+FRAME_SIZE_REFS_ONLY_CALLEE_SAVE+16)
END art_quick_instrumentation_exit

    /*
     * Instrumentation has requested that we deoptimize into the interpreter. The deoptimization
     * will long jump to the upcall with a special exception of -1.
     */
    .extern artDeoptimize
ENTRY art_quick_deoptimize
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la       $t9, artDeoptimize
    jalr     $t9            # artDeoptimize(Thread*)
                            # Returns caller method's frame size.
    move     $a0, rSELF     # pass Thread::current
END art_quick_deoptimize

    /*
     * Compiled code has requested that we deoptimize into the interpreter. The deoptimization
     * will long jump to the upcall with a special exception of -1.
     */
    .extern artDeoptimizeFromCompiledCode
ENTRY art_quick_deoptimize_from_compiled_code
    SETUP_SAVE_ALL_CALLEE_SAVE_FRAME
    la       $t9, artDeoptimizeFromCompiledCode
    jalr     $t9                            # artDeoptimizeFromCompiledCode(Thread*)
                                            # Returns caller method's frame size.
    move     $a0, rSELF                     # pass Thread::current
END art_quick_deoptimize_from_compiled_code

    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   $a0: low word
     *   $a1: high word
     *   $a2: shift count
     */
ENTRY_NO_GP art_quick_shl_long
    /* shl-long vAA, vBB, vCC */
    sll     $v0, $a0, $a2                    #  rlo<- alo << (shift&31)
    not     $v1, $a2                         #  rhi<- 31-shift  (shift is 5b)
    srl     $a0, 1
    srl     $a0, $v1                         #  alo<- alo >> (32-(shift&31))
    sll     $v1, $a1, $a2                    #  rhi<- ahi << (shift&31)
    andi    $a2, 0x20                        #  shift< shift & 0x20
    beqz    $a2, 1f
    or      $v1, $a0                         #  rhi<- rhi | alo

    move    $v1, $v0                         #  rhi<- rlo (if shift&0x20)
    move    $v0, $zero                       #  rlo<- 0 (if shift&0x20)

1:  jalr    $zero, $ra
    nop
END art_quick_shl_long

    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   $a0: low word
     *   $a1: high word
     *   $a2: shift count
     */
ENTRY_NO_GP art_quick_shr_long
    sra     $v1, $a1, $a2                    #  rhi<- ahi >> (shift&31)
    srl     $v0, $a0, $a2                    #  rlo<- alo >> (shift&31)
    sra     $a3, $a1, 31                     #  $a3<- sign(ah)
    not     $a0, $a2                         #  alo<- 31-shift (shift is 5b)
    sll     $a1, 1
    sll     $a1, $a0                         #  ahi<- ahi << (32-(shift&31))
    andi    $a2, 0x20                        #  shift & 0x20
    beqz    $a2, 1f
    or      $v0, $a1                         #  rlo<- rlo | ahi

    move    $v0, $v1                         #  rlo<- rhi (if shift&0x20)
    move    $v1, $a3                         #  rhi<- sign(ahi) (if shift&0x20)

1:  jalr    $zero, $ra
    nop
END art_quick_shr_long

    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to ignore all but the low
     * 6 bits.
     * On entry:
     *   $a0: low word
     *   $a1: high word
     *   $a2: shift count
     */
    /* ushr-long vAA, vBB, vCC */
ENTRY_NO_GP art_quick_ushr_long
    srl     $v1, $a1, $a2                    #  rhi<- ahi >> (shift&31)
    srl     $v0, $a0, $a2                    #  rlo<- alo >> (shift&31)
    not     $a0, $a2                         #  alo<- 31-shift (shift is 5b)
    sll     $a1, 1
    sll     $a1, $a0                         #  ahi<- ahi << (32-(shift&31))
    andi    $a2, 0x20                        #  shift & 0x20
    beqz    $a2, 1f
    or      $v0, $a1                         #  rlo<- rlo | ahi

    move    $v0, $v1                         #  rlo<- rhi (if shift&0x20)
    move    $v1, $zero                       #  rhi<- 0 (if shift&0x20)

1:  jalr    $zero, $ra
    nop
END art_quick_ushr_long

/* java.lang.String.indexOf(int ch, int fromIndex=0) */
ENTRY_NO_GP art_quick_indexof
/* $a0 holds address of "this" */
/* $a1 holds "ch" */
/* $a2 holds "fromIndex" */
  lw    $t0, MIRROR_STRING_COUNT_OFFSET($a0)    # this.length()
  slt   $t1, $a2, $zero # if fromIndex < 0
#if defined(_MIPS_ARCH_MIPS32R6) || defined(_MIPS_ARCH_MIPS64R6)
  seleqz $a2, $a2, $t1  #     fromIndex = 0;
#else
  movn   $a2, $zero, $t1 #    fromIndex = 0;
#endif
  subu  $t0, $t0, $a2   # this.length() - fromIndex
  blez  $t0, 6f         # if this.length()-fromIndex <= 0
  li    $v0, -1         #     return -1;

  sll   $v0, $a2, 1     # $a0 += $a2 * 2
  addu  $a0, $a0, $v0   #  "  "   "  " "
  move  $v0, $a2        # Set i to fromIndex.

1:
  lhu   $t3, MIRROR_STRING_VALUE_OFFSET($a0)    # if this.charAt(i) == ch
  beq   $t3, $a1, 6f                            #     return i;
  addu  $a0, $a0, 2     # i++
  subu  $t0, $t0, 1     # this.length() - i
  bnez  $t0, 1b         # while this.length() - i > 0
  addu  $v0, $v0, 1     # i++

  li    $v0, -1         # if this.length() - i <= 0
                        #     return -1;

6:
  j     $ra
  nop
END art_quick_indexof

/* java.lang.String.compareTo(String anotherString) */
ENTRY_NO_GP art_quick_string_compareto
/* $a0 holds address of "this" */
/* $a1 holds address of "anotherString" */
  beq    $a0, $a1, 9f   # this and anotherString are the same object
  move   $v0, $zero

  lw     $a2, MIRROR_STRING_COUNT_OFFSET($a0)   # this.length()
  lw     $a3, MIRROR_STRING_COUNT_OFFSET($a1)   # anotherString.length()
  MINu   $t2, $a2, $a3
# $t2 now holds min(this.length(),anotherString.length())

  beqz   $t2, 9f        # while min(this.length(),anotherString.length())-i != 0
  subu   $v0, $a2, $a3  # if $t2==0 return
                        #     (this.length() - anotherString.length())
1:
  lhu    $t0, MIRROR_STRING_VALUE_OFFSET($a0)   # while this.charAt(i) == anotherString.charAt(i)
  lhu    $t1, MIRROR_STRING_VALUE_OFFSET($a1)
  bne    $t0, $t1, 9f   # if this.charAt(i) != anotherString.charAt(i)
  subu   $v0, $t0, $t1  #     return (this.charAt(i) - anotherString.charAt(i))
  addiu  $a0, $a0, 2    # point at this.charAt(i++)
  subu   $t2, $t2, 1    # new value of
                        # min(this.length(),anotherString.length())-i
  bnez   $t2, 1b
  addiu  $a1, $a1, 2    # point at anotherString.charAt(i++)
  subu   $v0, $a2, $a3

9:
  j      $ra
  nop
END art_quick_string_compareto
