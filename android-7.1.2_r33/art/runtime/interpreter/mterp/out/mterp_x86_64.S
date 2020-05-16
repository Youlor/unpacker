/*
 * This file was generated automatically by gen-mterp.py for 'x86_64'.
 *
 * --> DO NOT EDIT <--
 */

/* File: x86_64/header.S */
/*
 * Copyright (C) 2016 The Android Open Source Project
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

/*
  Art assembly interpreter notes:

  First validate assembly code by implementing ExecuteXXXImpl() style body (doesn't
  handle invoke, allows higher-level code to create frame & shadow frame.

  Once that's working, support direct entry code & eliminate shadow frame (and
  excess locals allocation.

  Some (hopefully) temporary ugliness.  We'll treat rFP as pointing to the
  base of the vreg array within the shadow frame.  Access the other fields,
  dex_pc_, method_ and number_of_vregs_ via negative offsets.  For now, we'll continue
  the shadow frame mechanism of double-storing object references - via rFP &
  number_of_vregs_.

 */

/*
x86_64 ABI general notes:

Caller save set:
   rax, rdx, rcx, rsi, rdi, r8-r11, st(0)-st(7)
Callee save set:
   rbx, rbp, r12-r15
Return regs:
   32-bit in eax
   64-bit in rax
   fp on xmm0

First 8 fp parameters came in xmm0-xmm7.
First 6 non-fp parameters came in rdi, rsi, rdx, rcx, r8, r9.
Other parameters passed on stack, pushed right-to-left.  On entry to target, first
param is at 8(%esp).  Traditional entry code is:

Stack must be 16-byte aligned to support SSE in native code.

If we're not doing variable stack allocation (alloca), the frame pointer can be
eliminated and all arg references adjusted to be esp relative.
*/

/*
Mterp and x86_64 notes:

Some key interpreter variables will be assigned to registers.

  nick     reg   purpose
  rPROFILE rbp   countdown register for jit profiling
  rPC      r12   interpreted program counter, used for fetching instructions
  rFP      r13   interpreted frame pointer, used for accessing locals and args
  rINSTw   bx    first 16-bit code of current instruction
  rINSTbl  bl    opcode portion of instruction word
  rINSTbh  bh    high byte of inst word, usually contains src/tgt reg names
  rIBASE   r14   base of instruction handler table
  rREFS    r15   base of object references in shadow frame.

Notes:
   o High order 16 bits of ebx must be zero on entry to handler
   o rPC, rFP, rINSTw/rINSTbl valid on handler entry and exit
   o eax and ecx are scratch, rINSTw/ebx sometimes scratch

Macros are provided for common operations.  Each macro MUST emit only
one instruction to make instruction-counting easier.  They MUST NOT alter
unspecified registers or condition codes.
*/

/*
 * This is a #include, not a %include, because we want the C pre-processor
 * to expand the macros into assembler assignment statements.
 */
#include "asm_support.h"

/*
 * Handle mac compiler specific
 */
#if defined(__APPLE__)
    #define MACRO_LITERAL(value) $(value)
    #define FUNCTION_TYPE(name)
    #define SIZE(start,end)
    // Mac OS' symbols have an _ prefix.
    #define SYMBOL(name) _ ## name
#else
    #define MACRO_LITERAL(value) $value
    #define FUNCTION_TYPE(name) .type name, @function
    #define SIZE(start,end) .size start, .-end
    #define SYMBOL(name) name
#endif

.macro PUSH _reg
    pushq \_reg
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset \_reg, 0
.endm

.macro POP _reg
    popq \_reg
    .cfi_adjust_cfa_offset -8
    .cfi_restore \_reg
.endm

/*
 * Instead of holding a pointer to the shadow frame, we keep rFP at the base of the vregs.  So,
 * to access other shadow frame fields, we need to use a backwards offset.  Define those here.
 */
#define OFF_FP(a) (a - SHADOWFRAME_VREGS_OFFSET)
#define OFF_FP_NUMBER_OF_VREGS OFF_FP(SHADOWFRAME_NUMBER_OF_VREGS_OFFSET)
#define OFF_FP_DEX_PC OFF_FP(SHADOWFRAME_DEX_PC_OFFSET)
#define OFF_FP_LINK OFF_FP(SHADOWFRAME_LINK_OFFSET)
#define OFF_FP_METHOD OFF_FP(SHADOWFRAME_METHOD_OFFSET)
#define OFF_FP_RESULT_REGISTER OFF_FP(SHADOWFRAME_RESULT_REGISTER_OFFSET)
#define OFF_FP_DEX_PC_PTR OFF_FP(SHADOWFRAME_DEX_PC_PTR_OFFSET)
#define OFF_FP_CODE_ITEM OFF_FP(SHADOWFRAME_CODE_ITEM_OFFSET)
#define OFF_FP_COUNTDOWN_OFFSET OFF_FP(SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET)
#define OFF_FP_SHADOWFRAME (-SHADOWFRAME_VREGS_OFFSET)

/* Frame size must be 16-byte aligned.
 * Remember about 8 bytes for return address + 6 * 8 for spills.
 */
#define FRAME_SIZE     8

/* Frame diagram while executing ExecuteMterpImpl, high to low addresses */
#define IN_ARG3        %rcx
#define IN_ARG2        %rdx
#define IN_ARG1        %rsi
#define IN_ARG0        %rdi
/* Spill offsets relative to %esp */
#define SELF_SPILL     (FRAME_SIZE -  8)
/* Out Args  */
#define OUT_ARG3       %rcx
#define OUT_ARG2       %rdx
#define OUT_ARG1       %rsi
#define OUT_ARG0       %rdi
#define OUT_32_ARG3    %ecx
#define OUT_32_ARG2    %edx
#define OUT_32_ARG1    %esi
#define OUT_32_ARG0    %edi
#define OUT_FP_ARG1    %xmm1
#define OUT_FP_ARG0    %xmm0

/* During bringup, we'll use the shadow frame model instead of rFP */
/* single-purpose registers, given names for clarity */
#define rSELF    SELF_SPILL(%rsp)
#define rPC      %r12
#define rFP      %r13
#define rINST    %ebx
#define rINSTq   %rbx
#define rINSTw   %bx
#define rINSTbh  %bh
#define rINSTbl  %bl
#define rIBASE   %r14
#define rREFS    %r15
#define rPROFILE %ebp

#define MTERP_LOGGING 0

/*
 * "export" the PC to dex_pc field in the shadow frame, f/b/o future exception objects.  Must
 * be done *before* something throws.
 *
 * It's okay to do this more than once.
 *
 * NOTE: the fast interpreter keeps track of dex pc as a direct pointer to the mapped
 * dex byte codes.  However, the rest of the runtime expects dex pc to be an instruction
 * offset into the code_items_[] array.  For effiency, we will "export" the
 * current dex pc as a direct pointer using the EXPORT_PC macro, and rely on GetDexPC
 * to convert to a dex pc when needed.
 */
.macro EXPORT_PC
    movq    rPC, OFF_FP_DEX_PC_PTR(rFP)
.endm

/*
 * Refresh handler table.
 * IBase handles uses the caller save register so we must restore it after each call.
 * Also it is used as a result of some 64-bit operations (like imul) and we should
 * restore it in such cases also.
 *
 */
.macro REFRESH_IBASE
    movq    rSELF, rIBASE
    movq    THREAD_CURRENT_IBASE_OFFSET(rIBASE), rIBASE
.endm

/*
 * Refresh rINST.
 * At enter to handler rINST does not contain the opcode number.
 * However some utilities require the full value, so this macro
 * restores the opcode number.
 */
.macro REFRESH_INST _opnum
    movb    rINSTbl, rINSTbh
    movb    $\_opnum, rINSTbl
.endm

/*
 * Fetch the next instruction from rPC into rINSTw.  Does not advance rPC.
 */
.macro FETCH_INST
    movzwq  (rPC), rINSTq
.endm

/*
 * Remove opcode from rINST, compute the address of handler and jump to it.
 */
.macro GOTO_NEXT
    movzx   rINSTbl,%eax
    movzbl  rINSTbh,rINST
    shll    MACRO_LITERAL(7), %eax
    addq    rIBASE, %rax
    jmp     *%rax
.endm

/*
 * Advance rPC by instruction count.
 */
.macro ADVANCE_PC _count
    leaq    2*\_count(rPC), rPC
.endm

/*
 * Advance rPC by instruction count, fetch instruction and jump to handler.
 */
.macro ADVANCE_PC_FETCH_AND_GOTO_NEXT _count
    ADVANCE_PC \_count
    FETCH_INST
    GOTO_NEXT
.endm

/*
 * Get/set the 32-bit value from a Dalvik register.
 */
#define VREG_ADDRESS(_vreg) (rFP,_vreg,4)
#define VREG_REF_ADDRESS(_vreg) (rREFS,_vreg,4)

.macro GET_VREG _reg _vreg
    movl    (rFP,\_vreg,4), \_reg
.endm

/* Read wide value. */
.macro GET_WIDE_VREG _reg _vreg
    movq    (rFP,\_vreg,4), \_reg
.endm

.macro SET_VREG _reg _vreg
    movl    \_reg, (rFP,\_vreg,4)
    movl    MACRO_LITERAL(0), (rREFS,\_vreg,4)
.endm

/* Write wide value. reg is clobbered. */
.macro SET_WIDE_VREG _reg _vreg
    movq    \_reg, (rFP,\_vreg,4)
    xorq    \_reg, \_reg
    movq    \_reg, (rREFS,\_vreg,4)
.endm

.macro SET_VREG_OBJECT _reg _vreg
    movl    \_reg, (rFP,\_vreg,4)
    movl    \_reg, (rREFS,\_vreg,4)
.endm

.macro GET_VREG_HIGH _reg _vreg
    movl    4(rFP,\_vreg,4), \_reg
.endm

.macro SET_VREG_HIGH _reg _vreg
    movl    \_reg, 4(rFP,\_vreg,4)
    movl    MACRO_LITERAL(0), 4(rREFS,\_vreg,4)
.endm

.macro CLEAR_REF _vreg
    movl    MACRO_LITERAL(0),  (rREFS,\_vreg,4)
.endm

.macro CLEAR_WIDE_REF _vreg
    movl    MACRO_LITERAL(0),  (rREFS,\_vreg,4)
    movl    MACRO_LITERAL(0), 4(rREFS,\_vreg,4)
.endm

/* File: x86_64/entry.S */
/*
 * Copyright (C) 2016 The Android Open Source Project
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
/*
 * Interpreter entry point.
 */

    .text
    .global SYMBOL(ExecuteMterpImpl)
    FUNCTION_TYPE(ExecuteMterpImpl)

/*
 * On entry:
 *  0  Thread* self
 *  1  code_item
 *  2  ShadowFrame
 *  3  JValue* result_register
 *
 */

SYMBOL(ExecuteMterpImpl):
    .cfi_startproc
    .cfi_def_cfa rsp, 8

    /* Spill callee save regs */
    PUSH %rbx
    PUSH %rbp
    PUSH %r12
    PUSH %r13
    PUSH %r14
    PUSH %r15

    /* Allocate frame */
    subq    $FRAME_SIZE, %rsp
    .cfi_adjust_cfa_offset FRAME_SIZE

    /* Remember the return register */
    movq    IN_ARG3, SHADOWFRAME_RESULT_REGISTER_OFFSET(IN_ARG2)

    /* Remember the code_item */
    movq    IN_ARG1, SHADOWFRAME_CODE_ITEM_OFFSET(IN_ARG2)

    /* set up "named" registers */
    movl    SHADOWFRAME_NUMBER_OF_VREGS_OFFSET(IN_ARG2), %eax
    leaq    SHADOWFRAME_VREGS_OFFSET(IN_ARG2), rFP
    leaq    (rFP, %rax, 4), rREFS
    movl    SHADOWFRAME_DEX_PC_OFFSET(IN_ARG2), %eax
    leaq    CODEITEM_INSNS_OFFSET(IN_ARG1), rPC
    leaq    (rPC, %rax, 2), rPC
    EXPORT_PC

    /* Starting ibase */
    movq    IN_ARG0, rSELF
    REFRESH_IBASE

    /* Set up for backwards branches & osr profiling */
    movq    OFF_FP_METHOD(rFP), OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpSetUpHotnessCountdown)
    movswl  %ax, rPROFILE

    /* start executing the instruction at rPC */
    FETCH_INST
    GOTO_NEXT
    /* NOTE: no fallthrough */


    .global SYMBOL(artMterpAsmInstructionStart)
    FUNCTION_TYPE(SYMBOL(artMterpAsmInstructionStart))
SYMBOL(artMterpAsmInstructionStart) = .L_op_nop
    .text

/* ------------------------------ */
    .balign 128
.L_op_nop: /* 0x00 */
/* File: x86_64/op_nop.S */
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move: /* 0x01 */
/* File: x86_64/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movl    rINST, %eax                     # eax <- BA
    andb    $0xf, %al                      # eax <- A
    shrl    $4, rINST                      # rINST <- B
    GET_VREG %edx, rINSTq
    .if 0
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_from16: /* 0x02 */
/* File: x86_64/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    movzwq  2(rPC), %rax                    # eax <- BBBB
    GET_VREG %edx, %rax                     # edx <- fp[BBBB]
    .if 0
    SET_VREG_OBJECT %edx, rINSTq            # fp[A] <- fp[B]
    .else
    SET_VREG %edx, rINSTq                   # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_move_16: /* 0x03 */
/* File: x86_64/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    movzwq  4(rPC), %rcx                    # ecx <- BBBB
    movzwq  2(rPC), %rax                    # eax <- AAAA
    GET_VREG %edx, %rcx
    .if 0
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_move_wide: /* 0x04 */
/* File: x86_64/op_move_wide.S */
    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rdx, rINSTq              # rdx <- v[B]
    SET_WIDE_VREG %rdx, %rcx                # v[A] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_wide_from16: /* 0x05 */
/* File: x86_64/op_move_wide_from16.S */
    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwl  2(rPC), %ecx                    # ecx <- BBBB
    GET_WIDE_VREG %rdx, %rcx                # rdx <- v[B]
    SET_WIDE_VREG %rdx, rINSTq              # v[A] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_move_wide_16: /* 0x06 */
/* File: x86_64/op_move_wide_16.S */
    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwq  4(rPC), %rcx                    # ecx<- BBBB
    movzwq  2(rPC), %rax                    # eax<- AAAA
    GET_WIDE_VREG %rdx, %rcx                # rdx <- v[B]
    SET_WIDE_VREG %rdx, %rax                # v[A] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_move_object: /* 0x07 */
/* File: x86_64/op_move_object.S */
/* File: x86_64/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movl    rINST, %eax                     # eax <- BA
    andb    $0xf, %al                      # eax <- A
    shrl    $4, rINST                      # rINST <- B
    GET_VREG %edx, rINSTq
    .if 1
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_move_object_from16: /* 0x08 */
/* File: x86_64/op_move_object_from16.S */
/* File: x86_64/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    movzwq  2(rPC), %rax                    # eax <- BBBB
    GET_VREG %edx, %rax                     # edx <- fp[BBBB]
    .if 1
    SET_VREG_OBJECT %edx, rINSTq            # fp[A] <- fp[B]
    .else
    SET_VREG %edx, rINSTq                   # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_move_object_16: /* 0x09 */
/* File: x86_64/op_move_object_16.S */
/* File: x86_64/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    movzwq  4(rPC), %rcx                    # ecx <- BBBB
    movzwq  2(rPC), %rax                    # eax <- AAAA
    GET_VREG %edx, %rcx
    .if 1
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3


/* ------------------------------ */
    .balign 128
.L_op_move_result: /* 0x0a */
/* File: x86_64/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    movq    OFF_FP_RESULT_REGISTER(rFP), %rax    # get pointer to result JType.
    movl    (%rax), %eax                    # r0 <- result.i.
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- fp[B]
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_result_wide: /* 0x0b */
/* File: x86_64/op_move_result_wide.S */
    /* move-result-wide vAA */
    movq    OFF_FP_RESULT_REGISTER(rFP), %rax    # get pointer to result JType.
    movq    (%rax), %rdx                         # Get wide
    SET_WIDE_VREG %rdx, rINSTq                   # v[AA] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_result_object: /* 0x0c */
/* File: x86_64/op_move_result_object.S */
/* File: x86_64/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    movq    OFF_FP_RESULT_REGISTER(rFP), %rax    # get pointer to result JType.
    movl    (%rax), %eax                    # r0 <- result.i.
    .if 1
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- fp[B]
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_move_exception: /* 0x0d */
/* File: x86_64/op_move_exception.S */
    /* move-exception vAA */
    movq    rSELF, %rcx
    movl    THREAD_EXCEPTION_OFFSET(%rcx), %eax
    SET_VREG_OBJECT %eax, rINSTq            # fp[AA] <- exception object
    movl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_return_void: /* 0x0e */
/* File: x86_64/op_return_void.S */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movq    rSELF, OUT_ARG0
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    xorq    %rax, %rax
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return: /* 0x0f */
/* File: x86_64/op_return.S */
/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movq    rSELF, OUT_ARG0
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINSTq                   # eax <- vAA
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return_wide: /* 0x10 */
/* File: x86_64/op_return_wide.S */
/*
 * Return a 64-bit value.
 */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movq    rSELF, OUT_ARG0
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_WIDE_VREG %rax, rINSTq              # eax <- v[AA]
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return_object: /* 0x11 */
/* File: x86_64/op_return_object.S */
/* File: x86_64/op_return.S */
/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movq    rSELF, OUT_ARG0
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINSTq                   # eax <- vAA
    jmp     MterpReturn


/* ------------------------------ */
    .balign 128
.L_op_const_4: /* 0x12 */
/* File: x86_64/op_const_4.S */
    /* const/4 vA, #+B */
    movsbl  rINSTbl, %eax                   # eax <-ssssssBx
    movl    $0xf, rINST
    andl    %eax, rINST                     # rINST <- A
    sarl    $4, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_const_16: /* 0x13 */
/* File: x86_64/op_const_16.S */
    /* const/16 vAA, #+BBBB */
    movswl  2(rPC), %ecx                    # ecx <- ssssBBBB
    SET_VREG %ecx, rINSTq                   # vAA <- ssssBBBB
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const: /* 0x14 */
/* File: x86_64/op_const.S */
    /* const vAA, #+BBBBbbbb */
    movl    2(rPC), %eax                    # grab all 32 bits at once
    SET_VREG %eax, rINSTq                   # vAA<- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_const_high16: /* 0x15 */
/* File: x86_64/op_const_high16.S */
    /* const/high16 vAA, #+BBBB0000 */
    movzwl  2(rPC), %eax                    # eax <- 0000BBBB
    sall    $16, %eax                      # eax <- BBBB0000
    SET_VREG %eax, rINSTq                   # vAA <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_wide_16: /* 0x16 */
/* File: x86_64/op_const_wide_16.S */
    /* const-wide/16 vAA, #+BBBB */
    movswq  2(rPC), %rax                    # rax <- ssssBBBB
    SET_WIDE_VREG %rax, rINSTq              # store
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_wide_32: /* 0x17 */
/* File: x86_64/op_const_wide_32.S */
    /* const-wide/32 vAA, #+BBBBbbbb */
    movslq   2(rPC), %rax                   # eax <- ssssssssBBBBbbbb
    SET_WIDE_VREG %rax, rINSTq              # store
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_const_wide: /* 0x18 */
/* File: x86_64/op_const_wide.S */
    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    movq    2(rPC), %rax                    # rax <- HHHHhhhhBBBBbbbb
    SET_WIDE_VREG %rax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 5

/* ------------------------------ */
    .balign 128
.L_op_const_wide_high16: /* 0x19 */
/* File: x86_64/op_const_wide_high16.S */
    /* const-wide/high16 vAA, #+BBBB000000000000 */
    movzwq  2(rPC), %rax                    # eax <- 0000BBBB
    salq    $48, %rax                      # eax <- BBBB0000
    SET_WIDE_VREG %rax, rINSTq              # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_string: /* 0x1a */
/* File: x86_64/op_const_string.S */
    /* const/string vAA, String@BBBB */
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # OUT_ARG0 <- BBBB
    movq    rINSTq, OUT_ARG1
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpConstString)        # (index, tgt_reg, shadow_frame, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_string_jumbo: /* 0x1b */
/* File: x86_64/op_const_string_jumbo.S */
    /* const/string vAA, String@BBBBBBBB */
    EXPORT_PC
    movl    2(rPC), OUT_32_ARG0             # OUT_32_ARG0 <- BBBB
    movq    rINSTq, OUT_ARG1
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpConstString)        # (index, tgt_reg, shadow_frame, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_const_class: /* 0x1c */
/* File: x86_64/op_const_class.S */
    /* const/class vAA, Class@BBBB */
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # eax <- OUT_ARG0
    movq    rINSTq, OUT_ARG1
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpConstClass)         # (index, tgt_reg, shadow_frame, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_monitor_enter: /* 0x1d */
/* File: x86_64/op_monitor_enter.S */
/*
 * Synchronize on an object.
 */
    /* monitor-enter vAA */
    EXPORT_PC
    GET_VREG OUT_32_ARG0, rINSTq
    movq    rSELF, OUT_ARG1
    call    SYMBOL(artLockObjectFromCode)   # (object, self)
    testq   %rax, %rax
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_monitor_exit: /* 0x1e */
/* File: x86_64/op_monitor_exit.S */
/*
 * Unlock an object.
 *
 * Exceptions that occur when unlocking a monitor need to appear as
 * if they happened at the following instruction.  See the Dalvik
 * instruction spec.
 */
    /* monitor-exit vAA */
    EXPORT_PC
    GET_VREG OUT_32_ARG0, rINSTq
    movq    rSELF, OUT_ARG1
    call    SYMBOL(artUnlockObjectFromCode) # (object, self)
    testq   %rax, %rax
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_check_cast: /* 0x1f */
/* File: x86_64/op_check_cast.S */
/*
 * Check to see if a cast from one class to another is allowed.
 */
    /* check-cast vAA, class@BBBB */
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # OUT_ARG0 <- BBBB
    leaq    VREG_ADDRESS(rINSTq), OUT_ARG1
    movq    OFF_FP_METHOD(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpCheckCast)          # (index, &obj, method, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_instance_of: /* 0x20 */
/* File: x86_64/op_instance_of.S */
/*
 * Check to see if an object reference is an instance of a class.
 *
 * Most common situation is a non-null object, being compared against
 * an already-resolved class.
 */
    /* instance-of vA, vB, class@CCCC */
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # OUT_32_ARG0 <- CCCC
    movl    rINST, %eax                     # eax <- BA
    sarl    $4, %eax                       # eax <- B
    leaq    VREG_ADDRESS(%rax), OUT_ARG1    # Get object address
    movq    OFF_FP_METHOD(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpInstanceOf)         # (index, &obj, method, self)
    movsbl  %al, %eax
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    andb    $0xf, rINSTbl                  # rINSTbl <- A
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_array_length: /* 0x21 */
/* File: x86_64/op_array_length.S */
/*
 * Return the length of an array.
 */
    movl    rINST, %eax                     # eax <- BA
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %ecx, rINSTq                   # ecx <- vB (object ref)
    testl   %ecx, %ecx                      # is null?
    je      common_errNullObject
    andb    $0xf, %al                      # eax <- A
    movl    MIRROR_ARRAY_LENGTH_OFFSET(%rcx), rINST
    SET_VREG rINST, %rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_new_instance: /* 0x22 */
/* File: x86_64/op_new_instance.S */
/*
 * Create a new instance of a class.
 */
    /* new-instance vAA, class@BBBB */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rSELF, OUT_ARG1
    REFRESH_INST 34
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpNewInstance)
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_new_array: /* 0x23 */
/* File: x86_64/op_new_array.S */
/*
 * Allocate an array of objects, specified with the array class
 * and a count.
 *
 * The verifier guarantees that this is an array class, so we don't
 * check for it here.
 */
    /* new-array vA, vB, class@CCCC */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST 35
    movq    rINSTq, OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpNewArray)
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array: /* 0x24 */
/* File: x86_64/op_filled_new_array.S */
/*
 * Create a new array with elements filled from registers.
 *
 * for: filled-new-array, filled-new-array/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern MterpFilledNewArray
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    movq    rSELF, OUT_ARG2
    call    SYMBOL(MterpFilledNewArray)
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array_range: /* 0x25 */
/* File: x86_64/op_filled_new_array_range.S */
/* File: x86_64/op_filled_new_array.S */
/*
 * Create a new array with elements filled from registers.
 *
 * for: filled-new-array, filled-new-array/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern MterpFilledNewArrayRange
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    movq    rSELF, OUT_ARG2
    call    SYMBOL(MterpFilledNewArrayRange)
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3


/* ------------------------------ */
    .balign 128
.L_op_fill_array_data: /* 0x26 */
/* File: x86_64/op_fill_array_data.S */
    /* fill-array-data vAA, +BBBBBBBB */
    EXPORT_PC
    movl    2(rPC), %ecx                    # ecx <- BBBBbbbb
    leaq    (rPC,%rcx,2), OUT_ARG1          # OUT_ARG1 <- PC + BBBBbbbb*2
    GET_VREG OUT_32_ARG0, rINSTq            # OUT_ARG0 <- vAA (array object)
    call    SYMBOL(MterpFillArrayData)      # (obj, payload)
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_throw: /* 0x27 */
/* File: x86_64/op_throw.S */
/*
 * Throw an exception object in the current thread.
 */
    /* throw vAA */
    EXPORT_PC
    GET_VREG %eax, rINSTq                   # eax<- vAA (exception object)
    testb   %al, %al
    jz      common_errNullObject
    movq    rSELF, %rcx
    movq    %rax, THREAD_EXCEPTION_OFFSET(%rcx)
    jmp     MterpException

/* ------------------------------ */
    .balign 128
.L_op_goto: /* 0x28 */
/* File: x86_64/op_goto.S */
/*
 * Unconditional branch, 8-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto +AA */
    movsbq  rINSTbl, rINSTq                 # rINSTq <- ssssssAA
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_goto_16: /* 0x29 */
/* File: x86_64/op_goto_16.S */
/*
 * Unconditional branch, 16-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto/16 +AAAA */
    movswq  2(rPC), rINSTq                  # rINSTq <- ssssAAAA
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_goto_32: /* 0x2a */
/* File: x86_64/op_goto_32.S */
/*
 * Unconditional branch, 32-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 *
 *  Because we need the SF bit set, we'll use an adds
 * to convert from Dalvik offset to byte offset.
 */
    /* goto/32 +AAAAAAAA */
    movslq  2(rPC), rINSTq                  # rINSTq <- AAAAAAAA
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_packed_switch: /* 0x2b */
/* File: x86_64/op_packed_switch.S */
/*
 * Handle a packed-switch or sparse-switch instruction.  In both cases
 * we decode it and hand it off to a helper function.
 *
 * We don't really expect backward branches in a switch statement, but
 * they're perfectly legal, so we check for them here.
 *
 * for: packed-switch, sparse-switch
 */
    /* op vAA, +BBBB */
    movslq  2(rPC), OUT_ARG0                # rcx <- BBBBbbbb
    leaq    (rPC,OUT_ARG0,2), OUT_ARG0      # rcx <- PC + BBBBbbbb*2
    GET_VREG OUT_32_ARG1, rINSTq            # eax <- vAA
    call    SYMBOL(MterpDoPackedSwitch)
    testl   %eax, %eax
    movslq  %eax, rINSTq
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_sparse_switch: /* 0x2c */
/* File: x86_64/op_sparse_switch.S */
/* File: x86_64/op_packed_switch.S */
/*
 * Handle a packed-switch or sparse-switch instruction.  In both cases
 * we decode it and hand it off to a helper function.
 *
 * We don't really expect backward branches in a switch statement, but
 * they're perfectly legal, so we check for them here.
 *
 * for: packed-switch, sparse-switch
 */
    /* op vAA, +BBBB */
    movslq  2(rPC), OUT_ARG0                # rcx <- BBBBbbbb
    leaq    (rPC,OUT_ARG0,2), OUT_ARG0      # rcx <- PC + BBBBbbbb*2
    GET_VREG OUT_32_ARG1, rINSTq            # eax <- vAA
    call    SYMBOL(MterpDoSparseSwitch)
    testl   %eax, %eax
    movslq  %eax, rINSTq
    jmp     MterpCommonTakenBranch


/* ------------------------------ */
    .balign 128
.L_op_cmpl_float: /* 0x2d */
/* File: x86_64/op_cmpl_float.S */
/* File: x86_64/fpcmp.S */
/*
 * Compare two floating-point values.  Puts 0, 1, or -1 into the
 * destination register based on the results of the comparison.
 *
 * int compare(x, y) {
 *     if (x == y) {
 *         return 0;
 *     } else if (x < y) {
 *         return -1;
 *     } else if (x > y) {
 *         return 1;
 *     } else {
 *         return nanval ? 1 : -1;
 *     }
 * }
 */
    /* op vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx<- CC
    movzbq  2(rPC), %rax                    # eax<- BB
    movss VREG_ADDRESS(%rax), %xmm0
    xor     %eax, %eax
    ucomiss VREG_ADDRESS(%rcx), %xmm0
    jp      .Lop_cmpl_float_nan_is_neg
    je      .Lop_cmpl_float_finish
    jb      .Lop_cmpl_float_less
.Lop_cmpl_float_nan_is_pos:
    addb    $1, %al
    jmp     .Lop_cmpl_float_finish
.Lop_cmpl_float_nan_is_neg:
.Lop_cmpl_float_less:
    movl    $-1, %eax
.Lop_cmpl_float_finish:
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmpg_float: /* 0x2e */
/* File: x86_64/op_cmpg_float.S */
/* File: x86_64/fpcmp.S */
/*
 * Compare two floating-point values.  Puts 0, 1, or -1 into the
 * destination register based on the results of the comparison.
 *
 * int compare(x, y) {
 *     if (x == y) {
 *         return 0;
 *     } else if (x < y) {
 *         return -1;
 *     } else if (x > y) {
 *         return 1;
 *     } else {
 *         return nanval ? 1 : -1;
 *     }
 * }
 */
    /* op vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx<- CC
    movzbq  2(rPC), %rax                    # eax<- BB
    movss VREG_ADDRESS(%rax), %xmm0
    xor     %eax, %eax
    ucomiss VREG_ADDRESS(%rcx), %xmm0
    jp      .Lop_cmpg_float_nan_is_pos
    je      .Lop_cmpg_float_finish
    jb      .Lop_cmpg_float_less
.Lop_cmpg_float_nan_is_pos:
    addb    $1, %al
    jmp     .Lop_cmpg_float_finish
.Lop_cmpg_float_nan_is_neg:
.Lop_cmpg_float_less:
    movl    $-1, %eax
.Lop_cmpg_float_finish:
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmpl_double: /* 0x2f */
/* File: x86_64/op_cmpl_double.S */
/* File: x86_64/fpcmp.S */
/*
 * Compare two floating-point values.  Puts 0, 1, or -1 into the
 * destination register based on the results of the comparison.
 *
 * int compare(x, y) {
 *     if (x == y) {
 *         return 0;
 *     } else if (x < y) {
 *         return -1;
 *     } else if (x > y) {
 *         return 1;
 *     } else {
 *         return nanval ? 1 : -1;
 *     }
 * }
 */
    /* op vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx<- CC
    movzbq  2(rPC), %rax                    # eax<- BB
    movsd VREG_ADDRESS(%rax), %xmm0
    xor     %eax, %eax
    ucomisd VREG_ADDRESS(%rcx), %xmm0
    jp      .Lop_cmpl_double_nan_is_neg
    je      .Lop_cmpl_double_finish
    jb      .Lop_cmpl_double_less
.Lop_cmpl_double_nan_is_pos:
    addb    $1, %al
    jmp     .Lop_cmpl_double_finish
.Lop_cmpl_double_nan_is_neg:
.Lop_cmpl_double_less:
    movl    $-1, %eax
.Lop_cmpl_double_finish:
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmpg_double: /* 0x30 */
/* File: x86_64/op_cmpg_double.S */
/* File: x86_64/fpcmp.S */
/*
 * Compare two floating-point values.  Puts 0, 1, or -1 into the
 * destination register based on the results of the comparison.
 *
 * int compare(x, y) {
 *     if (x == y) {
 *         return 0;
 *     } else if (x < y) {
 *         return -1;
 *     } else if (x > y) {
 *         return 1;
 *     } else {
 *         return nanval ? 1 : -1;
 *     }
 * }
 */
    /* op vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx<- CC
    movzbq  2(rPC), %rax                    # eax<- BB
    movsd VREG_ADDRESS(%rax), %xmm0
    xor     %eax, %eax
    ucomisd VREG_ADDRESS(%rcx), %xmm0
    jp      .Lop_cmpg_double_nan_is_pos
    je      .Lop_cmpg_double_finish
    jb      .Lop_cmpg_double_less
.Lop_cmpg_double_nan_is_pos:
    addb    $1, %al
    jmp     .Lop_cmpg_double_finish
.Lop_cmpg_double_nan_is_neg:
.Lop_cmpg_double_less:
    movl    $-1, %eax
.Lop_cmpg_double_finish:
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmp_long: /* 0x31 */
/* File: x86_64/op_cmp_long.S */
/*
 * Compare two 64-bit values.  Puts 0, 1, or -1 into the destination
 * register based on the results of the comparison.
 */
    /* cmp-long vAA, vBB, vCC */
    movzbq  2(rPC), %rdx                    # edx <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rdx, %rdx                # rdx <- v[BB]
    xorl    %eax, %eax
    xorl    %edi, %edi
    addb    $1, %al
    movl    $-1, %esi
    cmpq    VREG_ADDRESS(%rcx), %rdx
    cmovl   %esi, %edi
    cmovg   %eax, %edi
    SET_VREG %edi, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_if_eq: /* 0x32 */
/* File: x86_64/op_if_eq.S */
/* File: x86_64/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # rcx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    cmpl    VREG_ADDRESS(rINSTq), %eax      # compare (vA, vB)
    jne   1f
    movswq  2(rPC), rINSTq                  # Get signed branch offset
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_ne: /* 0x33 */
/* File: x86_64/op_if_ne.S */
/* File: x86_64/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # rcx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    cmpl    VREG_ADDRESS(rINSTq), %eax      # compare (vA, vB)
    je   1f
    movswq  2(rPC), rINSTq                  # Get signed branch offset
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_lt: /* 0x34 */
/* File: x86_64/op_if_lt.S */
/* File: x86_64/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # rcx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    cmpl    VREG_ADDRESS(rINSTq), %eax      # compare (vA, vB)
    jge   1f
    movswq  2(rPC), rINSTq                  # Get signed branch offset
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_ge: /* 0x35 */
/* File: x86_64/op_if_ge.S */
/* File: x86_64/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # rcx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    cmpl    VREG_ADDRESS(rINSTq), %eax      # compare (vA, vB)
    jl   1f
    movswq  2(rPC), rINSTq                  # Get signed branch offset
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_gt: /* 0x36 */
/* File: x86_64/op_if_gt.S */
/* File: x86_64/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # rcx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    cmpl    VREG_ADDRESS(rINSTq), %eax      # compare (vA, vB)
    jle   1f
    movswq  2(rPC), rINSTq                  # Get signed branch offset
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_le: /* 0x37 */
/* File: x86_64/op_if_le.S */
/* File: x86_64/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # rcx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    cmpl    VREG_ADDRESS(rINSTq), %eax      # compare (vA, vB)
    jg   1f
    movswq  2(rPC), rINSTq                  # Get signed branch offset
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_eqz: /* 0x38 */
/* File: x86_64/op_if_eqz.S */
/* File: x86_64/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    jne   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_nez: /* 0x39 */
/* File: x86_64/op_if_nez.S */
/* File: x86_64/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    je   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_ltz: /* 0x3a */
/* File: x86_64/op_if_ltz.S */
/* File: x86_64/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    jge   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_gez: /* 0x3b */
/* File: x86_64/op_if_gez.S */
/* File: x86_64/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    jl   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_gtz: /* 0x3c */
/* File: x86_64/op_if_gtz.S */
/* File: x86_64/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    jle   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_lez: /* 0x3d */
/* File: x86_64/op_if_lez.S */
/* File: x86_64/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINSTq)       # compare (vA, 0)
    jg   1f
    movswq  2(rPC), rINSTq                  # fetch signed displacement
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
1:
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_unused_3e: /* 0x3e */
/* File: x86_64/op_unused_3e.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_3f: /* 0x3f */
/* File: x86_64/op_unused_3f.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_40: /* 0x40 */
/* File: x86_64/op_unused_40.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_41: /* 0x41 */
/* File: x86_64/op_unused_41.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_42: /* 0x42 */
/* File: x86_64/op_unused_42.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_43: /* 0x43 */
/* File: x86_64/op_unused_43.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_aget: /* 0x44 */
/* File: x86_64/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    movq    MIRROR_INT_ARRAY_DATA_OFFSET(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    movl   MIRROR_INT_ARRAY_DATA_OFFSET(%rax,%rcx,4), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aget_wide: /* 0x45 */
/* File: x86_64/op_aget_wide.S */
/* File: x86_64/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 1
    movq    MIRROR_WIDE_ARRAY_DATA_OFFSET(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    movq   MIRROR_WIDE_ARRAY_DATA_OFFSET(%rax,%rcx,8), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_object: /* 0x46 */
/* File: x86_64/op_aget_object.S */
/*
 * Array object get.  vAA <- vBB[vCC].
 *
 * for: aget-object
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG OUT_32_ARG0, %rax              # eax <- vBB (array object)
    GET_VREG OUT_32_ARG1, %rcx              # ecx <- vCC (requested index)
    EXPORT_PC
    call    SYMBOL(artAGetObjectFromMterp)  # (array, index)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    SET_VREG_OBJECT %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aget_boolean: /* 0x47 */
/* File: x86_64/op_aget_boolean.S */
/* File: x86_64/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    movq    MIRROR_BOOLEAN_ARRAY_DATA_OFFSET(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    movzbl   MIRROR_BOOLEAN_ARRAY_DATA_OFFSET(%rax,%rcx,1), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_byte: /* 0x48 */
/* File: x86_64/op_aget_byte.S */
/* File: x86_64/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    movq    MIRROR_BYTE_ARRAY_DATA_OFFSET(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    movsbl   MIRROR_BYTE_ARRAY_DATA_OFFSET(%rax,%rcx,1), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_char: /* 0x49 */
/* File: x86_64/op_aget_char.S */
/* File: x86_64/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    movq    MIRROR_CHAR_ARRAY_DATA_OFFSET(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    movzwl   MIRROR_CHAR_ARRAY_DATA_OFFSET(%rax,%rcx,2), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_short: /* 0x4a */
/* File: x86_64/op_aget_short.S */
/* File: x86_64/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    movq    MIRROR_SHORT_ARRAY_DATA_OFFSET(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    movswl   MIRROR_SHORT_ARRAY_DATA_OFFSET(%rax,%rcx,2), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput: /* 0x4b */
/* File: x86_64/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    movl    rINST, MIRROR_INT_ARRAY_DATA_OFFSET(%rax,%rcx,4)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aput_wide: /* 0x4c */
/* File: x86_64/op_aput_wide.S */
/* File: x86_64/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 1
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    movq    rINSTq, MIRROR_WIDE_ARRAY_DATA_OFFSET(%rax,%rcx,8)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_object: /* 0x4d */
/* File: x86_64/op_aput_object.S */
/*
 * Store an object into an array.  vBB[vCC] <- vAA.
 */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST 77
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpAputObject)         # (array, index)
    testb   %al, %al
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aput_boolean: /* 0x4e */
/* File: x86_64/op_aput_boolean.S */
/* File: x86_64/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    movb    rINSTbl, MIRROR_BOOLEAN_ARRAY_DATA_OFFSET(%rax,%rcx,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_byte: /* 0x4f */
/* File: x86_64/op_aput_byte.S */
/* File: x86_64/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    movb    rINSTbl, MIRROR_BYTE_ARRAY_DATA_OFFSET(%rax,%rcx,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_char: /* 0x50 */
/* File: x86_64/op_aput_char.S */
/* File: x86_64/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    movw    rINSTw, MIRROR_CHAR_ARRAY_DATA_OFFSET(%rax,%rcx,2)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_short: /* 0x51 */
/* File: x86_64/op_aput_short.S */
/* File: x86_64/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if 0
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    movw    rINSTw, MIRROR_SHORT_ARRAY_DATA_OFFSET(%rax,%rcx,2)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget: /* 0x52 */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGet32InstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iget_wide: /* 0x53 */
/* File: x86_64/op_iget_wide.S */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGet64InstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 1
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_object: /* 0x54 */
/* File: x86_64/op_iget_object.S */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGetObjInstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean: /* 0x55 */
/* File: x86_64/op_iget_boolean.S */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGetBooleanInstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_byte: /* 0x56 */
/* File: x86_64/op_iget_byte.S */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGetByteInstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_char: /* 0x57 */
/* File: x86_64/op_iget_char.S */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGetCharInstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_short: /* 0x58 */
/* File: x86_64/op_iget_short.S */
/* File: x86_64/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL(artGetShortInstanceFromCode)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput: /* 0x59 */
/* File: x86_64/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet32InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # field ref <- 0000CCCC
    movzbq  rINSTbl, %rcx                   # rcx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG OUT_32_ARG2, rINSTq            # fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet32InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_wide: /* 0x5a */
/* File: x86_64/op_iput_wide.S */
    /* iput-wide vA, vB, field@CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $0xf, rINSTbl                  # rINST <- A
    leaq    VREG_ADDRESS(rINSTq), OUT_ARG2  # &fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet64InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_object: /* 0x5b */
/* File: x86_64/op_iput_object.S */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST 91
    movl    rINST, OUT_32_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpIputObject)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_boolean: /* 0x5c */
/* File: x86_64/op_iput_boolean.S */
/* File: x86_64/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # field ref <- 0000CCCC
    movzbq  rINSTbl, %rcx                   # rcx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG OUT_32_ARG2, rINSTq            # fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet8InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_byte: /* 0x5d */
/* File: x86_64/op_iput_byte.S */
/* File: x86_64/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # field ref <- 0000CCCC
    movzbq  rINSTbl, %rcx                   # rcx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG OUT_32_ARG2, rINSTq            # fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet8InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_char: /* 0x5e */
/* File: x86_64/op_iput_char.S */
/* File: x86_64/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # field ref <- 0000CCCC
    movzbq  rINSTbl, %rcx                   # rcx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG OUT_32_ARG2, rINSTq            # fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet16InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_short: /* 0x5f */
/* File: x86_64/op_iput_short.S */
/* File: x86_64/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # field ref <- 0000CCCC
    movzbq  rINSTbl, %rcx                   # rcx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG OUT_32_ARG2, rINSTq            # fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet16InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget: /* 0x60 */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGet32StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGet32StaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sget_wide: /* 0x61 */
/* File: x86_64/op_sget_wide.S */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGet64StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGet64StaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 1
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_object: /* 0x62 */
/* File: x86_64/op_sget_object.S */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGetObjStaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGetObjStaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 1
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_boolean: /* 0x63 */
/* File: x86_64/op_sget_boolean.S */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGetBooleanStaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGetBooleanStaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_byte: /* 0x64 */
/* File: x86_64/op_sget_byte.S */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGetByteStaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGetByteStaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_char: /* 0x65 */
/* File: x86_64/op_sget_char.S */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGetCharStaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGetCharStaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_short: /* 0x66 */
/* File: x86_64/op_sget_short.S */
/* File: x86_64/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern artGetShortStaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL(artGetShortStaticFromCode)
    movq    rSELF, %rcx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if 0
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput: /* 0x67 */
/* File: x86_64/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet32StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    GET_VREG OUT_32_ARG1, rINSTq            # fp[AA]
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet32StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sput_wide: /* 0x68 */
/* File: x86_64/op_sput_wide.S */
/*
 * SPUT_WIDE handler wrapper.
 *
 */
    /* sput-wide vAA, field@BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    leaq    VREG_ADDRESS(rINSTq), OUT_ARG2  # &fp[AA]
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet64IndirectStaticFromMterp)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sput_object: /* 0x69 */
/* File: x86_64/op_sput_object.S */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST 105
    movq    rINSTq, OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpSputObject)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sput_boolean: /* 0x6a */
/* File: x86_64/op_sput_boolean.S */
/* File: x86_64/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet8StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    GET_VREG OUT_32_ARG1, rINSTq            # fp[AA]
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet8StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput_byte: /* 0x6b */
/* File: x86_64/op_sput_byte.S */
/* File: x86_64/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet8StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    GET_VREG OUT_32_ARG1, rINSTq            # fp[AA]
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet8StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput_char: /* 0x6c */
/* File: x86_64/op_sput_char.S */
/* File: x86_64/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet16StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    GET_VREG OUT_32_ARG1, rINSTq            # fp[AA]
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet16StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput_short: /* 0x6d */
/* File: x86_64/op_sput_short.S */
/* File: x86_64/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet16StaticFromCode
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    GET_VREG OUT_32_ARG1, rINSTq            # fp[AA]
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet16StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual: /* 0x6e */
/* File: x86_64/op_invoke_virtual.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtual
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 110
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeVirtual)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT

/*
 * Handle a virtual method call.
 *
 * for: invoke-virtual, invoke-virtual/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op vAA, {vCCCC..v(CCCC+AA-1)}, meth@BBBB */

/* ------------------------------ */
    .balign 128
.L_op_invoke_super: /* 0x6f */
/* File: x86_64/op_invoke_super.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuper
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 111
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeSuper)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT

/*
 * Handle a "super" method call.
 *
 * for: invoke-super, invoke-super/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op vAA, {vCCCC..v(CCCC+AA-1)}, meth@BBBB */

/* ------------------------------ */
    .balign 128
.L_op_invoke_direct: /* 0x70 */
/* File: x86_64/op_invoke_direct.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirect
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 112
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeDirect)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_static: /* 0x71 */
/* File: x86_64/op_invoke_static.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStatic
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 113
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeStatic)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT



/* ------------------------------ */
    .balign 128
.L_op_invoke_interface: /* 0x72 */
/* File: x86_64/op_invoke_interface.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterface
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 114
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeInterface)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT

/*
 * Handle an interface method call.
 *
 * for: invoke-interface, invoke-interface/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */

/* ------------------------------ */
    .balign 128
.L_op_return_void_no_barrier: /* 0x73 */
/* File: x86_64/op_return_void_no_barrier.S */
    movq    rSELF, OUT_ARG0
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    xorq    %rax, %rax
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range: /* 0x74 */
/* File: x86_64/op_invoke_virtual_range.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualRange
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 116
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeVirtualRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_super_range: /* 0x75 */
/* File: x86_64/op_invoke_super_range.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuperRange
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 117
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeSuperRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_direct_range: /* 0x76 */
/* File: x86_64/op_invoke_direct_range.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirectRange
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 118
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeDirectRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_static_range: /* 0x77 */
/* File: x86_64/op_invoke_static_range.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStaticRange
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 119
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeStaticRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_interface_range: /* 0x78 */
/* File: x86_64/op_invoke_interface_range.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterfaceRange
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 120
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeInterfaceRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_unused_79: /* 0x79 */
/* File: x86_64/op_unused_79.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_7a: /* 0x7a */
/* File: x86_64/op_unused_7a.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_neg_int: /* 0x7b */
/* File: x86_64/op_neg_int.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

    negl    %eax
    .if 0
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_not_int: /* 0x7c */
/* File: x86_64/op_not_int.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

    notl    %eax
    .if 0
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_neg_long: /* 0x7d */
/* File: x86_64/op_neg_long.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

    negq    %rax
    .if 1
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_not_long: /* 0x7e */
/* File: x86_64/op_not_long.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

    notq    %rax
    .if 1
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_neg_float: /* 0x7f */
/* File: x86_64/op_neg_float.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

    xorl    $0x80000000, %eax
    .if 0
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_neg_double: /* 0x80 */
/* File: x86_64/op_neg_double.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A
    movq    $0x8000000000000000, %rsi
    xorq    %rsi, %rax
    .if 1
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_long: /* 0x81 */
/* File: x86_64/op_int_to_long.S */
    /* int to long vA, vB */
    movzbq  rINSTbl, %rax                   # rax <- +A
    sarl    $4, %eax                       # eax <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    movslq  VREG_ADDRESS(%rax), %rax
    SET_WIDE_VREG %rax, rINSTq              # v[A] <- %rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_float: /* 0x82 */
/* File: x86_64/op_int_to_float.S */
/* File: x86_64/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    cvtsi2ssl    VREG_ADDRESS(rINSTq), %xmm0
    .if 0
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_double: /* 0x83 */
/* File: x86_64/op_int_to_double.S */
/* File: x86_64/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    cvtsi2sdl    VREG_ADDRESS(rINSTq), %xmm0
    .if 1
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_long_to_int: /* 0x84 */
/* File: x86_64/op_long_to_int.S */
/* we ignore the high word, making this equivalent to a 32-bit reg move */
/* File: x86_64/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movl    rINST, %eax                     # eax <- BA
    andb    $0xf, %al                      # eax <- A
    shrl    $4, rINST                      # rINST <- B
    GET_VREG %edx, rINSTq
    .if 0
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_long_to_float: /* 0x85 */
/* File: x86_64/op_long_to_float.S */
/* File: x86_64/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    cvtsi2ssq    VREG_ADDRESS(rINSTq), %xmm0
    .if 0
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_long_to_double: /* 0x86 */
/* File: x86_64/op_long_to_double.S */
/* File: x86_64/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    cvtsi2sdq    VREG_ADDRESS(rINSTq), %xmm0
    .if 1
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_float_to_int: /* 0x87 */
/* File: x86_64/op_float_to_int.S */
/* File: x86_64/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.
 */
    /* float/double to int/long vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    movss   VREG_ADDRESS(rINSTq), %xmm0
    movl  $0x7fffffff, %eax
    cvtsi2ssl %eax, %xmm1
    comiss    %xmm1, %xmm0
    jae     1f
    jp      2f
    cvttss2sil  %xmm0, %eax
    jmp     1f
2:
    xorl    %eax, %eax
1:
    .if 0
    SET_WIDE_VREG %eax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_float_to_long: /* 0x88 */
/* File: x86_64/op_float_to_long.S */
/* File: x86_64/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.
 */
    /* float/double to int/long vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    movss   VREG_ADDRESS(rINSTq), %xmm0
    movq  $0x7fffffffffffffff, %rax
    cvtsi2ssq %rax, %xmm1
    comiss    %xmm1, %xmm0
    jae     1f
    jp      2f
    cvttss2siq  %xmm0, %rax
    jmp     1f
2:
    xorq    %rax, %rax
1:
    .if 1
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %rax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_float_to_double: /* 0x89 */
/* File: x86_64/op_float_to_double.S */
/* File: x86_64/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    cvtss2sd    VREG_ADDRESS(rINSTq), %xmm0
    .if 1
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_double_to_int: /* 0x8a */
/* File: x86_64/op_double_to_int.S */
/* File: x86_64/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.
 */
    /* float/double to int/long vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    movsd   VREG_ADDRESS(rINSTq), %xmm0
    movl  $0x7fffffff, %eax
    cvtsi2sdl %eax, %xmm1
    comisd    %xmm1, %xmm0
    jae     1f
    jp      2f
    cvttsd2sil  %xmm0, %eax
    jmp     1f
2:
    xorl    %eax, %eax
1:
    .if 0
    SET_WIDE_VREG %eax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_double_to_long: /* 0x8b */
/* File: x86_64/op_double_to_long.S */
/* File: x86_64/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.
 */
    /* float/double to int/long vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    movsd   VREG_ADDRESS(rINSTq), %xmm0
    movq  $0x7fffffffffffffff, %rax
    cvtsi2sdq %rax, %xmm1
    comisd    %xmm1, %xmm0
    jae     1f
    jp      2f
    cvttsd2siq  %xmm0, %rax
    jmp     1f
2:
    xorq    %rax, %rax
1:
    .if 1
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %rax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_double_to_float: /* 0x8c */
/* File: x86_64/op_double_to_float.S */
/* File: x86_64/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    cvtsd2ss    VREG_ADDRESS(rINSTq), %xmm0
    .if 0
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_byte: /* 0x8d */
/* File: x86_64/op_int_to_byte.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

movsbl  %al, %eax
    .if 0
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_char: /* 0x8e */
/* File: x86_64/op_int_to_char.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

movzwl  %ax,%eax
    .if 0
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_short: /* 0x8f */
/* File: x86_64/op_int_to_short.S */
/* File: x86_64/unop.S */
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4,rINST                       # rINST <- B
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $0xf,%cl                       # ecx <- A

movswl %ax, %eax
    .if 0
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_add_int: /* 0x90 */
/* File: x86_64/op_add_int.S */
/* File: x86_64/binop.S */
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    addl    (rFP,%rcx,4), %eax                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_int: /* 0x91 */
/* File: x86_64/op_sub_int.S */
/* File: x86_64/binop.S */
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    subl    (rFP,%rcx,4), %eax                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_int: /* 0x92 */
/* File: x86_64/op_mul_int.S */
/* File: x86_64/binop.S */
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    imull   (rFP,%rcx,4), %eax                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_int: /* 0x93 */
/* File: x86_64/op_div_int.S */
/* File: x86_64/bindiv.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    .if 0
    GET_WIDE_VREG %rax, %rax                # eax <- vBB
    GET_WIDE_VREG %ecx, %rcx             # ecx <- vCC
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    GET_VREG %ecx, %rcx                  # ecx <- vCC
    .endif
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl  $-1, %ecx
    je      2f
    cdq                                    # rdx:rax <- sign-extended of rax
    idivl   %ecx
1:
    .if 0
    SET_WIDE_VREG %eax, rINSTq           # eax <- vBB
    .else
    SET_VREG %eax, rINSTq                # eax <- vBB
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 0
    xorl %eax, %eax
    .else
    negl %eax
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_rem_int: /* 0x94 */
/* File: x86_64/op_rem_int.S */
/* File: x86_64/bindiv.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    .if 0
    GET_WIDE_VREG %rax, %rax                # eax <- vBB
    GET_WIDE_VREG %ecx, %rcx             # ecx <- vCC
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    GET_VREG %ecx, %rcx                  # ecx <- vCC
    .endif
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl  $-1, %ecx
    je      2f
    cdq                                    # rdx:rax <- sign-extended of rax
    idivl   %ecx
1:
    .if 0
    SET_WIDE_VREG %edx, rINSTq           # eax <- vBB
    .else
    SET_VREG %edx, rINSTq                # eax <- vBB
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 1
    xorl %edx, %edx
    .else
    negl %edx
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_and_int: /* 0x95 */
/* File: x86_64/op_and_int.S */
/* File: x86_64/binop.S */
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    andl    (rFP,%rcx,4), %eax                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_int: /* 0x96 */
/* File: x86_64/op_or_int.S */
/* File: x86_64/binop.S */
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    orl     (rFP,%rcx,4), %eax                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_int: /* 0x97 */
/* File: x86_64/op_xor_int.S */
/* File: x86_64/binop.S */
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    xorl    (rFP,%rcx,4), %eax                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shl_int: /* 0x98 */
/* File: x86_64/op_shl_int.S */
/* File: x86_64/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if 0
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    sall    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    sall    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shr_int: /* 0x99 */
/* File: x86_64/op_shr_int.S */
/* File: x86_64/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if 0
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    sarl    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    sarl    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_ushr_int: /* 0x9a */
/* File: x86_64/op_ushr_int.S */
/* File: x86_64/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if 0
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    shrl    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    shrl    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_add_long: /* 0x9b */
/* File: x86_64/op_add_long.S */
/* File: x86_64/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    addq    (rFP,%rcx,4), %rax                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_long: /* 0x9c */
/* File: x86_64/op_sub_long.S */
/* File: x86_64/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    subq    (rFP,%rcx,4), %rax                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_long: /* 0x9d */
/* File: x86_64/op_mul_long.S */
/* File: x86_64/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    imulq   (rFP,%rcx,4), %rax                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_long: /* 0x9e */
/* File: x86_64/op_div_long.S */
/* File: x86_64/bindiv.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    .if 1
    GET_WIDE_VREG %rax, %rax                # eax <- vBB
    GET_WIDE_VREG %rcx, %rcx             # ecx <- vCC
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    GET_VREG %rcx, %rcx                  # ecx <- vCC
    .endif
    testq   %rcx, %rcx
    jz      common_errDivideByZero
    cmpq  $-1, %rcx
    je      2f
    cqo                                    # rdx:rax <- sign-extended of rax
    idivq   %rcx
1:
    .if 1
    SET_WIDE_VREG %rax, rINSTq           # eax <- vBB
    .else
    SET_VREG %rax, rINSTq                # eax <- vBB
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 0
    xorq %rax, %rax
    .else
    negq %rax
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_rem_long: /* 0x9f */
/* File: x86_64/op_rem_long.S */
/* File: x86_64/bindiv.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    .if 1
    GET_WIDE_VREG %rax, %rax                # eax <- vBB
    GET_WIDE_VREG %rcx, %rcx             # ecx <- vCC
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    GET_VREG %rcx, %rcx                  # ecx <- vCC
    .endif
    testq   %rcx, %rcx
    jz      common_errDivideByZero
    cmpq  $-1, %rcx
    je      2f
    cqo                                    # rdx:rax <- sign-extended of rax
    idivq   %rcx
1:
    .if 1
    SET_WIDE_VREG %rdx, rINSTq           # eax <- vBB
    .else
    SET_VREG %rdx, rINSTq                # eax <- vBB
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 1
    xorq %rdx, %rdx
    .else
    negq %rdx
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_and_long: /* 0xa0 */
/* File: x86_64/op_and_long.S */
/* File: x86_64/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    andq    (rFP,%rcx,4), %rax                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_long: /* 0xa1 */
/* File: x86_64/op_or_long.S */
/* File: x86_64/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    orq     (rFP,%rcx,4), %rax                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_long: /* 0xa2 */
/* File: x86_64/op_xor_long.S */
/* File: x86_64/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    xorq    (rFP,%rcx,4), %rax                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shl_long: /* 0xa3 */
/* File: x86_64/op_shl_long.S */
/* File: x86_64/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if 1
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    salq    %cl, %rax                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    salq    %cl, %rax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shr_long: /* 0xa4 */
/* File: x86_64/op_shr_long.S */
/* File: x86_64/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if 1
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    sarq    %cl, %rax                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    sarq    %cl, %rax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_ushr_long: /* 0xa5 */
/* File: x86_64/op_ushr_long.S */
/* File: x86_64/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if 1
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    shrq    %cl, %rax                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    shrq    %cl, %rax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_add_float: /* 0xa6 */
/* File: x86_64/op_add_float.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movss   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    addss VREG_ADDRESS(%rax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_float: /* 0xa7 */
/* File: x86_64/op_sub_float.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movss   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    subss VREG_ADDRESS(%rax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_float: /* 0xa8 */
/* File: x86_64/op_mul_float.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movss   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    mulss VREG_ADDRESS(%rax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_float: /* 0xa9 */
/* File: x86_64/op_div_float.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movss   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    divss VREG_ADDRESS(%rax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_float: /* 0xaa */
/* File: x86_64/op_rem_float.S */
    /* rem_float vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx <- BB
    movzbq  2(rPC), %rax                    # eax <- CC
    flds    VREG_ADDRESS(%rcx)              # vBB to fp stack
    flds    VREG_ADDRESS(%rax)              # vCC to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(rINSTq)            # %st to vAA
    CLEAR_REF rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_add_double: /* 0xab */
/* File: x86_64/op_add_double.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movsd   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    addsd VREG_ADDRESS(%rax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_double: /* 0xac */
/* File: x86_64/op_sub_double.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movsd   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    subsd VREG_ADDRESS(%rax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_double: /* 0xad */
/* File: x86_64/op_mul_double.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movsd   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    mulsd VREG_ADDRESS(%rax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_double: /* 0xae */
/* File: x86_64/op_div_double.S */
/* File: x86_64/sseBinop.S */
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movsd   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    divsd VREG_ADDRESS(%rax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_double: /* 0xaf */
/* File: x86_64/op_rem_double.S */
    /* rem_double vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx <- BB
    movzbq  2(rPC), %rax                    # eax <- CC
    fldl    VREG_ADDRESS(%rcx)              # %st1 <- fp[vBB]
    fldl    VREG_ADDRESS(%rax)              # %st0 <- fp[vCC]
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(rINSTq)            # fp[vAA] <- %st
    CLEAR_WIDE_REF rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_add_int_2addr: /* 0xb0 */
/* File: x86_64/op_add_int_2addr.S */
/* File: x86_64/binop2addr.S */
/*
 * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = r0 op r1".
 * This could be an instruction or a function call.
 *
 * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
 *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
 *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
 *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, rINSTq                   # eax <- vB
    addl    %eax, (rFP,%rcx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_int_2addr: /* 0xb1 */
/* File: x86_64/op_sub_int_2addr.S */
/* File: x86_64/binop2addr.S */
/*
 * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = r0 op r1".
 * This could be an instruction or a function call.
 *
 * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
 *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
 *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
 *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, rINSTq                   # eax <- vB
    subl    %eax, (rFP,%rcx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_int_2addr: /* 0xb2 */
/* File: x86_64/op_mul_int_2addr.S */
    /* mul vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    imull   (rFP,rINSTq,4), %eax
    SET_VREG %eax, %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_div_int_2addr: /* 0xb3 */
/* File: x86_64/op_div_int_2addr.S */
/* File: x86_64/bindiv2addr.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # rcx <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # eax <- vA
    GET_WIDE_VREG %ecx, %rcx             # ecx <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vA
    GET_VREG %ecx, %rcx                  # ecx <- vB
    .endif
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl  $-1, %ecx
    je      2f
    cdq                                    # rdx:rax <- sign-extended of rax
    idivl   %ecx
1:
    .if 0
    SET_WIDE_VREG %eax, rINSTq           # vA <- result
    .else
    SET_VREG %eax, rINSTq                # vA <- result
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
2:
    .if 0
    xorl %eax, %eax
    .else
    negl %eax
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_rem_int_2addr: /* 0xb4 */
/* File: x86_64/op_rem_int_2addr.S */
/* File: x86_64/bindiv2addr.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # rcx <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # eax <- vA
    GET_WIDE_VREG %ecx, %rcx             # ecx <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vA
    GET_VREG %ecx, %rcx                  # ecx <- vB
    .endif
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl  $-1, %ecx
    je      2f
    cdq                                    # rdx:rax <- sign-extended of rax
    idivl   %ecx
1:
    .if 0
    SET_WIDE_VREG %edx, rINSTq           # vA <- result
    .else
    SET_VREG %edx, rINSTq                # vA <- result
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
2:
    .if 1
    xorl %edx, %edx
    .else
    negl %edx
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_and_int_2addr: /* 0xb5 */
/* File: x86_64/op_and_int_2addr.S */
/* File: x86_64/binop2addr.S */
/*
 * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = r0 op r1".
 * This could be an instruction or a function call.
 *
 * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
 *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
 *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
 *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, rINSTq                   # eax <- vB
    andl    %eax, (rFP,%rcx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_or_int_2addr: /* 0xb6 */
/* File: x86_64/op_or_int_2addr.S */
/* File: x86_64/binop2addr.S */
/*
 * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = r0 op r1".
 * This could be an instruction or a function call.
 *
 * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
 *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
 *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
 *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, rINSTq                   # eax <- vB
    orl     %eax, (rFP,%rcx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_xor_int_2addr: /* 0xb7 */
/* File: x86_64/op_xor_int_2addr.S */
/* File: x86_64/binop2addr.S */
/*
 * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = r0 op r1".
 * This could be an instruction or a function call.
 *
 * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
 *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
 *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
 *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, rINSTq                   # eax <- vB
    xorl    %eax, (rFP,%rcx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shl_int_2addr: /* 0xb8 */
/* File: x86_64/op_shl_int_2addr.S */
/* File: x86_64/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    sall    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    sall    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shr_int_2addr: /* 0xb9 */
/* File: x86_64/op_shr_int_2addr.S */
/* File: x86_64/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    sarl    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    sarl    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_2addr: /* 0xba */
/* File: x86_64/op_ushr_int_2addr.S */
/* File: x86_64/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    shrl    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    shrl    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_add_long_2addr: /* 0xbb */
/* File: x86_64/op_add_long_2addr.S */
/* File: x86_64/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    addq    %rax, (rFP,%rcx,4)                                  # for ex: addq   %rax,(rFP,%rcx,4)
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_long_2addr: /* 0xbc */
/* File: x86_64/op_sub_long_2addr.S */
/* File: x86_64/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    subq    %rax, (rFP,%rcx,4)                                  # for ex: addq   %rax,(rFP,%rcx,4)
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_long_2addr: /* 0xbd */
/* File: x86_64/op_mul_long_2addr.S */
    /* mul vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, %rcx                # rax <- vA
    imulq   (rFP,rINSTq,4), %rax
    SET_WIDE_VREG %rax, %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_div_long_2addr: /* 0xbe */
/* File: x86_64/op_div_long_2addr.S */
/* File: x86_64/bindiv2addr.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # rcx <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # eax <- vA
    GET_WIDE_VREG %rcx, %rcx             # ecx <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vA
    GET_VREG %rcx, %rcx                  # ecx <- vB
    .endif
    testq   %rcx, %rcx
    jz      common_errDivideByZero
    cmpq  $-1, %rcx
    je      2f
    cqo                                    # rdx:rax <- sign-extended of rax
    idivq   %rcx
1:
    .if 1
    SET_WIDE_VREG %rax, rINSTq           # vA <- result
    .else
    SET_VREG %rax, rINSTq                # vA <- result
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
2:
    .if 0
    xorq %rax, %rax
    .else
    negq %rax
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_rem_long_2addr: /* 0xbf */
/* File: x86_64/op_rem_long_2addr.S */
/* File: x86_64/bindiv2addr.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # rcx <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # eax <- vA
    GET_WIDE_VREG %rcx, %rcx             # ecx <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vA
    GET_VREG %rcx, %rcx                  # ecx <- vB
    .endif
    testq   %rcx, %rcx
    jz      common_errDivideByZero
    cmpq  $-1, %rcx
    je      2f
    cqo                                    # rdx:rax <- sign-extended of rax
    idivq   %rcx
1:
    .if 1
    SET_WIDE_VREG %rdx, rINSTq           # vA <- result
    .else
    SET_VREG %rdx, rINSTq                # vA <- result
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
2:
    .if 1
    xorq %rdx, %rdx
    .else
    negq %rdx
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_and_long_2addr: /* 0xc0 */
/* File: x86_64/op_and_long_2addr.S */
/* File: x86_64/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    andq    %rax, (rFP,%rcx,4)                                  # for ex: addq   %rax,(rFP,%rcx,4)
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_or_long_2addr: /* 0xc1 */
/* File: x86_64/op_or_long_2addr.S */
/* File: x86_64/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    orq     %rax, (rFP,%rcx,4)                                  # for ex: addq   %rax,(rFP,%rcx,4)
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_xor_long_2addr: /* 0xc2 */
/* File: x86_64/op_xor_long_2addr.S */
/* File: x86_64/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    xorq    %rax, (rFP,%rcx,4)                                  # for ex: addq   %rax,(rFP,%rcx,4)
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shl_long_2addr: /* 0xc3 */
/* File: x86_64/op_shl_long_2addr.S */
/* File: x86_64/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    salq    %cl, %rax                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    salq    %cl, %rax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shr_long_2addr: /* 0xc4 */
/* File: x86_64/op_shr_long_2addr.S */
/* File: x86_64/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    sarq    %cl, %rax                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    sarq    %cl, %rax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_ushr_long_2addr: /* 0xc5 */
/* File: x86_64/op_ushr_long_2addr.S */
/* File: x86_64/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    shrq    %cl, %rax                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    shrq    %cl, %rax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_add_float_2addr: /* 0xc6 */
/* File: x86_64/op_add_float_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    addss VREG_ADDRESS(rINSTq), %xmm0
    movss %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_float_2addr: /* 0xc7 */
/* File: x86_64/op_sub_float_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    subss VREG_ADDRESS(rINSTq), %xmm0
    movss %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_float_2addr: /* 0xc8 */
/* File: x86_64/op_mul_float_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    mulss VREG_ADDRESS(rINSTq), %xmm0
    movss %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_div_float_2addr: /* 0xc9 */
/* File: x86_64/op_div_float_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    divss VREG_ADDRESS(rINSTq), %xmm0
    movss %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_rem_float_2addr: /* 0xca */
/* File: x86_64/op_rem_float_2addr.S */
    /* rem_float/2addr vA, vB */
    movzbq  rINSTbl, %rcx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    flds    VREG_ADDRESS(rINSTq)            # vB to fp stack
    andb    $0xf, %cl                      # ecx <- A
    flds    VREG_ADDRESS(%rcx)              # vA to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(%rcx)              # %st to vA
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_add_double_2addr: /* 0xcb */
/* File: x86_64/op_add_double_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    addsd VREG_ADDRESS(rINSTq), %xmm0
    movsd %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_double_2addr: /* 0xcc */
/* File: x86_64/op_sub_double_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    subsd VREG_ADDRESS(rINSTq), %xmm0
    movsd %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_double_2addr: /* 0xcd */
/* File: x86_64/op_mul_double_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    mulsd VREG_ADDRESS(rINSTq), %xmm0
    movsd %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_div_double_2addr: /* 0xce */
/* File: x86_64/op_div_double_2addr.S */
/* File: x86_64/sseBinop2Addr.S */
    movl    rINST, %ecx                     # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    divsd VREG_ADDRESS(rINSTq), %xmm0
    movsd %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_rem_double_2addr: /* 0xcf */
/* File: x86_64/op_rem_double_2addr.S */
    /* rem_double/2addr vA, vB */
    movzbq  rINSTbl, %rcx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fldl    VREG_ADDRESS(rINSTq)            # vB to fp stack
    andb    $0xf, %cl                      # ecx <- A
    fldl    VREG_ADDRESS(%rcx)              # vA to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(%rcx)              # %st to vA
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_add_int_lit16: /* 0xd0 */
/* File: x86_64/op_add_int_lit16.S */
/* File: x86_64/binopLit16.S */
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    addl    %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rsub_int: /* 0xd1 */
/* File: x86_64/op_rsub_int.S */
/* this op is "rsub-int", but can be thought of as "rsub-int/lit16" */
/* File: x86_64/binopLit16.S */
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    subl    %eax, %ecx                                  # for example: addl %ecx, %eax
    SET_VREG %ecx, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit16: /* 0xd2 */
/* File: x86_64/op_mul_int_lit16.S */
/* File: x86_64/binopLit16.S */
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    imull   %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_int_lit16: /* 0xd3 */
/* File: x86_64/op_div_int_lit16.S */
/* File: x86_64/bindivLit16.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl    $-1, %ecx
    je      2f
    cdq                                     # rax <- sign-extended of eax
    idivl   %ecx
1:
    SET_VREG %eax, rINSTq                # vA <- result
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 0
    xorl    %eax, %eax
    .else
    negl    %eax
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit16: /* 0xd4 */
/* File: x86_64/op_rem_int_lit16.S */
/* File: x86_64/bindivLit16.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl    $-1, %ecx
    je      2f
    cdq                                     # rax <- sign-extended of eax
    idivl   %ecx
1:
    SET_VREG %edx, rINSTq                # vA <- result
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 1
    xorl    %edx, %edx
    .else
    negl    %edx
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_and_int_lit16: /* 0xd5 */
/* File: x86_64/op_and_int_lit16.S */
/* File: x86_64/binopLit16.S */
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andl    %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit16: /* 0xd6 */
/* File: x86_64/op_or_int_lit16.S */
/* File: x86_64/binopLit16.S */
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    orl     %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit16: /* 0xd7 */
/* File: x86_64/op_xor_int_lit16.S */
/* File: x86_64/binopLit16.S */
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    xorl    %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_add_int_lit8: /* 0xd8 */
/* File: x86_64/op_add_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    addl    %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rsub_int_lit8: /* 0xd9 */
/* File: x86_64/op_rsub_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    subl    %eax, %ecx                                  # ex: addl %ecx,%eax
    SET_VREG %ecx, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit8: /* 0xda */
/* File: x86_64/op_mul_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    imull   %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_int_lit8: /* 0xdb */
/* File: x86_64/op_div_int_lit8.S */
/* File: x86_64/bindivLit8.S */
/*
 * 32-bit div/rem "lit8" binary operation.  Handles special case of
 * op0=minint & op1=-1
 */
    /* div/rem/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %rax                    # eax <- rBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $-1, %ecx
    je      2f
    cdq                                     # rax <- sign-extended of eax
    idivl   %ecx
1:
    SET_VREG %eax, rINSTq                # vA <- result
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 0
    xorl    %eax, %eax
    .else
    negl    %eax
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit8: /* 0xdc */
/* File: x86_64/op_rem_int_lit8.S */
/* File: x86_64/bindivLit8.S */
/*
 * 32-bit div/rem "lit8" binary operation.  Handles special case of
 * op0=minint & op1=-1
 */
    /* div/rem/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %rax                    # eax <- rBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $-1, %ecx
    je      2f
    cdq                                     # rax <- sign-extended of eax
    idivl   %ecx
1:
    SET_VREG %edx, rINSTq                # vA <- result
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if 1
    xorl    %edx, %edx
    .else
    negl    %edx
    .endif
    jmp     1b


/* ------------------------------ */
    .balign 128
.L_op_and_int_lit8: /* 0xdd */
/* File: x86_64/op_and_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    andl    %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit8: /* 0xde */
/* File: x86_64/op_or_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    orl     %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit8: /* 0xdf */
/* File: x86_64/op_xor_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    xorl    %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shl_int_lit8: /* 0xe0 */
/* File: x86_64/op_shl_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    sall    %cl, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shr_int_lit8: /* 0xe1 */
/* File: x86_64/op_shr_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    sarl    %cl, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_lit8: /* 0xe2 */
/* File: x86_64/op_ushr_int_lit8.S */
/* File: x86_64/binopLit8.S */
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    shrl    %cl, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_quick: /* 0xe3 */
/* File: x86_64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf,rINSTbl                   # rINST <- A
    .if 0
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    movl (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iget_wide_quick: /* 0xe4 */
/* File: x86_64/op_iget_wide_quick.S */
/* File: x86_64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf,rINSTbl                   # rINST <- A
    .if 1
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    movswl (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_object_quick: /* 0xe5 */
/* File: x86_64/op_iget_object_quick.S */
    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    .extern artIGetObjectFromMterp
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG0, %rcx              # vB (object we're operating on)
    movzwl  2(rPC), OUT_32_ARG1             # eax <- field byte offset
    EXPORT_PC
    callq   SYMBOL(artIGetObjectFromMterp)  # (obj, offset)
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_quick: /* 0xe6 */
/* File: x86_64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINSTq                  # rINST <- v[A]
    movzwq  2(rPC), %rax                    # rax <- field byte offset
    movl    rINST, (%rcx,%rax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_wide_quick: /* 0xe7 */
/* File: x86_64/op_iput_wide_quick.S */
    /* iput-wide-quick vA, vB, offset@CCCC */
    movzbq    rINSTbl, %rcx                 # rcx<- BA
    sarl      $4, %ecx                     # ecx<- B
    GET_VREG  %ecx, %rcx                    # vB (object we're operating on)
    testl     %ecx, %ecx                    # is object null?
    je        common_errNullObject
    movzwq    2(rPC), %rax                  # rax<- field byte offset
    leaq      (%rcx,%rax,1), %rcx           # ecx<- Address of 64-bit target
    andb      $0xf, rINSTbl                # rINST<- A
    GET_WIDE_VREG %rax, rINSTq              # rax<- fp[A]/fp[A+1]
    movq      %rax, (%rcx)                  # obj.field<- r0/r1
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_object_quick: /* 0xe8 */
/* File: x86_64/op_iput_object_quick.S */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST 232
    movl    rINST, OUT_32_ARG2
    call    SYMBOL(MterpIputObjectQuick)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_quick: /* 0xe9 */
/* File: x86_64/op_invoke_virtual_quick.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuick
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 233
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeVirtualQuick)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range_quick: /* 0xea */
/* File: x86_64/op_invoke_virtual_range_quick.S */
/* File: x86_64/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuickRange
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST 234
    movl    rINST, OUT_32_ARG3
    call    SYMBOL(MterpInvokeVirtualQuickRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_iput_boolean_quick: /* 0xeb */
/* File: x86_64/op_iput_boolean_quick.S */
/* File: x86_64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINSTq                  # rINST <- v[A]
    movzwq  2(rPC), %rax                    # rax <- field byte offset
    movb    rINSTbl, (%rcx,%rax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_byte_quick: /* 0xec */
/* File: x86_64/op_iput_byte_quick.S */
/* File: x86_64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINSTq                  # rINST <- v[A]
    movzwq  2(rPC), %rax                    # rax <- field byte offset
    movb    rINSTbl, (%rcx,%rax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_char_quick: /* 0xed */
/* File: x86_64/op_iput_char_quick.S */
/* File: x86_64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINSTq                  # rINST <- v[A]
    movzwq  2(rPC), %rax                    # rax <- field byte offset
    movw    rINSTw, (%rcx,%rax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_short_quick: /* 0xee */
/* File: x86_64/op_iput_short_quick.S */
/* File: x86_64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINSTq                  # rINST <- v[A]
    movzwq  2(rPC), %rax                    # rax <- field byte offset
    movw    rINSTw, (%rcx,%rax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean_quick: /* 0xef */
/* File: x86_64/op_iget_boolean_quick.S */
/* File: x86_64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf,rINSTbl                   # rINST <- A
    .if 0
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    movsbl (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_byte_quick: /* 0xf0 */
/* File: x86_64/op_iget_byte_quick.S */
/* File: x86_64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf,rINSTbl                   # rINST <- A
    .if 0
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    movsbl (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_char_quick: /* 0xf1 */
/* File: x86_64/op_iget_char_quick.S */
/* File: x86_64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf,rINSTbl                   # rINST <- A
    .if 0
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    movzwl (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_short_quick: /* 0xf2 */
/* File: x86_64/op_iget_short_quick.S */
/* File: x86_64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf,rINSTbl                   # rINST <- A
    .if 0
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    movswl (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_invoke_lambda: /* 0xf3 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_f4: /* 0xf4 */
/* File: x86_64/op_unused_f4.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_capture_variable: /* 0xf5 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_create_lambda: /* 0xf6 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_liberate_variable: /* 0xf7 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_box_lambda: /* 0xf8 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unbox_lambda: /* 0xf9 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fa: /* 0xfa */
/* File: x86_64/op_unused_fa.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fb: /* 0xfb */
/* File: x86_64/op_unused_fb.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fc: /* 0xfc */
/* File: x86_64/op_unused_fc.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fd: /* 0xfd */
/* File: x86_64/op_unused_fd.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fe: /* 0xfe */
/* File: x86_64/op_unused_fe.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_ff: /* 0xff */
/* File: x86_64/op_unused_ff.S */
/* File: x86_64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


    .balign 128
    SIZE(SYMBOL(artMterpAsmInstructionStart),SYMBOL(artMterpAsmInstructionStart))
    .global SYMBOL(artMterpAsmInstructionEnd)
SYMBOL(artMterpAsmInstructionEnd):

/*
 * ===========================================================================
 *  Sister implementations
 * ===========================================================================
 */
    .global SYMBOL(artMterpAsmSisterStart)
    FUNCTION_TYPE(SYMBOL(artMterpAsmSisterStart))
    .text
    .balign 4
SYMBOL(artMterpAsmSisterStart):

    SIZE(SYMBOL(artMterpAsmSisterStart),SYMBOL(artMterpAsmSisterStart))
    .global SYMBOL(artMterpAsmSisterEnd)
SYMBOL(artMterpAsmSisterEnd):


    .global SYMBOL(artMterpAsmAltInstructionStart)
    FUNCTION_TYPE(SYMBOL(artMterpAsmAltInstructionStart))
    .text

SYMBOL(artMterpAsmAltInstructionStart) = .L_ALT_op_nop
/* ------------------------------ */
    .balign 128
.L_ALT_op_nop: /* 0x00 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(0*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move: /* 0x01 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(1*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_from16: /* 0x02 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(2*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_16: /* 0x03 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(3*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide: /* 0x04 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(4*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_from16: /* 0x05 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(5*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_16: /* 0x06 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(6*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object: /* 0x07 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(7*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_from16: /* 0x08 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(8*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_16: /* 0x09 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(9*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result: /* 0x0a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(10*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_wide: /* 0x0b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(11*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_object: /* 0x0c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(12*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_exception: /* 0x0d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(13*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void: /* 0x0e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(14*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return: /* 0x0f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(15*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_wide: /* 0x10 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(16*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_object: /* 0x11 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(17*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_4: /* 0x12 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(18*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_16: /* 0x13 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(19*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const: /* 0x14 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(20*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_high16: /* 0x15 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(21*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_16: /* 0x16 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(22*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_32: /* 0x17 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(23*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide: /* 0x18 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(24*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_high16: /* 0x19 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(25*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string: /* 0x1a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(26*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string_jumbo: /* 0x1b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(27*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_class: /* 0x1c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(28*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_enter: /* 0x1d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(29*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_exit: /* 0x1e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(30*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_check_cast: /* 0x1f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(31*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_instance_of: /* 0x20 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(32*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_array_length: /* 0x21 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(33*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_instance: /* 0x22 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(34*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_array: /* 0x23 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(35*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array: /* 0x24 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(36*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array_range: /* 0x25 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(37*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_fill_array_data: /* 0x26 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(38*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_throw: /* 0x27 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(39*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto: /* 0x28 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(40*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_16: /* 0x29 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(41*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_32: /* 0x2a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(42*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_packed_switch: /* 0x2b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(43*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sparse_switch: /* 0x2c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(44*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_float: /* 0x2d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(45*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_float: /* 0x2e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(46*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_double: /* 0x2f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(47*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_double: /* 0x30 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(48*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmp_long: /* 0x31 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(49*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eq: /* 0x32 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(50*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ne: /* 0x33 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(51*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lt: /* 0x34 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(52*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ge: /* 0x35 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(53*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gt: /* 0x36 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(54*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_le: /* 0x37 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(55*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eqz: /* 0x38 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(56*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_nez: /* 0x39 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(57*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ltz: /* 0x3a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(58*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gez: /* 0x3b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(59*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gtz: /* 0x3c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(60*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lez: /* 0x3d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(61*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3e: /* 0x3e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(62*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3f: /* 0x3f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(63*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_40: /* 0x40 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(64*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_41: /* 0x41 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(65*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_42: /* 0x42 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(66*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_43: /* 0x43 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(67*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget: /* 0x44 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(68*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_wide: /* 0x45 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(69*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_object: /* 0x46 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(70*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_boolean: /* 0x47 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(71*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_byte: /* 0x48 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(72*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_char: /* 0x49 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(73*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_short: /* 0x4a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(74*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput: /* 0x4b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(75*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_wide: /* 0x4c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(76*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_object: /* 0x4d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(77*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_boolean: /* 0x4e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(78*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_byte: /* 0x4f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(79*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_char: /* 0x50 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(80*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_short: /* 0x51 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(81*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget: /* 0x52 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(82*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide: /* 0x53 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(83*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object: /* 0x54 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(84*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean: /* 0x55 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(85*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte: /* 0x56 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(86*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char: /* 0x57 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(87*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short: /* 0x58 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(88*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput: /* 0x59 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(89*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide: /* 0x5a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(90*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object: /* 0x5b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(91*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean: /* 0x5c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(92*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte: /* 0x5d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(93*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char: /* 0x5e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(94*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short: /* 0x5f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(95*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget: /* 0x60 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(96*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_wide: /* 0x61 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(97*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_object: /* 0x62 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(98*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_boolean: /* 0x63 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(99*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_byte: /* 0x64 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(100*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_char: /* 0x65 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(101*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_short: /* 0x66 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(102*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput: /* 0x67 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(103*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_wide: /* 0x68 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(104*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_object: /* 0x69 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(105*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_boolean: /* 0x6a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(106*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_byte: /* 0x6b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(107*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_char: /* 0x6c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(108*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_short: /* 0x6d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(109*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual: /* 0x6e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(110*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super: /* 0x6f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(111*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct: /* 0x70 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(112*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static: /* 0x71 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(113*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface: /* 0x72 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(114*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void_no_barrier: /* 0x73 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(115*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range: /* 0x74 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(116*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super_range: /* 0x75 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(117*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct_range: /* 0x76 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(118*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static_range: /* 0x77 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(119*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface_range: /* 0x78 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(120*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_79: /* 0x79 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(121*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_7a: /* 0x7a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(122*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_int: /* 0x7b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(123*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_int: /* 0x7c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(124*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_long: /* 0x7d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(125*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_long: /* 0x7e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(126*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_float: /* 0x7f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(127*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_double: /* 0x80 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(128*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_long: /* 0x81 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(129*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_float: /* 0x82 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(130*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_double: /* 0x83 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(131*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_int: /* 0x84 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(132*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_float: /* 0x85 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(133*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_double: /* 0x86 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(134*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_int: /* 0x87 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(135*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_long: /* 0x88 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(136*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_double: /* 0x89 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(137*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_int: /* 0x8a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(138*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_long: /* 0x8b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(139*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_float: /* 0x8c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(140*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_byte: /* 0x8d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(141*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_char: /* 0x8e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(142*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_short: /* 0x8f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(143*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int: /* 0x90 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(144*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int: /* 0x91 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(145*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int: /* 0x92 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(146*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int: /* 0x93 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(147*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int: /* 0x94 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(148*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int: /* 0x95 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(149*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int: /* 0x96 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(150*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int: /* 0x97 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(151*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int: /* 0x98 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(152*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int: /* 0x99 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(153*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int: /* 0x9a */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(154*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long: /* 0x9b */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(155*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long: /* 0x9c */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(156*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long: /* 0x9d */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(157*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long: /* 0x9e */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(158*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long: /* 0x9f */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(159*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long: /* 0xa0 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(160*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long: /* 0xa1 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(161*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long: /* 0xa2 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(162*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long: /* 0xa3 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(163*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long: /* 0xa4 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(164*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long: /* 0xa5 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(165*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float: /* 0xa6 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(166*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float: /* 0xa7 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(167*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float: /* 0xa8 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(168*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float: /* 0xa9 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(169*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float: /* 0xaa */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(170*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double: /* 0xab */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(171*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double: /* 0xac */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(172*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double: /* 0xad */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(173*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double: /* 0xae */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(174*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double: /* 0xaf */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(175*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_2addr: /* 0xb0 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(176*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int_2addr: /* 0xb1 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(177*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_2addr: /* 0xb2 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(178*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_2addr: /* 0xb3 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(179*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_2addr: /* 0xb4 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(180*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_2addr: /* 0xb5 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(181*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_2addr: /* 0xb6 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(182*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_2addr: /* 0xb7 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(183*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_2addr: /* 0xb8 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(184*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_2addr: /* 0xb9 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(185*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_2addr: /* 0xba */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(186*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long_2addr: /* 0xbb */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(187*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long_2addr: /* 0xbc */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(188*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long_2addr: /* 0xbd */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(189*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long_2addr: /* 0xbe */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(190*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long_2addr: /* 0xbf */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(191*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long_2addr: /* 0xc0 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(192*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long_2addr: /* 0xc1 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(193*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long_2addr: /* 0xc2 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(194*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long_2addr: /* 0xc3 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(195*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long_2addr: /* 0xc4 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(196*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long_2addr: /* 0xc5 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(197*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float_2addr: /* 0xc6 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(198*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float_2addr: /* 0xc7 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(199*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float_2addr: /* 0xc8 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(200*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float_2addr: /* 0xc9 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(201*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float_2addr: /* 0xca */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(202*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double_2addr: /* 0xcb */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(203*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double_2addr: /* 0xcc */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(204*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double_2addr: /* 0xcd */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(205*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double_2addr: /* 0xce */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(206*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double_2addr: /* 0xcf */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(207*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit16: /* 0xd0 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(208*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int: /* 0xd1 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(209*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit16: /* 0xd2 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(210*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit16: /* 0xd3 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(211*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit16: /* 0xd4 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(212*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit16: /* 0xd5 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(213*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit16: /* 0xd6 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(214*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit16: /* 0xd7 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(215*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit8: /* 0xd8 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(216*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int_lit8: /* 0xd9 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(217*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit8: /* 0xda */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(218*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit8: /* 0xdb */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(219*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit8: /* 0xdc */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(220*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit8: /* 0xdd */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(221*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit8: /* 0xde */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(222*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit8: /* 0xdf */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(223*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_lit8: /* 0xe0 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(224*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_lit8: /* 0xe1 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(225*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_lit8: /* 0xe2 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(226*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_quick: /* 0xe3 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(227*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide_quick: /* 0xe4 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(228*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object_quick: /* 0xe5 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(229*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_quick: /* 0xe6 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(230*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide_quick: /* 0xe7 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(231*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object_quick: /* 0xe8 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(232*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_quick: /* 0xe9 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(233*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range_quick: /* 0xea */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(234*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean_quick: /* 0xeb */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(235*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte_quick: /* 0xec */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(236*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char_quick: /* 0xed */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(237*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short_quick: /* 0xee */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(238*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean_quick: /* 0xef */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(239*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte_quick: /* 0xf0 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(240*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char_quick: /* 0xf1 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(241*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short_quick: /* 0xf2 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(242*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_lambda: /* 0xf3 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(243*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_f4: /* 0xf4 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(244*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_capture_variable: /* 0xf5 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(245*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_create_lambda: /* 0xf6 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(246*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_liberate_variable: /* 0xf7 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(247*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_box_lambda: /* 0xf8 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(248*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unbox_lambda: /* 0xf9 */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(249*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fa: /* 0xfa */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(250*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fb: /* 0xfb */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(251*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fc: /* 0xfc */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(252*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fd: /* 0xfd */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(253*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fe: /* 0xfe */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(254*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_ff: /* 0xff */
/* File: x86_64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    jmp     .L_op_nop+(255*128)

    .balign 128
    SIZE(SYMBOL(artMterpAsmAltInstructionStart),SYMBOL(artMterpAsmAltInstructionStart))
    .global SYMBOL(artMterpAsmAltInstructionEnd)
SYMBOL(artMterpAsmAltInstructionEnd):
/* File: x86_64/footer.S */
/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */

    .text
    .align  2

/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogDivideByZeroException)
#endif
    jmp     MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogArrayIndexException)
#endif
    jmp     MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNegativeArraySizeException)
#endif
    jmp     MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNoSuchMethodException)
#endif
    jmp     MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNullObjectException)
#endif
    jmp     MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogExceptionThrownException)
#endif
    jmp     MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    THREAD_FLAGS_OFFSET(OUT_ARG0), OUT_32_ARG2
    call    SYMBOL(MterpLogSuspendFallback)
#endif
    jmp     MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    movq    rSELF, %rcx
    cmpq    $0, THREAD_EXCEPTION_OFFSET(%rcx)
    jz      MterpFallback
    /* intentional fallthrough - handle pending exception. */

/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpHandleException)
    testb   %al, %al
    jz      MterpExceptionReturn
    movq    OFF_FP_CODE_ITEM(rFP), %rax
    mov     OFF_FP_DEX_PC(rFP), %ecx
    leaq    CODEITEM_INSNS_OFFSET(%rax), rPC
    leaq    (rPC, %rcx, 2), rPC
    movq    rPC, OFF_FP_DEX_PC_PTR(rFP)
    /* Do we need to switch interpreters? */
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    /* resume execution at catch block */
    REFRESH_IBASE
    FETCH_INST
    GOTO_NEXT
    /* NOTE: no fallthrough */

/*
 * Common handling for branches with support for Jit profiling.
 * On entry:
 *    rINST          <= signed offset
 *    rPROFILE       <= signed hotness countdown (expanded to 32 bits)
 *    condition bits <= set to establish sign of offset (use "NoFlags" entry if not)
 *
 * We have quite a few different cases for branch profiling, OSR detection and
 * suspend check support here.
 *
 * Taken backward branches:
 *    If profiling active, do hotness countdown and report if we hit zero.
 *    If in osr check mode, see if our target is a compiled loop header entry and do OSR if so.
 *    Is there a pending suspend request?  If so, suspend.
 *
 * Taken forward branches and not-taken backward branches:
 *    If in osr check mode, see if our target is a compiled loop header entry and do OSR if so.
 *
 * Our most common case is expected to be a taken backward branch with active jit profiling,
 * but no full OSR check and no pending suspend request.
 * Next most common case is not-taken branch with no full OSR check.
 *
 */
MterpCommonTakenBranch:
    jg      .L_forward_branch               # don't add forward branches to hotness
/*
 * We need to subtract 1 from positive values and we should not see 0 here,
 * so we may use the result of the comparison with -1.
 */
#if JIT_CHECK_OSR != -1
#  error "JIT_CHECK_OSR must be -1."
#endif
    cmpl    $JIT_CHECK_OSR, rPROFILE
    je      .L_osr_check
    decl    rPROFILE
    je      .L_add_batch                    # counted down to zero - report
.L_resume_backward_branch:
    movq    rSELF, %rax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%rax)
    REFRESH_IBASE
    leaq    (rPC, rINSTq, 2), rPC
    FETCH_INST
    jnz     .L_suspend_request_pending
    GOTO_NEXT

.L_suspend_request_pending:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    call    SYMBOL(MterpSuspendCheck)       # (self)
    testb   %al, %al
    jnz     MterpFallback
    REFRESH_IBASE                           # might have changed during suspend
    GOTO_NEXT

.L_no_count_backwards:
    cmpl    $JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    jne     .L_resume_backward_branch
.L_osr_check:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jz      .L_resume_backward_branch
    jmp     MterpOnStackReplacement

.L_forward_branch:
    cmpl    $JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    je      .L_check_osr_forward
.L_resume_forward_branch:
    leaq    (rPC, rINSTq, 2), rPC
    FETCH_INST
    GOTO_NEXT

.L_check_osr_forward:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jz      .L_resume_forward_branch
    jmp     MterpOnStackReplacement

.L_add_batch:
    movl    rPROFILE, %eax
    movq    OFF_FP_METHOD(rFP), OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movw    %ax, OFF_FP_COUNTDOWN_OFFSET(rFP)
    movq    rSELF, OUT_ARG2
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    movswl  %ax, rPROFILE
    jmp     .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    $2, OUT_32_ARG2
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jnz     MterpOnStackReplacement
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    rINST, OUT_32_ARG2
    call    SYMBOL(MterpLogOSR)
#endif
    movl    $1, %eax
    jmp     MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogFallback)
#endif
MterpCommonFallback:
    xorl    %eax, %eax
    jmp     MterpDone

/*
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    movl    $1, %eax
    jmp     MterpDone
MterpReturn:
    movq    OFF_FP_RESULT_REGISTER(rFP), %rdx
    movq    %rax, (%rdx)
    movl    $1, %eax
MterpDone:
/*
 * At this point, we expect rPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending rPROFILE and the cached hotness counter).  rPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    testl   rPROFILE, rPROFILE
    jle     MRestoreFrame                   # if > 0, we may have some counts to report.

    movl    %eax, rINST                     # stash return value
    /* Report cached hotness counts */
    movl    rPROFILE, %eax
    movq    OFF_FP_METHOD(rFP), OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movw    %ax, OFF_FP_COUNTDOWN_OFFSET(rFP)
    movq    rSELF, OUT_ARG2
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    movl    rINST, %eax                     # restore return value

    /* pop up frame */
MRestoreFrame:
    addq    $FRAME_SIZE, %rsp
    .cfi_adjust_cfa_offset -FRAME_SIZE

    /* Restore callee save register */
    POP %r15
    POP %r14
    POP %r13
    POP %r12
    POP %rbp
    POP %rbx
    ret
    .cfi_endproc
    SIZE(ExecuteMterpImpl,ExecuteMterpImpl)

