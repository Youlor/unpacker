/*
 * This file was generated automatically by gen-mterp.py for 'x86'.
 *
 * --> DO NOT EDIT <--
 */

/* File: x86/header.S */
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
x86 ABI general notes:

Caller save set:
   eax, edx, ecx, st(0)-st(7)
Callee save set:
   ebx, esi, edi, ebp
Return regs:
   32-bit in eax
   64-bit in edx:eax (low-order 32 in eax)
   fp on top of fp stack st(0)

Parameters passed on stack, pushed right-to-left.  On entry to target, first
parm is at 4(%esp).  Traditional entry code is:

functEntry:
    push    %ebp             # save old frame pointer
    mov     %ebp,%esp        # establish new frame pointer
    sub     FrameSize,%esp   # Allocate storage for spill, locals & outs

Once past the prologue, arguments are referenced at ((argno + 2)*4)(%ebp)

Stack must be 16-byte aligned to support SSE in native code.

If we're not doing variable stack allocation (alloca), the frame pointer can be
eliminated and all arg references adjusted to be esp relative.
*/

/*
Mterp and x86 notes:

Some key interpreter variables will be assigned to registers.

  nick     reg   purpose
  rPC      esi   interpreted program counter, used for fetching instructions
  rFP      edi   interpreted frame pointer, used for accessing locals and args
  rINSTw   bx    first 16-bit code of current instruction
  rINSTbl  bl    opcode portion of instruction word
  rINSTbh  bh    high byte of inst word, usually contains src/tgt reg names
  rIBASE   edx   base of instruction handler table
  rREFS    ebp   base of object references in shadow frame.

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
    pushl \_reg
    .cfi_adjust_cfa_offset 4
    .cfi_rel_offset \_reg, 0
.endm

.macro POP _reg
    popl \_reg
    .cfi_adjust_cfa_offset -4
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
#define OFF_FP_SHADOWFRAME OFF_FP(0)

/* Frame size must be 16-byte aligned.
 * Remember about 4 bytes for return address + 4 * 4 for spills
 */
#define FRAME_SIZE     28

/* Frame diagram while executing ExecuteMterpImpl, high to low addresses */
#define IN_ARG3        (FRAME_SIZE + 16 + 16)
#define IN_ARG2        (FRAME_SIZE + 16 + 12)
#define IN_ARG1        (FRAME_SIZE + 16 +  8)
#define IN_ARG0        (FRAME_SIZE + 16 +  4)
/* Spill offsets relative to %esp */
#define LOCAL0         (FRAME_SIZE -  4)
#define LOCAL1         (FRAME_SIZE -  8)
#define LOCAL2         (FRAME_SIZE - 12)
/* Out Arg offsets, relative to %esp */
#define OUT_ARG3       ( 12)
#define OUT_ARG2       (  8)
#define OUT_ARG1       (  4)
#define OUT_ARG0       (  0)  /* <- ExecuteMterpImpl esp + 0 */

/* During bringup, we'll use the shadow frame model instead of rFP */
/* single-purpose registers, given names for clarity */
#define rSELF    IN_ARG0(%esp)
#define rPC      %esi
#define rFP      %edi
#define rINST    %ebx
#define rINSTw   %bx
#define rINSTbh  %bh
#define rINSTbl  %bl
#define rIBASE   %edx
#define rREFS    %ebp
#define rPROFILE OFF_FP_COUNTDOWN_OFFSET(rFP)

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
    movl    rPC, OFF_FP_DEX_PC_PTR(rFP)
.endm

/*
 * Refresh handler table.
 */
.macro REFRESH_IBASE
    movl    rSELF, rIBASE
    movl    THREAD_CURRENT_IBASE_OFFSET(rIBASE), rIBASE
.endm

/*
 * Refresh handler table.
 * IBase handles uses the caller save register so we must restore it after each call.
 * Also it is used as a result of some 64-bit operations (like imul) and we should
 * restore it in such cases also.
 *
 * TODO: Consider spilling the IBase instead of restoring it from Thread structure.
 */
.macro RESTORE_IBASE
    movl    rSELF, rIBASE
    movl    THREAD_CURRENT_IBASE_OFFSET(rIBASE), rIBASE
.endm

/*
 * If rSELF is already loaded then we can use it from known reg.
 */
.macro RESTORE_IBASE_FROM_SELF _reg
    movl    THREAD_CURRENT_IBASE_OFFSET(\_reg), rIBASE
.endm

/*
 * Refresh rINST.
 * At enter to handler rINST does not contain the opcode number.
 * However some utilities require the full value, so this macro
 * restores the opcode number.
 */
.macro REFRESH_INST _opnum
    movb    rINSTbl, rINSTbh
    movb    MACRO_LITERAL(\_opnum), rINSTbl
.endm

/*
 * Fetch the next instruction from rPC into rINSTw.  Does not advance rPC.
 */
.macro FETCH_INST
    movzwl  (rPC), rINST
.endm

/*
 * Remove opcode from rINST, compute the address of handler and jump to it.
 */
.macro GOTO_NEXT
    movzx   rINSTbl,%eax
    movzbl  rINSTbh,rINST
    shll    MACRO_LITERAL(7), %eax
    addl    rIBASE, %eax
    jmp     *%eax
.endm

/*
 * Advance rPC by instruction count.
 */
.macro ADVANCE_PC _count
    leal    2*\_count(rPC), rPC
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
#define VREG_HIGH_ADDRESS(_vreg) 4(rFP,_vreg,4)
#define VREG_REF_ADDRESS(_vreg) (rREFS,_vreg,4)
#define VREG_REF_HIGH_ADDRESS(_vreg) 4(rREFS,_vreg,4)

.macro GET_VREG _reg _vreg
    movl    (rFP,\_vreg,4), \_reg
.endm

/* Read wide value to xmm. */
.macro GET_WIDE_FP_VREG _reg _vreg
    movq    (rFP,\_vreg,4), \_reg
.endm

.macro SET_VREG _reg _vreg
    movl    \_reg, (rFP,\_vreg,4)
    movl    MACRO_LITERAL(0), (rREFS,\_vreg,4)
.endm

/* Write wide value from xmm. xmm is clobbered. */
.macro SET_WIDE_FP_VREG _reg _vreg
    movq    \_reg, (rFP,\_vreg,4)
    pxor    \_reg, \_reg
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

/* File: x86/entry.S */
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
    .cfi_def_cfa esp, 4

    /* Spill callee save regs */
    PUSH    %ebp
    PUSH    %edi
    PUSH    %esi
    PUSH    %ebx

    /* Allocate frame */
    subl    $FRAME_SIZE, %esp
    .cfi_adjust_cfa_offset FRAME_SIZE

    /* Load ShadowFrame pointer */
    movl    IN_ARG2(%esp), %edx

    /* Remember the return register */
    movl    IN_ARG3(%esp), %eax
    movl    %eax, SHADOWFRAME_RESULT_REGISTER_OFFSET(%edx)

    /* Remember the code_item */
    movl    IN_ARG1(%esp), %ecx
    movl    %ecx, SHADOWFRAME_CODE_ITEM_OFFSET(%edx)

    /* set up "named" registers */
    movl    SHADOWFRAME_NUMBER_OF_VREGS_OFFSET(%edx), %eax
    leal    SHADOWFRAME_VREGS_OFFSET(%edx), rFP
    leal    (rFP, %eax, 4), rREFS
    movl    SHADOWFRAME_DEX_PC_OFFSET(%edx), %eax
    lea     CODEITEM_INSNS_OFFSET(%ecx), rPC
    lea     (rPC, %eax, 2), rPC
    EXPORT_PC

    /* Set up for backwards branches & osr profiling */
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpSetUpHotnessCountdown)

    /* Starting ibase */
    REFRESH_IBASE

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
/* File: x86/op_nop.S */
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move: /* 0x01 */
/* File: x86/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- BA
    andb    $0xf, %al                      # eax <- A
    shrl    $4, rINST                      # rINST <- B
    GET_VREG rINST, rINST
    .if 0
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_from16: /* 0x02 */
/* File: x86/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    movzx   rINSTbl, %eax                   # eax <- AA
    movw    2(rPC), rINSTw                  # rINSTw <- BBBB
    GET_VREG rINST, rINST                   # rINST <- fp[BBBB]
    .if 0
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_move_16: /* 0x03 */
/* File: x86/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    movzwl  4(rPC), %ecx                    # ecx <- BBBB
    movzwl  2(rPC), %eax                    # eax <- AAAA
    GET_VREG rINST, %ecx
    .if 0
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_move_wide: /* 0x04 */
/* File: x86/op_move_wide.S */
    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, rINST                      # rINST <- B
    andb    $0xf, %cl                      # ecx <- A
    GET_WIDE_FP_VREG %xmm0, rINST           # xmm0 <- v[B]
    SET_WIDE_FP_VREG %xmm0, %ecx            # v[A] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_wide_from16: /* 0x05 */
/* File: x86/op_move_wide_from16.S */
    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwl  2(rPC), %ecx                    # ecx <- BBBB
    movzbl  rINSTbl, %eax                   # eax <- AAAA
    GET_WIDE_FP_VREG %xmm0, %ecx            # xmm0 <- v[B]
    SET_WIDE_FP_VREG %xmm0, %eax            # v[A] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_move_wide_16: /* 0x06 */
/* File: x86/op_move_wide_16.S */
    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwl  4(rPC), %ecx                    # ecx<- BBBB
    movzwl  2(rPC), %eax                    # eax<- AAAA
    GET_WIDE_FP_VREG %xmm0, %ecx            # xmm0 <- v[B]
    SET_WIDE_FP_VREG %xmm0, %eax            # v[A] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_move_object: /* 0x07 */
/* File: x86/op_move_object.S */
/* File: x86/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- BA
    andb    $0xf, %al                      # eax <- A
    shrl    $4, rINST                      # rINST <- B
    GET_VREG rINST, rINST
    .if 1
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_move_object_from16: /* 0x08 */
/* File: x86/op_move_object_from16.S */
/* File: x86/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    movzx   rINSTbl, %eax                   # eax <- AA
    movw    2(rPC), rINSTw                  # rINSTw <- BBBB
    GET_VREG rINST, rINST                   # rINST <- fp[BBBB]
    .if 1
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_move_object_16: /* 0x09 */
/* File: x86/op_move_object_16.S */
/* File: x86/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    movzwl  4(rPC), %ecx                    # ecx <- BBBB
    movzwl  2(rPC), %eax                    # eax <- AAAA
    GET_VREG rINST, %ecx
    .if 1
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3


/* ------------------------------ */
    .balign 128
.L_op_move_result: /* 0x0a */
/* File: x86/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    movl    OFF_FP_RESULT_REGISTER(rFP), %eax    # get pointer to result JType.
    movl    (%eax), %eax                    # r0 <- result.i.
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- fp[B]
    .else
    SET_VREG %eax, rINST                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_result_wide: /* 0x0b */
/* File: x86/op_move_result_wide.S */
    /* move-result-wide vAA */
    movl    OFF_FP_RESULT_REGISTER(rFP), %eax    # get pointer to result JType.
    movl    4(%eax), %ecx                   # Get high
    movl    (%eax), %eax                    # Get low
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    SET_VREG_HIGH %ecx, rINST               # v[AA+1] <- ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_move_result_object: /* 0x0c */
/* File: x86/op_move_result_object.S */
/* File: x86/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    movl    OFF_FP_RESULT_REGISTER(rFP), %eax    # get pointer to result JType.
    movl    (%eax), %eax                    # r0 <- result.i.
    .if 1
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- fp[B]
    .else
    SET_VREG %eax, rINST                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_move_exception: /* 0x0d */
/* File: x86/op_move_exception.S */
    /* move-exception vAA */
    movl    rSELF, %ecx
    movl    THREAD_EXCEPTION_OFFSET(%ecx), %eax
    SET_VREG_OBJECT %eax, rINST             # fp[AA] <- exception object
    movl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_return_void: /* 0x0e */
/* File: x86/op_return_void.S */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movl    rSELF, %eax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    SYMBOL(MterpSuspendCheck)
1:
    xorl    %eax, %eax
    xorl    %ecx, %ecx
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return: /* 0x0f */
/* File: x86/op_return.S */
/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movl    rSELF, %eax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINST                    # eax <- vAA
    xorl    %ecx, %ecx
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return_wide: /* 0x10 */
/* File: x86/op_return_wide.S */
/*
 * Return a 64-bit value.
 */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movl    rSELF, %eax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINST                    # eax <- v[AA+0]
    GET_VREG_HIGH %ecx, rINST               # ecx <- v[AA+1]
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return_object: /* 0x11 */
/* File: x86/op_return_object.S */
/* File: x86/op_return.S */
/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movl    rSELF, %eax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINST                    # eax <- vAA
    xorl    %ecx, %ecx
    jmp     MterpReturn


/* ------------------------------ */
    .balign 128
.L_op_const_4: /* 0x12 */
/* File: x86/op_const_4.S */
    /* const/4 vA, #+B */
    movsx   rINSTbl, %eax                   # eax <-ssssssBx
    movl    $0xf, rINST
    andl    %eax, rINST                     # rINST <- A
    sarl    $4, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_const_16: /* 0x13 */
/* File: x86/op_const_16.S */
    /* const/16 vAA, #+BBBB */
    movswl  2(rPC), %ecx                    # ecx <- ssssBBBB
    SET_VREG %ecx, rINST                    # vAA <- ssssBBBB
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const: /* 0x14 */
/* File: x86/op_const.S */
    /* const vAA, #+BBBBbbbb */
    movl    2(rPC), %eax                    # grab all 32 bits at once
    SET_VREG %eax, rINST                    # vAA<- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_const_high16: /* 0x15 */
/* File: x86/op_const_high16.S */
    /* const/high16 vAA, #+BBBB0000 */
    movzwl  2(rPC), %eax                    # eax <- 0000BBBB
    sall    $16, %eax                      # eax <- BBBB0000
    SET_VREG %eax, rINST                    # vAA <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_wide_16: /* 0x16 */
/* File: x86/op_const_wide_16.S */
    /* const-wide/16 vAA, #+BBBB */
    movswl  2(rPC), %eax                    # eax <- ssssBBBB
    movl    rIBASE, %ecx                    # preserve rIBASE (cltd trashes it)
    cltd                                    # rIBASE:eax <- ssssssssssssBBBB
    SET_VREG_HIGH rIBASE, rINST             # store msw
    SET_VREG %eax, rINST                    # store lsw
    movl    %ecx, rIBASE                    # restore rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_wide_32: /* 0x17 */
/* File: x86/op_const_wide_32.S */
    /* const-wide/32 vAA, #+BBBBbbbb */
    movl    2(rPC), %eax                    # eax <- BBBBbbbb
    movl    rIBASE, %ecx                    # preserve rIBASE (cltd trashes it)
    cltd                                    # rIBASE:eax <- ssssssssssssBBBB
    SET_VREG_HIGH rIBASE, rINST             # store msw
    SET_VREG %eax, rINST                    # store lsw
    movl    %ecx, rIBASE                    # restore rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_const_wide: /* 0x18 */
/* File: x86/op_const_wide.S */
    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    movl    2(rPC), %eax                    # eax <- lsw
    movzbl  rINSTbl, %ecx                   # ecx <- AA
    movl    6(rPC), rINST                   # rINST <- msw
    SET_VREG %eax, %ecx
    SET_VREG_HIGH  rINST, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 5

/* ------------------------------ */
    .balign 128
.L_op_const_wide_high16: /* 0x19 */
/* File: x86/op_const_wide_high16.S */
    /* const-wide/high16 vAA, #+BBBB000000000000 */
    movzwl  2(rPC), %eax                    # eax <- 0000BBBB
    sall    $16, %eax                      # eax <- BBBB0000
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    xorl    %eax, %eax
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_string: /* 0x1a */
/* File: x86/op_const_string.S */
    /* const/string vAA, String@BBBB */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- BBBB
    movl    %eax, OUT_ARG0(%esp)
    movl    rINST, OUT_ARG1(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG3(%esp)
    call    SYMBOL(MterpConstString)        # (index, tgt_reg, shadow_frame, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_const_string_jumbo: /* 0x1b */
/* File: x86/op_const_string_jumbo.S */
    /* const/string vAA, String@BBBBBBBB */
    EXPORT_PC
    movl    2(rPC), %eax                    # eax <- BBBB
    movl    %eax, OUT_ARG0(%esp)
    movl    rINST, OUT_ARG1(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG3(%esp)
    call    SYMBOL(MterpConstString)        # (index, tgt_reg, shadow_frame, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_const_class: /* 0x1c */
/* File: x86/op_const_class.S */
    /* const/class vAA, Class@BBBB */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- BBBB
    movl    %eax, OUT_ARG0(%esp)
    movl    rINST, OUT_ARG1(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG3(%esp)
    call    SYMBOL(MterpConstClass)         # (index, tgt_reg, shadow_frame, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_monitor_enter: /* 0x1d */
/* File: x86/op_monitor_enter.S */
/*
 * Synchronize on an object.
 */
    /* monitor-enter vAA */
    EXPORT_PC
    GET_VREG %ecx, rINST
    movl    %ecx, OUT_ARG0(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(artLockObjectFromCode)   # (object, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_monitor_exit: /* 0x1e */
/* File: x86/op_monitor_exit.S */
/*
 * Unlock an object.
 *
 * Exceptions that occur when unlocking a monitor need to appear as
 * if they happened at the following instruction.  See the Dalvik
 * instruction spec.
 */
    /* monitor-exit vAA */
    EXPORT_PC
    GET_VREG %ecx, rINST
    movl    %ecx, OUT_ARG0(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(artUnlockObjectFromCode) # (object, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_check_cast: /* 0x1f */
/* File: x86/op_check_cast.S */
/*
 * Check to see if a cast from one class to another is allowed.
 */
    /* check-cast vAA, class@BBBB */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- BBBB
    movl    %eax, OUT_ARG0(%esp)
    leal    VREG_ADDRESS(rINST), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    OFF_FP_METHOD(rFP),%eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)
    call    SYMBOL(MterpCheckCast)          # (index, &obj, method, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_instance_of: /* 0x20 */
/* File: x86/op_instance_of.S */
/*
 * Check to see if an object reference is an instance of a class.
 *
 * Most common situation is a non-null object, being compared against
 * an already-resolved class.
 */
    /* instance-of vA, vB, class@CCCC */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- BBBB
    movl    %eax, OUT_ARG0(%esp)
    movl    rINST, %eax                     # eax <- BA
    sarl    $4, %eax                       # eax <- B
    leal    VREG_ADDRESS(%eax), %ecx        # Get object address
    movl    %ecx, OUT_ARG1(%esp)
    movl    OFF_FP_METHOD(rFP),%eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)
    call    SYMBOL(MterpInstanceOf)         # (index, &obj, method, self)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    andb    $0xf, rINSTbl                  # rINSTbl <- A
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_array_length: /* 0x21 */
/* File: x86/op_array_length.S */
/*
 * Return the length of an array.
 */
    mov     rINST, %eax                     # eax <- BA
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %ecx, rINST                    # ecx <- vB (object ref)
    testl   %ecx, %ecx                      # is null?
    je      common_errNullObject
    andb    $0xf, %al                      # eax <- A
    movl    MIRROR_ARRAY_LENGTH_OFFSET(%ecx), rINST
    SET_VREG rINST, %eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_new_instance: /* 0x22 */
/* File: x86/op_new_instance.S */
/*
 * Create a new instance of a class.
 */
    /* new-instance vAA, class@BBBB */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG1(%esp)
    REFRESH_INST 34
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpNewInstance)
    RESTORE_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_new_array: /* 0x23 */
/* File: x86/op_new_array.S */
/*
 * Allocate an array of objects, specified with the array class
 * and a count.
 *
 * The verifier guarantees that this is an array class, so we don't
 * check for it here.
 */
    /* new-array vA, vB, class@CCCC */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST 35
    movl    rINST, OUT_ARG2(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)
    call    SYMBOL(MterpNewArray)
    RESTORE_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array: /* 0x24 */
/* File: x86/op_filled_new_array.S */
/*
 * Create a new array with elements filled from registers.
 *
 * for: filled-new-array, filled-new-array/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern MterpFilledNewArray
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)
    call    SYMBOL(MterpFilledNewArray)
    REFRESH_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array_range: /* 0x25 */
/* File: x86/op_filled_new_array_range.S */
/* File: x86/op_filled_new_array.S */
/*
 * Create a new array with elements filled from registers.
 *
 * for: filled-new-array, filled-new-array/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern MterpFilledNewArrayRange
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)
    call    SYMBOL(MterpFilledNewArrayRange)
    REFRESH_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3


/* ------------------------------ */
    .balign 128
.L_op_fill_array_data: /* 0x26 */
/* File: x86/op_fill_array_data.S */
    /* fill-array-data vAA, +BBBBBBBB */
    EXPORT_PC
    movl    2(rPC), %ecx                    # ecx <- BBBBbbbb
    leal    (rPC,%ecx,2), %ecx              # ecx <- PC + BBBBbbbb*2
    GET_VREG %eax, rINST                    # eax <- vAA (array object)
    movl    %eax, OUT_ARG0(%esp)
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpFillArrayData)      # (obj, payload)
    REFRESH_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3

/* ------------------------------ */
    .balign 128
.L_op_throw: /* 0x27 */
/* File: x86/op_throw.S */
/*
 * Throw an exception object in the current thread.
 */
    /* throw vAA */
    EXPORT_PC
    GET_VREG %eax, rINST                    # eax<- vAA (exception object)
    testl   %eax, %eax
    jz      common_errNullObject
    movl    rSELF,%ecx
    movl    %eax, THREAD_EXCEPTION_OFFSET(%ecx)
    jmp     MterpException

/* ------------------------------ */
    .balign 128
.L_op_goto: /* 0x28 */
/* File: x86/op_goto.S */
/*
 * Unconditional branch, 8-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto +AA */
    movsbl  rINSTbl, rINST                  # rINST <- ssssssAA
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_goto_16: /* 0x29 */
/* File: x86/op_goto_16.S */
/*
 * Unconditional branch, 16-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto/16 +AAAA */
    movswl  2(rPC), rINST                   # rINST <- ssssAAAA
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_goto_32: /* 0x2a */
/* File: x86/op_goto_32.S */
/*
 * Unconditional branch, 32-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 *
 * Unlike most opcodes, this one is allowed to branch to itself, so
 * our "backward branch" test must be "<=0" instead of "<0".  Because
 * we need the V bit set, we'll use an adds to convert from Dalvik
 * offset to byte offset.
 */
    /* goto/32 +AAAAAAAA */
    movl    2(rPC), rINST                   # rINST <- AAAAAAAA
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_packed_switch: /* 0x2b */
/* File: x86/op_packed_switch.S */
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
    movl    2(rPC), %ecx                    # ecx <- BBBBbbbb
    GET_VREG %eax, rINST                    # eax <- vAA
    leal    (rPC,%ecx,2), %ecx              # ecx <- PC + BBBBbbbb*2
    movl    %eax, OUT_ARG1(%esp)            # ARG1 <- vAA
    movl    %ecx, OUT_ARG0(%esp)            # ARG0 <- switchData
    call    SYMBOL(MterpDoPackedSwitch)
    REFRESH_IBASE
    testl   %eax, %eax
    movl    %eax, rINST
    jmp     MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_sparse_switch: /* 0x2c */
/* File: x86/op_sparse_switch.S */
/* File: x86/op_packed_switch.S */
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
    movl    2(rPC), %ecx                    # ecx <- BBBBbbbb
    GET_VREG %eax, rINST                    # eax <- vAA
    leal    (rPC,%ecx,2), %ecx              # ecx <- PC + BBBBbbbb*2
    movl    %eax, OUT_ARG1(%esp)            # ARG1 <- vAA
    movl    %ecx, OUT_ARG0(%esp)            # ARG0 <- switchData
    call    SYMBOL(MterpDoSparseSwitch)
    REFRESH_IBASE
    testl   %eax, %eax
    movl    %eax, rINST
    jmp     MterpCommonTakenBranch


/* ------------------------------ */
    .balign 128
.L_op_cmpl_float: /* 0x2d */
/* File: x86/op_cmpl_float.S */
/* File: x86/fpcmp.S */
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
    movzbl  3(rPC), %ecx                    # ecx<- CC
    movzbl  2(rPC), %eax                    # eax<- BB
    movss VREG_ADDRESS(%eax), %xmm0
    xor     %eax, %eax
    ucomiss VREG_ADDRESS(%ecx), %xmm0
    jp      .Lop_cmpl_float_nan_is_neg
    je      .Lop_cmpl_float_finish
    jb      .Lop_cmpl_float_less
.Lop_cmpl_float_nan_is_pos:
    incl    %eax
    jmp     .Lop_cmpl_float_finish
.Lop_cmpl_float_nan_is_neg:
.Lop_cmpl_float_less:
    decl    %eax
.Lop_cmpl_float_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmpg_float: /* 0x2e */
/* File: x86/op_cmpg_float.S */
/* File: x86/fpcmp.S */
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
    movzbl  3(rPC), %ecx                    # ecx<- CC
    movzbl  2(rPC), %eax                    # eax<- BB
    movss VREG_ADDRESS(%eax), %xmm0
    xor     %eax, %eax
    ucomiss VREG_ADDRESS(%ecx), %xmm0
    jp      .Lop_cmpg_float_nan_is_pos
    je      .Lop_cmpg_float_finish
    jb      .Lop_cmpg_float_less
.Lop_cmpg_float_nan_is_pos:
    incl    %eax
    jmp     .Lop_cmpg_float_finish
.Lop_cmpg_float_nan_is_neg:
.Lop_cmpg_float_less:
    decl    %eax
.Lop_cmpg_float_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmpl_double: /* 0x2f */
/* File: x86/op_cmpl_double.S */
/* File: x86/fpcmp.S */
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
    movzbl  3(rPC), %ecx                    # ecx<- CC
    movzbl  2(rPC), %eax                    # eax<- BB
    movsd VREG_ADDRESS(%eax), %xmm0
    xor     %eax, %eax
    ucomisd VREG_ADDRESS(%ecx), %xmm0
    jp      .Lop_cmpl_double_nan_is_neg
    je      .Lop_cmpl_double_finish
    jb      .Lop_cmpl_double_less
.Lop_cmpl_double_nan_is_pos:
    incl    %eax
    jmp     .Lop_cmpl_double_finish
.Lop_cmpl_double_nan_is_neg:
.Lop_cmpl_double_less:
    decl    %eax
.Lop_cmpl_double_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmpg_double: /* 0x30 */
/* File: x86/op_cmpg_double.S */
/* File: x86/fpcmp.S */
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
    movzbl  3(rPC), %ecx                    # ecx<- CC
    movzbl  2(rPC), %eax                    # eax<- BB
    movsd VREG_ADDRESS(%eax), %xmm0
    xor     %eax, %eax
    ucomisd VREG_ADDRESS(%ecx), %xmm0
    jp      .Lop_cmpg_double_nan_is_pos
    je      .Lop_cmpg_double_finish
    jb      .Lop_cmpg_double_less
.Lop_cmpg_double_nan_is_pos:
    incl    %eax
    jmp     .Lop_cmpg_double_finish
.Lop_cmpg_double_nan_is_neg:
.Lop_cmpg_double_less:
    decl    %eax
.Lop_cmpg_double_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_cmp_long: /* 0x31 */
/* File: x86/op_cmp_long.S */
/*
 * Compare two 64-bit values.  Puts 0, 1, or -1 into the destination
 * register based on the results of the comparison.
 */
    /* cmp-long vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1], BB is clobbered
    cmpl    VREG_HIGH_ADDRESS(%ecx), %eax
    jl      .Lop_cmp_long_smaller
    jg      .Lop_cmp_long_bigger
    movzbl  2(rPC), %eax                    # eax <- BB, restore BB
    GET_VREG %eax, %eax                     # eax <- v[BB]
    sub     VREG_ADDRESS(%ecx), %eax
    ja      .Lop_cmp_long_bigger
    jb      .Lop_cmp_long_smaller
.Lop_cmp_long_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.Lop_cmp_long_bigger:
    movl    $1, %eax
    jmp     .Lop_cmp_long_finish

.Lop_cmp_long_smaller:
    movl    $-1, %eax
    jmp     .Lop_cmp_long_finish

/* ------------------------------ */
    .balign 128
.L_op_if_eq: /* 0x32 */
/* File: x86/op_if_eq.S */
/* File: x86/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    jne   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_ne: /* 0x33 */
/* File: x86/op_if_ne.S */
/* File: x86/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    je   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_lt: /* 0x34 */
/* File: x86/op_if_lt.S */
/* File: x86/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    jge   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_ge: /* 0x35 */
/* File: x86/op_if_ge.S */
/* File: x86/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    jl   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_gt: /* 0x36 */
/* File: x86/op_if_gt.S */
/* File: x86/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    jle   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_le: /* 0x37 */
/* File: x86/op_if_le.S */
/* File: x86/bincmp.S */
/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    jg   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_eqz: /* 0x38 */
/* File: x86/op_if_eqz.S */
/* File: x86/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    jne   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_nez: /* 0x39 */
/* File: x86/op_if_nez.S */
/* File: x86/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    je   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_ltz: /* 0x3a */
/* File: x86/op_if_ltz.S */
/* File: x86/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    jge   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_gez: /* 0x3b */
/* File: x86/op_if_gez.S */
/* File: x86/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    jl   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_gtz: /* 0x3c */
/* File: x86/op_if_gtz.S */
/* File: x86/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    jle   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_if_lez: /* 0x3d */
/* File: x86/op_if_lez.S */
/* File: x86/zcmp.S */
/*
 * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
 */
    /* if-cmp vAA, +BBBB */
    cmpl    $0, VREG_ADDRESS(rINST)        # compare (vA, 0)
    jg   1f
    movswl  2(rPC), rINST                   # fetch signed displacement
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_unused_3e: /* 0x3e */
/* File: x86/op_unused_3e.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_3f: /* 0x3f */
/* File: x86/op_unused_3f.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_40: /* 0x40 */
/* File: x86/op_unused_40.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_41: /* 0x41 */
/* File: x86/op_unused_41.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_42: /* 0x42 */
/* File: x86/op_unused_42.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_43: /* 0x43 */
/* File: x86/op_unused_43.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_aget: /* 0x44 */
/* File: x86/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    movl   MIRROR_INT_ARRAY_DATA_OFFSET(%eax,%ecx,4), %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aget_wide: /* 0x45 */
/* File: x86/op_aget_wide.S */
/*
 * Array get, 64 bits.  vAA <- vBB[vCC].
 */
    /* aget-wide vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_WIDE_ARRAY_DATA_OFFSET(%eax,%ecx,8), %eax
    movq    (%eax), %xmm0                   # xmm0 <- vBB[vCC]
    SET_WIDE_FP_VREG %xmm0, rINST           # vAA <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aget_object: /* 0x46 */
/* File: x86/op_aget_object.S */
/*
 * Array object get.  vAA <- vBB[vCC].
 *
 * for: aget-object
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecs <- vCC (requested index)
    EXPORT_PC
    movl    %eax, OUT_ARG0(%esp)
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(artAGetObjectFromMterp)  # (array, index)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    SET_VREG_OBJECT %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aget_boolean: /* 0x47 */
/* File: x86/op_aget_boolean.S */
/* File: x86/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    movzbl   MIRROR_BOOLEAN_ARRAY_DATA_OFFSET(%eax,%ecx,1), %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_byte: /* 0x48 */
/* File: x86/op_aget_byte.S */
/* File: x86/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    movsbl   MIRROR_BYTE_ARRAY_DATA_OFFSET(%eax,%ecx,1), %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_char: /* 0x49 */
/* File: x86/op_aget_char.S */
/* File: x86/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    movzwl   MIRROR_CHAR_ARRAY_DATA_OFFSET(%eax,%ecx,2), %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aget_short: /* 0x4a */
/* File: x86/op_aget_short.S */
/* File: x86/op_aget.S */
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    movswl   MIRROR_SHORT_ARRAY_DATA_OFFSET(%eax,%ecx,2), %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput: /* 0x4b */
/* File: x86/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_INT_ARRAY_DATA_OFFSET(%eax,%ecx,4), %eax
    GET_VREG rINST, rINST
    movl  rINST, (%eax)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aput_wide: /* 0x4c */
/* File: x86/op_aput_wide.S */
/*
 * Array put, 64 bits.  vBB[vCC] <- vAA.
 *
 */
    /* aput-wide vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_WIDE_ARRAY_DATA_OFFSET(%eax,%ecx,8), %eax
    GET_WIDE_FP_VREG %xmm0, rINST           # xmm0 <- vAA
    movq    %xmm0, (%eax)                   # vBB[vCC] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aput_object: /* 0x4d */
/* File: x86/op_aput_object.S */
/*
 * Store an object into an array.  vBB[vCC] <- vAA.
 */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST 77
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpAputObject)         # (array, index)
    RESTORE_IBASE
    testb   %al, %al
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_aput_boolean: /* 0x4e */
/* File: x86/op_aput_boolean.S */
/* File: x86/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_BOOLEAN_ARRAY_DATA_OFFSET(%eax,%ecx,1), %eax
    GET_VREG rINST, rINST
    movb  rINSTbl, (%eax)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_byte: /* 0x4f */
/* File: x86/op_aput_byte.S */
/* File: x86/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_BYTE_ARRAY_DATA_OFFSET(%eax,%ecx,1), %eax
    GET_VREG rINST, rINST
    movb  rINSTbl, (%eax)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_char: /* 0x50 */
/* File: x86/op_aput_char.S */
/* File: x86/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_CHAR_ARRAY_DATA_OFFSET(%eax,%ecx,2), %eax
    GET_VREG rINST, rINST
    movw  rINSTw, (%eax)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_aput_short: /* 0x51 */
/* File: x86/op_aput_short.S */
/* File: x86/op_aput.S */
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_SHORT_ARRAY_DATA_OFFSET(%eax,%ecx,2), %eax
    GET_VREG rINST, rINST
    movw  rINSTw, (%eax)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget: /* 0x52 */
/* File: x86/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGet32InstanceFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iget_wide: /* 0x53 */
/* File: x86/op_iget_wide.S */
/*
 * 64-bit instance field get.
 *
 * for: iget-wide
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGet64InstanceFromCode)
    mov     rSELF, %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    SET_VREG %eax, rINST
    SET_VREG_HIGH %edx, rINST
    RESTORE_IBASE_FROM_SELF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iget_object: /* 0x54 */
/* File: x86/op_iget_object.S */
/* File: x86/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGetObjInstanceFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 1
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean: /* 0x55 */
/* File: x86/op_iget_boolean.S */
/* File: x86/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGetBooleanInstanceFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_byte: /* 0x56 */
/* File: x86/op_iget_byte.S */
/* File: x86/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGetByteInstanceFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_char: /* 0x57 */
/* File: x86/op_iget_char.S */
/* File: x86/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGetCharInstanceFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_short: /* 0x58 */
/* File: x86/op_iget_short.S */
/* File: x86/op_iget.S */
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artGetShortInstanceFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf, rINSTbl                  # rINST <- A
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput: /* 0x59 */
/* File: x86/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet32InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG %eax, rINST
    movl    %eax, OUT_ARG2(%esp)            # fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL(artSet32InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_wide: /* 0x5a */
/* File: x86/op_iput_wide.S */
    /* iput-wide vA, vB, field@CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl,%ecx                    # ecx <- BA
    sarl    $4,%ecx                        # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $0xf,rINSTbl                   # rINST <- A
    leal    VREG_ADDRESS(rINST), %eax
    movl    %eax, OUT_ARG2(%esp)            # &fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL(artSet64InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_object: /* 0x5b */
/* File: x86/op_iput_object.S */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST 91
    movl    rINST, OUT_ARG2(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG3(%esp)
    call    SYMBOL(MterpIputObject)
    testb   %al, %al
    jz      MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_boolean: /* 0x5c */
/* File: x86/op_iput_boolean.S */
/* File: x86/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG %eax, rINST
    movl    %eax, OUT_ARG2(%esp)            # fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL(artSet8InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_byte: /* 0x5d */
/* File: x86/op_iput_byte.S */
/* File: x86/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG %eax, rINST
    movl    %eax, OUT_ARG2(%esp)            # fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL(artSet8InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_char: /* 0x5e */
/* File: x86/op_iput_char.S */
/* File: x86/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG %eax, rINST
    movl    %eax, OUT_ARG2(%esp)            # fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL(artSet16InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_short: /* 0x5f */
/* File: x86/op_iput_short.S */
/* File: x86/op_iput.S */
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $0xf, rINSTbl                  # rINST<- A
    GET_VREG %eax, rINST
    movl    %eax, OUT_ARG2(%esp)            # fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL(artSet16InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget: /* 0x60 */
/* File: x86/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern artGet32StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGet32StaticFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sget_wide: /* 0x61 */
/* File: x86/op_sget_wide.S */
/*
 * SGET_WIDE handler wrapper.
 *
 */
    /* sget-wide vAA, field@BBBB */
    .extern artGet64StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGet64StaticFromCode)
    movl    rSELF, %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    SET_VREG %eax, rINST                    # fp[A]<- low part
    SET_VREG_HIGH %edx, rINST               # fp[A+1]<- high part
    RESTORE_IBASE_FROM_SELF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sget_object: /* 0x62 */
/* File: x86/op_sget_object.S */
/* File: x86/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern artGetObjStaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGetObjStaticFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if 1
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_boolean: /* 0x63 */
/* File: x86/op_sget_boolean.S */
/* File: x86/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern artGetBooleanStaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGetBooleanStaticFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_byte: /* 0x64 */
/* File: x86/op_sget_byte.S */
/* File: x86/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern artGetByteStaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGetByteStaticFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_char: /* 0x65 */
/* File: x86/op_sget_char.S */
/* File: x86/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern artGetCharStaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGetCharStaticFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sget_short: /* 0x66 */
/* File: x86/op_sget_short.S */
/* File: x86/op_sget.S */
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern artGetShortStaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGetShortStaticFromCode)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if 0
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput: /* 0x67 */
/* File: x86/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet32StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    GET_VREG rINST, rINST
    movl    rINST, OUT_ARG1(%esp)           # fp[AA]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artSet32StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sput_wide: /* 0x68 */
/* File: x86/op_sput_wide.S */
/*
 * SPUT_WIDE handler wrapper.
 *
 */
    /* sput-wide vAA, field@BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    leal    VREG_ADDRESS(rINST), %eax
    movl    %eax, OUT_ARG2(%esp)            # &fp[AA]
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artSet64IndirectStaticFromMterp)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sput_object: /* 0x69 */
/* File: x86/op_sput_object.S */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST 105
    movl    rINST, OUT_ARG2(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)
    call    SYMBOL(MterpSputObject)
    testb   %al, %al
    jz      MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_sput_boolean: /* 0x6a */
/* File: x86/op_sput_boolean.S */
/* File: x86/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet8StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    GET_VREG rINST, rINST
    movl    rINST, OUT_ARG1(%esp)           # fp[AA]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artSet8StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput_byte: /* 0x6b */
/* File: x86/op_sput_byte.S */
/* File: x86/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet8StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    GET_VREG rINST, rINST
    movl    rINST, OUT_ARG1(%esp)           # fp[AA]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artSet8StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput_char: /* 0x6c */
/* File: x86/op_sput_char.S */
/* File: x86/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet16StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    GET_VREG rINST, rINST
    movl    rINST, OUT_ARG1(%esp)           # fp[AA]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artSet16StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sput_short: /* 0x6d */
/* File: x86/op_sput_short.S */
/* File: x86/op_sput.S */
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern artSet16StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    GET_VREG rINST, rINST
    movl    rINST, OUT_ARG1(%esp)           # fp[AA]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL(artSet16StaticFromCode)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual: /* 0x6e */
/* File: x86/op_invoke_virtual.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtual
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 110
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeVirtual)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
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
/* File: x86/op_invoke_super.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuper
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 111
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeSuper)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
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
/* File: x86/op_invoke_direct.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirect
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 112
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeDirect)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_static: /* 0x71 */
/* File: x86/op_invoke_static.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStatic
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 113
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeStatic)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT



/* ------------------------------ */
    .balign 128
.L_op_invoke_interface: /* 0x72 */
/* File: x86/op_invoke_interface.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterface
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 114
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeInterface)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
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
/* File: x86/op_return_void_no_barrier.S */
    movl    rSELF, %eax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    SYMBOL(MterpSuspendCheck)
1:
    xorl    %eax, %eax
    xorl    %ecx, %ecx
    jmp     MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range: /* 0x74 */
/* File: x86/op_invoke_virtual_range.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualRange
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 116
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeVirtualRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_super_range: /* 0x75 */
/* File: x86/op_invoke_super_range.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuperRange
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 117
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeSuperRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_direct_range: /* 0x76 */
/* File: x86/op_invoke_direct_range.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirectRange
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 118
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeDirectRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_static_range: /* 0x77 */
/* File: x86/op_invoke_static_range.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStaticRange
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 119
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeStaticRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_interface_range: /* 0x78 */
/* File: x86/op_invoke_interface_range.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterfaceRange
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 120
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeInterfaceRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_unused_79: /* 0x79 */
/* File: x86/op_unused_79.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_7a: /* 0x7a */
/* File: x86/op_unused_7a.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_neg_int: /* 0x7b */
/* File: x86/op_neg_int.S */
/* File: x86/unop.S */
/*
 * Generic 32-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movzbl  rINSTbl,%ecx                    # ecx <- A+
    sarl    $4,rINST                       # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf,%cl                       # ecx <- A
    negl    %eax
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_not_int: /* 0x7c */
/* File: x86/op_not_int.S */
/* File: x86/unop.S */
/*
 * Generic 32-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movzbl  rINSTbl,%ecx                    # ecx <- A+
    sarl    $4,rINST                       # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf,%cl                       # ecx <- A
    notl %eax
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_neg_long: /* 0x7d */
/* File: x86/op_neg_long.S */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, %ecx                     # eax <- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # ecx <- v[B+1]
    negl    %eax
    adcl    $0, %ecx
    negl    %ecx
    SET_VREG %eax, rINST                    # v[A+0] <- eax
    SET_VREG_HIGH %ecx, rINST               # v[A+1] <- ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_not_long: /* 0x7e */
/* File: x86/op_not_long.S */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, %ecx                     # eax <- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # ecx <- v[B+1]
    notl    %eax
    notl    %ecx
    SET_VREG %eax, rINST                    # v[A+0] <- eax
    SET_VREG_HIGH %ecx, rINST               # v[A+1] <- ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_neg_float: /* 0x7f */
/* File: x86/op_neg_float.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    flds   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    fchs
    fstps  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 0
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_neg_double: /* 0x80 */
/* File: x86/op_neg_double.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fldl   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    fchs
    fstpl  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 1
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_long: /* 0x81 */
/* File: x86/op_int_to_long.S */
    /* int to long vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- +A
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    andb    $0xf, rINSTbl                  # rINST <- A
    movl    rIBASE, %ecx                    # cltd trashes rIBASE/edx
    cltd                                    # rINST:eax<- sssssssBBBBBBBB
    SET_VREG_HIGH rIBASE, rINST             # v[A+1] <- rIBASE
    SET_VREG %eax, rINST                    # v[A+0] <- %eax
    movl    %ecx, rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_float: /* 0x82 */
/* File: x86/op_int_to_float.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fildl   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    
    fstps  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 0
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_double: /* 0x83 */
/* File: x86/op_int_to_double.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fildl   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    
    fstpl  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 1
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_long_to_int: /* 0x84 */
/* File: x86/op_long_to_int.S */
/* we ignore the high word, making this equivalent to a 32-bit reg move */
/* File: x86/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- BA
    andb    $0xf, %al                      # eax <- A
    shrl    $4, rINST                      # rINST <- B
    GET_VREG rINST, rINST
    .if 0
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_long_to_float: /* 0x85 */
/* File: x86/op_long_to_float.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fildll   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    
    fstps  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 0
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_long_to_double: /* 0x86 */
/* File: x86/op_long_to_double.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fildll   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    
    fstpl  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 1
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_float_to_int: /* 0x87 */
/* File: x86/op_float_to_int.S */
/* File: x86/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.  This model
 * differs from what is delivered normally via the x86 fpu, so we have
 * to play some games.
 */
    /* float/double to int/long vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    .if 0
    fldl    VREG_ADDRESS(rINST)             # %st0 <- vB
    .else
    flds    VREG_ADDRESS(rINST)             # %st0 <- vB
    .endif
    ftst
    fnstcw  LOCAL0(%esp)                    # remember original rounding mode
    movzwl  LOCAL0(%esp), %eax
    movb    $0xc, %ah
    movw    %ax, LOCAL0+2(%esp)
    fldcw   LOCAL0+2(%esp)                  # set "to zero" rounding mode
    andb    $0xf, %cl                      # ecx <- A
    .if 0
    fistpll VREG_ADDRESS(%ecx)              # convert and store
    .else
    fistpl  VREG_ADDRESS(%ecx)              # convert and store
    .endif
    fldcw   LOCAL0(%esp)                    # restore previous rounding mode
    .if 0
    movl    $0x80000000, %eax
    xorl    VREG_HIGH_ADDRESS(%ecx), %eax
    orl     VREG_ADDRESS(%ecx), %eax
    .else
    cmpl    $0x80000000, VREG_ADDRESS(%ecx)
    .endif
    je      .Lop_float_to_int_special_case # fix up result

.Lop_float_to_int_finish:
    xor     %eax, %eax
    mov     %eax, VREG_REF_ADDRESS(%ecx)
    .if 0
    mov     %eax, VREG_REF_HIGH_ADDRESS(%ecx)
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.Lop_float_to_int_special_case:
    fnstsw  %ax
    sahf
    jp      .Lop_float_to_int_isNaN
    adcl    $-1, VREG_ADDRESS(%ecx)
    .if 0
    adcl    $-1, VREG_HIGH_ADDRESS(%ecx)
    .endif
   jmp      .Lop_float_to_int_finish
.Lop_float_to_int_isNaN:
    movl    $0, VREG_ADDRESS(%ecx)
    .if 0
    movl    $0, VREG_HIGH_ADDRESS(%ecx)
    .endif
    jmp     .Lop_float_to_int_finish


/* ------------------------------ */
    .balign 128
.L_op_float_to_long: /* 0x88 */
/* File: x86/op_float_to_long.S */
/* File: x86/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.  This model
 * differs from what is delivered normally via the x86 fpu, so we have
 * to play some games.
 */
    /* float/double to int/long vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    .if 0
    fldl    VREG_ADDRESS(rINST)             # %st0 <- vB
    .else
    flds    VREG_ADDRESS(rINST)             # %st0 <- vB
    .endif
    ftst
    fnstcw  LOCAL0(%esp)                    # remember original rounding mode
    movzwl  LOCAL0(%esp), %eax
    movb    $0xc, %ah
    movw    %ax, LOCAL0+2(%esp)
    fldcw   LOCAL0+2(%esp)                  # set "to zero" rounding mode
    andb    $0xf, %cl                      # ecx <- A
    .if 1
    fistpll VREG_ADDRESS(%ecx)              # convert and store
    .else
    fistpl  VREG_ADDRESS(%ecx)              # convert and store
    .endif
    fldcw   LOCAL0(%esp)                    # restore previous rounding mode
    .if 1
    movl    $0x80000000, %eax
    xorl    VREG_HIGH_ADDRESS(%ecx), %eax
    orl     VREG_ADDRESS(%ecx), %eax
    .else
    cmpl    $0x80000000, VREG_ADDRESS(%ecx)
    .endif
    je      .Lop_float_to_long_special_case # fix up result

.Lop_float_to_long_finish:
    xor     %eax, %eax
    mov     %eax, VREG_REF_ADDRESS(%ecx)
    .if 1
    mov     %eax, VREG_REF_HIGH_ADDRESS(%ecx)
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.Lop_float_to_long_special_case:
    fnstsw  %ax
    sahf
    jp      .Lop_float_to_long_isNaN
    adcl    $-1, VREG_ADDRESS(%ecx)
    .if 1
    adcl    $-1, VREG_HIGH_ADDRESS(%ecx)
    .endif
   jmp      .Lop_float_to_long_finish
.Lop_float_to_long_isNaN:
    movl    $0, VREG_ADDRESS(%ecx)
    .if 1
    movl    $0, VREG_HIGH_ADDRESS(%ecx)
    .endif
    jmp     .Lop_float_to_long_finish


/* ------------------------------ */
    .balign 128
.L_op_float_to_double: /* 0x89 */
/* File: x86/op_float_to_double.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    flds   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    
    fstpl  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 1
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_double_to_int: /* 0x8a */
/* File: x86/op_double_to_int.S */
/* File: x86/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.  This model
 * differs from what is delivered normally via the x86 fpu, so we have
 * to play some games.
 */
    /* float/double to int/long vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    .if 1
    fldl    VREG_ADDRESS(rINST)             # %st0 <- vB
    .else
    flds    VREG_ADDRESS(rINST)             # %st0 <- vB
    .endif
    ftst
    fnstcw  LOCAL0(%esp)                    # remember original rounding mode
    movzwl  LOCAL0(%esp), %eax
    movb    $0xc, %ah
    movw    %ax, LOCAL0+2(%esp)
    fldcw   LOCAL0+2(%esp)                  # set "to zero" rounding mode
    andb    $0xf, %cl                      # ecx <- A
    .if 0
    fistpll VREG_ADDRESS(%ecx)              # convert and store
    .else
    fistpl  VREG_ADDRESS(%ecx)              # convert and store
    .endif
    fldcw   LOCAL0(%esp)                    # restore previous rounding mode
    .if 0
    movl    $0x80000000, %eax
    xorl    VREG_HIGH_ADDRESS(%ecx), %eax
    orl     VREG_ADDRESS(%ecx), %eax
    .else
    cmpl    $0x80000000, VREG_ADDRESS(%ecx)
    .endif
    je      .Lop_double_to_int_special_case # fix up result

.Lop_double_to_int_finish:
    xor     %eax, %eax
    mov     %eax, VREG_REF_ADDRESS(%ecx)
    .if 0
    mov     %eax, VREG_REF_HIGH_ADDRESS(%ecx)
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.Lop_double_to_int_special_case:
    fnstsw  %ax
    sahf
    jp      .Lop_double_to_int_isNaN
    adcl    $-1, VREG_ADDRESS(%ecx)
    .if 0
    adcl    $-1, VREG_HIGH_ADDRESS(%ecx)
    .endif
   jmp      .Lop_double_to_int_finish
.Lop_double_to_int_isNaN:
    movl    $0, VREG_ADDRESS(%ecx)
    .if 0
    movl    $0, VREG_HIGH_ADDRESS(%ecx)
    .endif
    jmp     .Lop_double_to_int_finish


/* ------------------------------ */
    .balign 128
.L_op_double_to_long: /* 0x8b */
/* File: x86/op_double_to_long.S */
/* File: x86/cvtfp_int.S */
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.  This model
 * differs from what is delivered normally via the x86 fpu, so we have
 * to play some games.
 */
    /* float/double to int/long vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    .if 1
    fldl    VREG_ADDRESS(rINST)             # %st0 <- vB
    .else
    flds    VREG_ADDRESS(rINST)             # %st0 <- vB
    .endif
    ftst
    fnstcw  LOCAL0(%esp)                    # remember original rounding mode
    movzwl  LOCAL0(%esp), %eax
    movb    $0xc, %ah
    movw    %ax, LOCAL0+2(%esp)
    fldcw   LOCAL0+2(%esp)                  # set "to zero" rounding mode
    andb    $0xf, %cl                      # ecx <- A
    .if 1
    fistpll VREG_ADDRESS(%ecx)              # convert and store
    .else
    fistpl  VREG_ADDRESS(%ecx)              # convert and store
    .endif
    fldcw   LOCAL0(%esp)                    # restore previous rounding mode
    .if 1
    movl    $0x80000000, %eax
    xorl    VREG_HIGH_ADDRESS(%ecx), %eax
    orl     VREG_ADDRESS(%ecx), %eax
    .else
    cmpl    $0x80000000, VREG_ADDRESS(%ecx)
    .endif
    je      .Lop_double_to_long_special_case # fix up result

.Lop_double_to_long_finish:
    xor     %eax, %eax
    mov     %eax, VREG_REF_ADDRESS(%ecx)
    .if 1
    mov     %eax, VREG_REF_HIGH_ADDRESS(%ecx)
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.Lop_double_to_long_special_case:
    fnstsw  %ax
    sahf
    jp      .Lop_double_to_long_isNaN
    adcl    $-1, VREG_ADDRESS(%ecx)
    .if 1
    adcl    $-1, VREG_HIGH_ADDRESS(%ecx)
    .endif
   jmp      .Lop_double_to_long_finish
.Lop_double_to_long_isNaN:
    movl    $0, VREG_ADDRESS(%ecx)
    .if 1
    movl    $0, VREG_HIGH_ADDRESS(%ecx)
    .endif
    jmp     .Lop_double_to_long_finish


/* ------------------------------ */
    .balign 128
.L_op_double_to_float: /* 0x8c */
/* File: x86/op_double_to_float.S */
/* File: x86/fpcvt.S */
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fldl   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $0xf, %cl                      # ecx <- A
    
    fstps  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if 0
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_byte: /* 0x8d */
/* File: x86/op_int_to_byte.S */
/* File: x86/unop.S */
/*
 * Generic 32-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movzbl  rINSTbl,%ecx                    # ecx <- A+
    sarl    $4,rINST                       # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf,%cl                       # ecx <- A
    movsbl  %al, %eax
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_char: /* 0x8e */
/* File: x86/op_int_to_char.S */
/* File: x86/unop.S */
/*
 * Generic 32-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movzbl  rINSTbl,%ecx                    # ecx <- A+
    sarl    $4,rINST                       # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf,%cl                       # ecx <- A
    movzwl  %ax,%eax
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_int_to_short: /* 0x8f */
/* File: x86/op_int_to_short.S */
/* File: x86/unop.S */
/*
 * Generic 32-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movzbl  rINSTbl,%ecx                    # ecx <- A+
    sarl    $4,rINST                       # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf,%cl                       # ecx <- A
    movswl %ax, %eax
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_add_int: /* 0x90 */
/* File: x86/op_add_int.S */
/* File: x86/binop.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    addl    (rFP,%ecx,4), %eax                                  # ex: addl    (rFP,%ecx,4),%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_int: /* 0x91 */
/* File: x86/op_sub_int.S */
/* File: x86/binop.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    subl    (rFP,%ecx,4), %eax                                  # ex: addl    (rFP,%ecx,4),%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_int: /* 0x92 */
/* File: x86/op_mul_int.S */
    /*
     * 32-bit binary multiplication.
     */
    /* mul vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    mov     rIBASE, LOCAL0(%esp)
    imull   (rFP,%ecx,4), %eax              # trashes rIBASE/edx
    mov     LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_div_int: /* 0x93 */
/* File: x86/op_div_int.S */
/* File: x86/bindiv.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    mov     rIBASE, LOCAL0(%esp)
    testl   %ecx, %ecx
    je      common_errDivideByZero
    movl    %eax, %edx
    orl     %ecx, %edx
    testl   $0xFFFFFF00, %edx              # If both arguments are less
                                            #   than 8-bit and +ve
    jz      .Lop_div_int_8                   # Do 8-bit divide
    testl   $0xFFFF0000, %edx              # If both arguments are less
                                            #   than 16-bit and +ve
    jz      .Lop_div_int_16                  # Do 16-bit divide
    cmpl    $-1, %ecx
    jne     .Lop_div_int_32
    cmpl    $0x80000000, %eax
    jne     .Lop_div_int_32
    movl    $0x80000000, %eax
    jmp     .Lop_div_int_finish
.Lop_div_int_32:
    cltd
    idivl   %ecx
    jmp     .Lop_div_int_finish
.Lop_div_int_8:
    div     %cl                             # 8-bit divide otherwise.
                                            # Remainder in %ah, quotient in %al
    .if 0
    movl    %eax, %edx
    shr     $8, %edx
    .else
    andl    $0x000000FF, %eax
    .endif
    jmp     .Lop_div_int_finish
.Lop_div_int_16:
    xorl    %edx, %edx                      # Clear %edx before divide
    div     %cx
.Lop_div_int_finish:
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_int: /* 0x94 */
/* File: x86/op_rem_int.S */
/* File: x86/bindiv.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    mov     rIBASE, LOCAL0(%esp)
    testl   %ecx, %ecx
    je      common_errDivideByZero
    movl    %eax, %edx
    orl     %ecx, %edx
    testl   $0xFFFFFF00, %edx              # If both arguments are less
                                            #   than 8-bit and +ve
    jz      .Lop_rem_int_8                   # Do 8-bit divide
    testl   $0xFFFF0000, %edx              # If both arguments are less
                                            #   than 16-bit and +ve
    jz      .Lop_rem_int_16                  # Do 16-bit divide
    cmpl    $-1, %ecx
    jne     .Lop_rem_int_32
    cmpl    $0x80000000, %eax
    jne     .Lop_rem_int_32
    movl    $0, rIBASE
    jmp     .Lop_rem_int_finish
.Lop_rem_int_32:
    cltd
    idivl   %ecx
    jmp     .Lop_rem_int_finish
.Lop_rem_int_8:
    div     %cl                             # 8-bit divide otherwise.
                                            # Remainder in %ah, quotient in %al
    .if 1
    movl    %eax, %edx
    shr     $8, %edx
    .else
    andl    $0x000000FF, %eax
    .endif
    jmp     .Lop_rem_int_finish
.Lop_rem_int_16:
    xorl    %edx, %edx                      # Clear %edx before divide
    div     %cx
.Lop_rem_int_finish:
    SET_VREG rIBASE, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_and_int: /* 0x95 */
/* File: x86/op_and_int.S */
/* File: x86/binop.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    andl    (rFP,%ecx,4), %eax                                  # ex: addl    (rFP,%ecx,4),%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_int: /* 0x96 */
/* File: x86/op_or_int.S */
/* File: x86/binop.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    orl     (rFP,%ecx,4), %eax                                  # ex: addl    (rFP,%ecx,4),%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_int: /* 0x97 */
/* File: x86/op_xor_int.S */
/* File: x86/binop.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    xorl    (rFP,%ecx,4), %eax                                  # ex: addl    (rFP,%ecx,4),%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shl_int: /* 0x98 */
/* File: x86/op_shl_int.S */
/* File: x86/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC),%eax                     # eax <- BB
    movzbl  3(rPC),%ecx                     # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # eax <- vBB
    sall    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shr_int: /* 0x99 */
/* File: x86/op_shr_int.S */
/* File: x86/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC),%eax                     # eax <- BB
    movzbl  3(rPC),%ecx                     # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # eax <- vBB
    sarl    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_ushr_int: /* 0x9a */
/* File: x86/op_ushr_int.S */
/* File: x86/binop1.S */
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC),%eax                     # eax <- BB
    movzbl  3(rPC),%ecx                     # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # eax <- vBB
    shrl    %cl, %eax                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_add_long: /* 0x9b */
/* File: x86/op_add_long.S */
/* File: x86/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)            # save rIBASE
    GET_VREG rIBASE, %eax                   # rIBASE <- v[BB+0]
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1]
    addl    (rFP,%ecx,4), rIBASE                                 # ex: addl   (rFP,%ecx,4),rIBASE
    adcl    4(rFP,%ecx,4), %eax                                 # ex: adcl   4(rFP,%ecx,4),%eax
    SET_VREG rIBASE, rINST                  # v[AA+0] <- rIBASE
    movl    LOCAL0(%esp), rIBASE            # restore rIBASE
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_long: /* 0x9c */
/* File: x86/op_sub_long.S */
/* File: x86/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)            # save rIBASE
    GET_VREG rIBASE, %eax                   # rIBASE <- v[BB+0]
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1]
    subl    (rFP,%ecx,4), rIBASE                                 # ex: addl   (rFP,%ecx,4),rIBASE
    sbbl    4(rFP,%ecx,4), %eax                                 # ex: adcl   4(rFP,%ecx,4),%eax
    SET_VREG rIBASE, rINST                  # v[AA+0] <- rIBASE
    movl    LOCAL0(%esp), rIBASE            # restore rIBASE
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_long: /* 0x9d */
/* File: x86/op_mul_long.S */
/*
 * Signed 64-bit integer multiply.
 *
 * We could definately use more free registers for
 * this code.   We spill rINSTw (ebx),
 * giving us eax, ebc, ecx and edx as computational
 * temps.  On top of that, we'll spill edi (rFP)
 * for use as the vB pointer and esi (rPC) for use
 * as the vC pointer.  Yuck.
 *
 */
    /* mul-long vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- B
    movzbl  3(rPC), %ecx                    # ecx <- C
    mov     rPC, LOCAL0(%esp)               # save Interpreter PC
    mov     rFP, LOCAL1(%esp)               # save FP
    mov     rIBASE, LOCAL2(%esp)            # save rIBASE
    leal    (rFP,%eax,4), %esi              # esi <- &v[B]
    leal    (rFP,%ecx,4), rFP               # rFP <- &v[C]
    movl    4(%esi), %ecx                   # ecx <- Bmsw
    imull   (rFP), %ecx                     # ecx <- (Bmsw*Clsw)
    movl    4(rFP), %eax                    # eax <- Cmsw
    imull   (%esi), %eax                    # eax <- (Cmsw*Blsw)
    addl    %eax, %ecx                      # ecx <- (Bmsw*Clsw)+(Cmsw*Blsw)
    movl    (rFP), %eax                     # eax <- Clsw
    mull    (%esi)                          # eax <- (Clsw*Alsw)
    mov     LOCAL0(%esp), rPC               # restore Interpreter PC
    mov     LOCAL1(%esp), rFP               # restore FP
    leal    (%ecx,rIBASE), rIBASE           # full result now in rIBASE:%eax
    SET_VREG_HIGH rIBASE, rINST             # v[B+1] <- rIBASE
    mov     LOCAL2(%esp), rIBASE            # restore IBASE
    SET_VREG %eax, rINST                    # v[B] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_div_long: /* 0x9e */
/* File: x86/op_div_long.S */
/* art_quick_* methods has quick abi,
 *   so use eax, ecx, edx, ebx for args
 */
    /* div vAA, vBB, vCC */
    .extern art_quick_ldiv
    mov     rIBASE, LOCAL0(%esp)            # save rIBASE/%edx
    mov     rINST, LOCAL1(%esp)             # save rINST/%ebx
    movzbl  3(rPC), %eax                    # eax <- CC
    GET_VREG %ecx, %eax
    GET_VREG_HIGH %ebx, %eax
    movl    %ecx, %edx
    orl     %ebx, %ecx
    jz      common_errDivideByZero
    movzbl  2(rPC), %eax                    # eax <- BB
    GET_VREG_HIGH %ecx, %eax
    GET_VREG %eax, %eax
    call    SYMBOL(art_quick_ldiv)
    mov     LOCAL1(%esp), rINST             # restore rINST/%ebx
    SET_VREG_HIGH rIBASE, rINST
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE            # restore rIBASE/%edx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_rem_long: /* 0x9f */
/* File: x86/op_rem_long.S */
/* File: x86/op_div_long.S */
/* art_quick_* methods has quick abi,
 *   so use eax, ecx, edx, ebx for args
 */
    /* div vAA, vBB, vCC */
    .extern art_quick_lmod
    mov     rIBASE, LOCAL0(%esp)            # save rIBASE/%edx
    mov     rINST, LOCAL1(%esp)             # save rINST/%ebx
    movzbl  3(rPC), %eax                    # eax <- CC
    GET_VREG %ecx, %eax
    GET_VREG_HIGH %ebx, %eax
    movl    %ecx, %edx
    orl     %ebx, %ecx
    jz      common_errDivideByZero
    movzbl  2(rPC), %eax                    # eax <- BB
    GET_VREG_HIGH %ecx, %eax
    GET_VREG %eax, %eax
    call    SYMBOL(art_quick_lmod)
    mov     LOCAL1(%esp), rINST             # restore rINST/%ebx
    SET_VREG_HIGH rIBASE, rINST
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE            # restore rIBASE/%edx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_and_long: /* 0xa0 */
/* File: x86/op_and_long.S */
/* File: x86/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)            # save rIBASE
    GET_VREG rIBASE, %eax                   # rIBASE <- v[BB+0]
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1]
    andl    (rFP,%ecx,4), rIBASE                                 # ex: addl   (rFP,%ecx,4),rIBASE
    andl    4(rFP,%ecx,4), %eax                                 # ex: adcl   4(rFP,%ecx,4),%eax
    SET_VREG rIBASE, rINST                  # v[AA+0] <- rIBASE
    movl    LOCAL0(%esp), rIBASE            # restore rIBASE
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_long: /* 0xa1 */
/* File: x86/op_or_long.S */
/* File: x86/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)            # save rIBASE
    GET_VREG rIBASE, %eax                   # rIBASE <- v[BB+0]
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1]
    orl     (rFP,%ecx,4), rIBASE                                 # ex: addl   (rFP,%ecx,4),rIBASE
    orl     4(rFP,%ecx,4), %eax                                 # ex: adcl   4(rFP,%ecx,4),%eax
    SET_VREG rIBASE, rINST                  # v[AA+0] <- rIBASE
    movl    LOCAL0(%esp), rIBASE            # restore rIBASE
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_long: /* 0xa2 */
/* File: x86/op_xor_long.S */
/* File: x86/binopWide.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)            # save rIBASE
    GET_VREG rIBASE, %eax                   # rIBASE <- v[BB+0]
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1]
    xorl    (rFP,%ecx,4), rIBASE                                 # ex: addl   (rFP,%ecx,4),rIBASE
    xorl    4(rFP,%ecx,4), %eax                                 # ex: adcl   4(rFP,%ecx,4),%eax
    SET_VREG rIBASE, rINST                  # v[AA+0] <- rIBASE
    movl    LOCAL0(%esp), rIBASE            # restore rIBASE
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shl_long: /* 0xa3 */
/* File: x86/op_shl_long.S */
/*
 * Long integer shift.  This is different from the generic 32/64-bit
 * binary operations because vAA/vBB are 64-bit but vCC (the shift
 * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
 * 6 bits of the shift distance.  x86 shifts automatically mask off
 * the low 5 bits of %cl, so have to handle the 64 > shiftcount > 31
 * case specially.
 */
    /* shl-long vAA, vBB, vCC */
    /* ecx gets shift count */
    /* Need to spill rINST */
    /* rINSTw gets AA */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, %eax              # ecx <- v[BB+1]
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    GET_VREG %eax, %eax                     # eax <- v[BB+0]
    shldl   %eax,rIBASE
    sall    %cl, %eax
    testb   $32, %cl
    je      2f
    movl    %eax, rIBASE
    xorl    %eax, %eax
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- %eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_shr_long: /* 0xa4 */
/* File: x86/op_shr_long.S */
/*
 * Long integer shift.  This is different from the generic 32/64-bit
 * binary operations because vAA/vBB are 64-bit but vCC (the shift
 * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
 * 6 bits of the shift distance.  x86 shifts automatically mask off
 * the low 5 bits of %cl, so have to handle the 64 > shiftcount > 31
 * case specially.
 */
    /* shr-long vAA, vBB, vCC */
    /* ecx gets shift count */
    /* Need to spill rIBASE */
    /* rINSTw gets AA */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, %eax              # rIBASE<- v[BB+1]
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    GET_VREG %eax, %eax                     # eax <- v[BB+0]
    shrdl   rIBASE, %eax
    sarl    %cl, rIBASE
    testb   $32, %cl
    je      2f
    movl    rIBASE, %eax
    sarl    $31, rIBASE
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_ushr_long: /* 0xa5 */
/* File: x86/op_ushr_long.S */
/*
 * Long integer shift.  This is different from the generic 32/64-bit
 * binary operations because vAA/vBB are 64-bit but vCC (the shift
 * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
 * 6 bits of the shift distance.  x86 shifts automatically mask off
 * the low 5 bits of %cl, so have to handle the 64 > shiftcount > 31
 * case specially.
 */
    /* shr-long vAA, vBB, vCC */
    /* ecx gets shift count */
    /* Need to spill rIBASE */
    /* rINSTw gets AA */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, %eax              # rIBASE <- v[BB+1]
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    GET_VREG %eax, %eax                     # eax <- v[BB+0]
    shrdl   rIBASE, %eax
    shrl    %cl, rIBASE
    testb   $32, %cl
    je      2f
    movl    rIBASE, %eax
    xorl    rIBASE, rIBASE
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[BB+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_add_float: /* 0xa6 */
/* File: x86/op_add_float.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movss   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    addss VREG_ADDRESS(%eax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_float: /* 0xa7 */
/* File: x86/op_sub_float.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movss   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    subss VREG_ADDRESS(%eax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_float: /* 0xa8 */
/* File: x86/op_mul_float.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movss   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    mulss VREG_ADDRESS(%eax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_float: /* 0xa9 */
/* File: x86/op_div_float.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movss   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    divss VREG_ADDRESS(%eax), %xmm0
    movss   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movss   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_float: /* 0xaa */
/* File: x86/op_rem_float.S */
    /* rem_float vAA, vBB, vCC */
    movzbl  3(rPC), %ecx                    # ecx <- BB
    movzbl  2(rPC), %eax                    # eax <- CC
    flds    VREG_ADDRESS(%ecx)              # vBB to fp stack
    flds    VREG_ADDRESS(%eax)              # vCC to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(rINST)             # %st to vAA
    CLEAR_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_add_double: /* 0xab */
/* File: x86/op_add_double.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movsd   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    addsd VREG_ADDRESS(%eax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_sub_double: /* 0xac */
/* File: x86/op_sub_double.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movsd   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    subsd VREG_ADDRESS(%eax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_double: /* 0xad */
/* File: x86/op_mul_double.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movsd   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    mulsd VREG_ADDRESS(%eax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_div_double: /* 0xae */
/* File: x86/op_div_double.S */
/* File: x86/sseBinop.S */
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movsd   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    divsd VREG_ADDRESS(%eax), %xmm0
    movsd   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movsd   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_double: /* 0xaf */
/* File: x86/op_rem_double.S */
    /* rem_double vAA, vBB, vCC */
    movzbl  3(rPC), %ecx                    # ecx <- BB
    movzbl  2(rPC), %eax                    # eax <- CC
    fldl    VREG_ADDRESS(%ecx)              # %st1 <- fp[vBB]
    fldl    VREG_ADDRESS(%eax)              # %st0 <- fp[vCC]
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(rINST)             # fp[vAA] <- %st
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_add_int_2addr: /* 0xb0 */
/* File: x86/op_add_int_2addr.S */
/* File: x86/binop2addr.S */
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
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf, %cl                      # ecx <- A
    addl    %eax, (rFP,%ecx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_int_2addr: /* 0xb1 */
/* File: x86/op_sub_int_2addr.S */
/* File: x86/binop2addr.S */
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
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf, %cl                      # ecx <- A
    subl    %eax, (rFP,%ecx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_int_2addr: /* 0xb2 */
/* File: x86/op_mul_int_2addr.S */
    /* mul vA, vB */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf, %cl                      # ecx <- A
    movl    rIBASE, rINST
    imull   (rFP,%ecx,4), %eax              # trashes rIBASE/edx
    movl    rINST, rIBASE
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_div_int_2addr: /* 0xb3 */
/* File: x86/op_div_int_2addr.S */
/* File: x86/bindiv2addr.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    mov     rIBASE, LOCAL0(%esp)
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $-1, %ecx
    jne     .Lop_div_int_2addr_continue_div2addr
    cmpl    $0x80000000, %eax
    jne     .Lop_div_int_2addr_continue_div2addr
    movl    $0x80000000, %eax
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.Lop_div_int_2addr_continue_div2addr:
    cltd
    idivl   %ecx
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_rem_int_2addr: /* 0xb4 */
/* File: x86/op_rem_int_2addr.S */
/* File: x86/bindiv2addr.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    mov     rIBASE, LOCAL0(%esp)
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $-1, %ecx
    jne     .Lop_rem_int_2addr_continue_div2addr
    cmpl    $0x80000000, %eax
    jne     .Lop_rem_int_2addr_continue_div2addr
    movl    $0, rIBASE
    SET_VREG rIBASE, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.Lop_rem_int_2addr_continue_div2addr:
    cltd
    idivl   %ecx
    SET_VREG rIBASE, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_and_int_2addr: /* 0xb5 */
/* File: x86/op_and_int_2addr.S */
/* File: x86/binop2addr.S */
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
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf, %cl                      # ecx <- A
    andl    %eax, (rFP,%ecx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_or_int_2addr: /* 0xb6 */
/* File: x86/op_or_int_2addr.S */
/* File: x86/binop2addr.S */
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
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf, %cl                      # ecx <- A
    orl     %eax, (rFP,%ecx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_xor_int_2addr: /* 0xb7 */
/* File: x86/op_xor_int_2addr.S */
/* File: x86/binop2addr.S */
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
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $0xf, %cl                      # ecx <- A
    xorl    %eax, (rFP,%ecx,4)                                  # for ex: addl   %eax,(rFP,%ecx,4)
    CLEAR_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shl_int_2addr: /* 0xb8 */
/* File: x86/op_shl_int_2addr.S */
/* File: x86/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vAA
    sall    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shr_int_2addr: /* 0xb9 */
/* File: x86/op_shr_int_2addr.S */
/* File: x86/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vAA
    sarl    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_2addr: /* 0xba */
/* File: x86/op_ushr_int_2addr.S */
/* File: x86/shop2addr.S */
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vAA
    shrl    %cl, %eax                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_add_long_2addr: /* 0xbb */
/* File: x86/op_add_long_2addr.S */
/* File: x86/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %eax, %ecx                     # eax<- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # eax<- v[B+1]
    andb    $0xF, rINSTbl                  # rINST<- A
    addl    %eax, (rFP,rINST,4)                                 # ex: addl   %eax,(rFP,rINST,4)
    adcl    %ecx, 4(rFP,rINST,4)                                 # ex: adcl   %ecx,4(rFP,rINST,4)
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_long_2addr: /* 0xbc */
/* File: x86/op_sub_long_2addr.S */
/* File: x86/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %eax, %ecx                     # eax<- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # eax<- v[B+1]
    andb    $0xF, rINSTbl                  # rINST<- A
    subl    %eax, (rFP,rINST,4)                                 # ex: addl   %eax,(rFP,rINST,4)
    sbbl    %ecx, 4(rFP,rINST,4)                                 # ex: adcl   %ecx,4(rFP,rINST,4)
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_long_2addr: /* 0xbd */
/* File: x86/op_mul_long_2addr.S */
/*
 * Signed 64-bit integer multiply, 2-addr version
 *
 * We could definately use more free registers for
 * this code.  We must spill %edx (rIBASE) because it
 * is used by imul.  We'll also spill rINST (ebx),
 * giving us eax, ebc, ecx and rIBASE as computational
 * temps.  On top of that, we'll spill %esi (edi)
 * for use as the vA pointer and rFP (esi) for use
 * as the vB pointer.  Yuck.
 */
    /* mul-long/2addr vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- BA
    andb    $0xf, %al                      # eax <- A
    CLEAR_WIDE_REF %eax                     # clear refs in advance
    sarl    $4, rINST                      # rINST <- B
    mov     rPC, LOCAL0(%esp)               # save Interpreter PC
    mov     rFP, LOCAL1(%esp)               # save FP
    mov     rIBASE, LOCAL2(%esp)            # save rIBASE
    leal    (rFP,%eax,4), %esi              # esi <- &v[A]
    leal    (rFP,rINST,4), rFP              # rFP <- &v[B]
    movl    4(%esi), %ecx                   # ecx <- Amsw
    imull   (rFP), %ecx                     # ecx <- (Amsw*Blsw)
    movl    4(rFP), %eax                    # eax <- Bmsw
    imull   (%esi), %eax                    # eax <- (Bmsw*Alsw)
    addl    %eax, %ecx                      # ecx <- (Amsw*Blsw)+(Bmsw*Alsw)
    movl    (rFP), %eax                     # eax <- Blsw
    mull    (%esi)                          # eax <- (Blsw*Alsw)
    leal    (%ecx,rIBASE), rIBASE           # full result now in %edx:%eax
    movl    rIBASE, 4(%esi)                 # v[A+1] <- rIBASE
    movl    %eax, (%esi)                    # v[A] <- %eax
    mov     LOCAL0(%esp), rPC               # restore Interpreter PC
    mov     LOCAL2(%esp), rIBASE            # restore IBASE
    mov     LOCAL1(%esp), rFP               # restore FP
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_div_long_2addr: /* 0xbe */
/* File: x86/op_div_long_2addr.S */
/* art_quick_* methods has quick abi,
 *   so use eax, ecx, edx, ebx for args
 */
    /* div/2addr vA, vB */
    .extern   art_quick_ldiv
    mov     rIBASE, LOCAL0(%esp)            # save rIBASE/%edx
    movzbl  rINSTbl, %eax
    shrl    $4, %eax                       # eax <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    mov     rINST, LOCAL1(%esp)             # save rINST/%ebx
    movl    %ebx, %ecx
    GET_VREG %edx, %eax
    GET_VREG_HIGH %ebx, %eax
    movl    %edx, %eax
    orl     %ebx, %eax
    jz      common_errDivideByZero
    GET_VREG %eax, %ecx
    GET_VREG_HIGH %ecx, %ecx
    call    SYMBOL(art_quick_ldiv)
    mov     LOCAL1(%esp), rINST             # restore rINST/%ebx
    SET_VREG_HIGH rIBASE, rINST
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE            # restore rIBASE/%edx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_rem_long_2addr: /* 0xbf */
/* File: x86/op_rem_long_2addr.S */
/* File: x86/op_div_long_2addr.S */
/* art_quick_* methods has quick abi,
 *   so use eax, ecx, edx, ebx for args
 */
    /* div/2addr vA, vB */
    .extern   art_quick_lmod
    mov     rIBASE, LOCAL0(%esp)            # save rIBASE/%edx
    movzbl  rINSTbl, %eax
    shrl    $4, %eax                       # eax <- B
    andb    $0xf, rINSTbl                  # rINST <- A
    mov     rINST, LOCAL1(%esp)             # save rINST/%ebx
    movl    %ebx, %ecx
    GET_VREG %edx, %eax
    GET_VREG_HIGH %ebx, %eax
    movl    %edx, %eax
    orl     %ebx, %eax
    jz      common_errDivideByZero
    GET_VREG %eax, %ecx
    GET_VREG_HIGH %ecx, %ecx
    call    SYMBOL(art_quick_lmod)
    mov     LOCAL1(%esp), rINST             # restore rINST/%ebx
    SET_VREG_HIGH rIBASE, rINST
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE            # restore rIBASE/%edx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_and_long_2addr: /* 0xc0 */
/* File: x86/op_and_long_2addr.S */
/* File: x86/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %eax, %ecx                     # eax<- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # eax<- v[B+1]
    andb    $0xF, rINSTbl                  # rINST<- A
    andl    %eax, (rFP,rINST,4)                                 # ex: addl   %eax,(rFP,rINST,4)
    andl    %ecx, 4(rFP,rINST,4)                                 # ex: adcl   %ecx,4(rFP,rINST,4)
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_or_long_2addr: /* 0xc1 */
/* File: x86/op_or_long_2addr.S */
/* File: x86/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %eax, %ecx                     # eax<- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # eax<- v[B+1]
    andb    $0xF, rINSTbl                  # rINST<- A
    orl     %eax, (rFP,rINST,4)                                 # ex: addl   %eax,(rFP,rINST,4)
    orl     %ecx, 4(rFP,rINST,4)                                 # ex: adcl   %ecx,4(rFP,rINST,4)
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_xor_long_2addr: /* 0xc2 */
/* File: x86/op_xor_long_2addr.S */
/* File: x86/binopWide2addr.S */
/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $4, %ecx                       # ecx<- B
    GET_VREG %eax, %ecx                     # eax<- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # eax<- v[B+1]
    andb    $0xF, rINSTbl                  # rINST<- A
    xorl    %eax, (rFP,rINST,4)                                 # ex: addl   %eax,(rFP,rINST,4)
    xorl    %ecx, 4(rFP,rINST,4)                                 # ex: adcl   %ecx,4(rFP,rINST,4)
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_shl_long_2addr: /* 0xc3 */
/* File: x86/op_shl_long_2addr.S */
/*
 * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
 * 32-bit shift distance.
 */
    /* shl-long/2addr vA, vB */
    /* ecx gets shift count */
    /* Need to spill rIBASE */
    /* rINSTw gets AA */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- v[AA+0]
    sarl    $4, %ecx                       # ecx <- B
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, rINST             # rIBASE <- v[AA+1]
    GET_VREG %ecx, %ecx                     # ecx <- vBB
    shldl   %eax, rIBASE
    sall    %cl, %eax
    testb   $32, %cl
    je      2f
    movl    %eax, rIBASE
    xorl    %eax, %eax
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_shr_long_2addr: /* 0xc4 */
/* File: x86/op_shr_long_2addr.S */
/*
 * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
 * 32-bit shift distance.
 */
    /* shl-long/2addr vA, vB */
    /* ecx gets shift count */
    /* Need to spill rIBASE */
    /* rINSTw gets AA */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- v[AA+0]
    sarl    $4, %ecx                       # ecx <- B
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, rINST             # rIBASE <- v[AA+1]
    GET_VREG %ecx, %ecx                     # ecx <- vBB
    shrdl   rIBASE, %eax
    sarl    %cl, rIBASE
    testb   $32, %cl
    je      2f
    movl    rIBASE, %eax
    sarl    $31, rIBASE
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_ushr_long_2addr: /* 0xc5 */
/* File: x86/op_ushr_long_2addr.S */
/*
 * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
 * 32-bit shift distance.
 */
    /* shl-long/2addr vA, vB */
    /* ecx gets shift count */
    /* Need to spill rIBASE */
    /* rINSTw gets AA */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- v[AA+0]
    sarl    $4, %ecx                       # ecx <- B
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, rINST             # rIBASE <- v[AA+1]
    GET_VREG %ecx, %ecx                     # ecx <- vBB
    shrdl   rIBASE, %eax
    shrl    %cl, rIBASE
    testb   $32, %cl
    je      2f
    movl    rIBASE, %eax
    xorl    rIBASE, rIBASE
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_add_float_2addr: /* 0xc6 */
/* File: x86/op_add_float_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    addss VREG_ADDRESS(rINST), %xmm0
    movss %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_float_2addr: /* 0xc7 */
/* File: x86/op_sub_float_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    subss VREG_ADDRESS(rINST), %xmm0
    movss %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_float_2addr: /* 0xc8 */
/* File: x86/op_mul_float_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    mulss VREG_ADDRESS(rINST), %xmm0
    movss %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_div_float_2addr: /* 0xc9 */
/* File: x86/op_div_float_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movss VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    divss VREG_ADDRESS(rINST), %xmm0
    movss %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movss %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_rem_float_2addr: /* 0xca */
/* File: x86/op_rem_float_2addr.S */
    /* rem_float/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    flds    VREG_ADDRESS(rINST)             # vB to fp stack
    andb    $0xf, %cl                      # ecx <- A
    flds    VREG_ADDRESS(%ecx)              # vA to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(%ecx)              # %st to vA
    CLEAR_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_add_double_2addr: /* 0xcb */
/* File: x86/op_add_double_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    addsd VREG_ADDRESS(rINST), %xmm0
    movsd %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_sub_double_2addr: /* 0xcc */
/* File: x86/op_sub_double_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    subsd VREG_ADDRESS(rINST), %xmm0
    movsd %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_mul_double_2addr: /* 0xcd */
/* File: x86/op_mul_double_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    mulsd VREG_ADDRESS(rINST), %xmm0
    movsd %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_div_double_2addr: /* 0xce */
/* File: x86/op_div_double_2addr.S */
/* File: x86/sseBinop2Addr.S */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $0xf, %ecx                     # ecx <- A
    movsd VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $4, rINST                      # rINST<- B
    divsd VREG_ADDRESS(rINST), %xmm0
    movsd %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movsd %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1


/* ------------------------------ */
    .balign 128
.L_op_rem_double_2addr: /* 0xcf */
/* File: x86/op_rem_double_2addr.S */
    /* rem_double/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $4, rINST                      # rINST <- B
    fldl    VREG_ADDRESS(rINST)             # vB to fp stack
    andb    $0xf, %cl                      # ecx <- A
    fldl    VREG_ADDRESS(%ecx)              # vA to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(%ecx)              # %st to vA
    CLEAR_WIDE_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

/* ------------------------------ */
    .balign 128
.L_op_add_int_lit16: /* 0xd0 */
/* File: x86/op_add_int_lit16.S */
/* File: x86/binopLit16.S */
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
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    addl    %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rsub_int: /* 0xd1 */
/* File: x86/op_rsub_int.S */
/* this op is "rsub-int", but can be thought of as "rsub-int/lit16" */
/* File: x86/binopLit16.S */
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
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    subl    %eax, %ecx                                  # for example: addl %ecx, %eax
    SET_VREG %ecx, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit16: /* 0xd2 */
/* File: x86/op_mul_int_lit16.S */
    /* mul/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movl    rIBASE, %ecx
    movswl  2(rPC), rIBASE                  # rIBASE <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    imull   rIBASE, %eax                    # trashes rIBASE/edx
    movl    %ecx, rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_div_int_lit16: /* 0xd3 */
/* File: x86/op_div_int_lit16.S */
/* File: x86/bindivLit16.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $-1, %ecx
    jne     .Lop_div_int_lit16_continue_div
    cmpl    $0x80000000, %eax
    jne     .Lop_div_int_lit16_continue_div
    movl    $0x80000000, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.Lop_div_int_lit16_continue_div:
    mov     rIBASE, LOCAL0(%esp)
    cltd
    idivl   %ecx
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit16: /* 0xd4 */
/* File: x86/op_rem_int_lit16.S */
/* File: x86/bindivLit16.S */
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $-1, %ecx
    jne     .Lop_rem_int_lit16_continue_div
    cmpl    $0x80000000, %eax
    jne     .Lop_rem_int_lit16_continue_div
    movl    $0, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.Lop_rem_int_lit16_continue_div:
    mov     rIBASE, LOCAL0(%esp)
    cltd
    idivl   %ecx
    SET_VREG rIBASE, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_and_int_lit16: /* 0xd5 */
/* File: x86/op_and_int_lit16.S */
/* File: x86/binopLit16.S */
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
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    andl    %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit16: /* 0xd6 */
/* File: x86/op_or_int_lit16.S */
/* File: x86/binopLit16.S */
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
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    orl     %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit16: /* 0xd7 */
/* File: x86/op_xor_int_lit16.S */
/* File: x86/binopLit16.S */
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
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $0xf, rINSTbl                  # rINST <- A
    xorl    %ecx, %eax                                  # for example: addl %ecx, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_add_int_lit8: /* 0xd8 */
/* File: x86/op_add_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    addl    %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rsub_int_lit8: /* 0xd9 */
/* File: x86/op_rsub_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    subl    %eax, %ecx                                  # ex: addl %ecx,%eax
    SET_VREG %ecx, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit8: /* 0xda */
/* File: x86/op_mul_int_lit8.S */
    /* mul/lit8 vAA, vBB, #+CC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movl    rIBASE, %ecx
    GET_VREG  %eax, %eax                    # eax <- rBB
    movsbl  3(rPC), rIBASE                  # rIBASE <- ssssssCC
    imull   rIBASE, %eax                    # trashes rIBASE/edx
    movl    %ecx, rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_div_int_lit8: /* 0xdb */
/* File: x86/op_div_int_lit8.S */
/* File: x86/bindivLit8.S */
/*
 * 32-bit div/rem "lit8" binary operation.  Handles special case of
 * op0=minint & op1=-1
 */
    /* div/rem/lit8 vAA, vBB, #+CC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %eax                    # eax <- rBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $0x80000000, %eax
    jne     .Lop_div_int_lit8_continue_div
    cmpl    $-1, %ecx
    jne     .Lop_div_int_lit8_continue_div
    movl    $0x80000000, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.Lop_div_int_lit8_continue_div:
    mov     rIBASE, LOCAL0(%esp)
    cltd
    idivl   %ecx
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit8: /* 0xdc */
/* File: x86/op_rem_int_lit8.S */
/* File: x86/bindivLit8.S */
/*
 * 32-bit div/rem "lit8" binary operation.  Handles special case of
 * op0=minint & op1=-1
 */
    /* div/rem/lit8 vAA, vBB, #+CC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %eax                    # eax <- rBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $0x80000000, %eax
    jne     .Lop_rem_int_lit8_continue_div
    cmpl    $-1, %ecx
    jne     .Lop_rem_int_lit8_continue_div
    movl    $0, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.Lop_rem_int_lit8_continue_div:
    mov     rIBASE, LOCAL0(%esp)
    cltd
    idivl   %ecx
    SET_VREG rIBASE, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_and_int_lit8: /* 0xdd */
/* File: x86/op_and_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    andl    %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit8: /* 0xde */
/* File: x86/op_or_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    orl     %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit8: /* 0xdf */
/* File: x86/op_xor_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    xorl    %ecx, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shl_int_lit8: /* 0xe0 */
/* File: x86/op_shl_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    sall    %cl, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_shr_int_lit8: /* 0xe1 */
/* File: x86/op_shr_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    sarl    %cl, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_lit8: /* 0xe2 */
/* File: x86/op_ushr_int_lit8.S */
/* File: x86/binopLit8.S */
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
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG %eax, %eax                     # eax <- rBB
    shrl    %cl, %eax                                  # ex: addl %ecx,%eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_quick: /* 0xe3 */
/* File: x86/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movl (%ecx,%eax,1), %eax
    andb    $0xf,rINSTbl                   # rINST <- A
    SET_VREG %eax, rINST                    # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iget_wide_quick: /* 0xe4 */
/* File: x86/op_iget_wide_quick.S */
    /* iget-wide-quick vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movq    (%ecx,%eax,1), %xmm0
    andb    $0xf, rINSTbl                  # rINST <- A
    SET_WIDE_FP_VREG %xmm0, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iget_object_quick: /* 0xe5 */
/* File: x86/op_iget_object_quick.S */
    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movl    %ecx, OUT_ARG0(%esp)
    movl    %eax, OUT_ARG1(%esp)
    EXPORT_PC
    call    SYMBOL(artIGetObjectFromMterp)  # (obj, offset)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $0xf,rINSTbl                   # rINST <- A
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_quick: /* 0xe6 */
/* File: x86/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINST                   # rINST <- v[A]
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movl    rINST, (%ecx,%eax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_wide_quick: /* 0xe7 */
/* File: x86/op_iput_wide_quick.S */
    /* iput-wide-quick vA, vB, offset@CCCC */
    movzbl    rINSTbl, %ecx                 # ecx<- BA
    sarl      $4, %ecx                     # ecx<- B
    GET_VREG  %ecx, %ecx                    # vB (object we're operating on)
    testl     %ecx, %ecx                    # is object null?
    je        common_errNullObject
    movzwl    2(rPC), %eax                  # eax<- field byte offset
    leal      (%ecx,%eax,1), %ecx           # ecx<- Address of 64-bit target
    andb      $0xf, rINSTbl                # rINST<- A
    GET_WIDE_FP_VREG %xmm0, rINST           # xmm0<- fp[A]/fp[A+1]
    movq      %xmm0, (%ecx)                 # obj.field<- r0/r1
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_iput_object_quick: /* 0xe8 */
/* File: x86/op_iput_object_quick.S */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST 232
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpIputObjectQuick)
    testb   %al, %al
    jz      MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_quick: /* 0xe9 */
/* File: x86/op_invoke_virtual_quick.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuick
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 233
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeVirtualQuick)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range_quick: /* 0xea */
/* File: x86/op_invoke_virtual_range_quick.S */
/* File: x86/invoke.S */
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuickRange
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST 234
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL(MterpInvokeVirtualQuickRange)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT


/* ------------------------------ */
    .balign 128
.L_op_iput_boolean_quick: /* 0xeb */
/* File: x86/op_iput_boolean_quick.S */
/* File: x86/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINST                   # rINST <- v[A]
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movb    rINSTbl, (%ecx,%eax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_byte_quick: /* 0xec */
/* File: x86/op_iput_byte_quick.S */
/* File: x86/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINST                   # rINST <- v[A]
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movb    rINSTbl, (%ecx,%eax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_char_quick: /* 0xed */
/* File: x86/op_iput_char_quick.S */
/* File: x86/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINST                   # rINST <- v[A]
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movw    rINSTw, (%ecx,%eax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iput_short_quick: /* 0xee */
/* File: x86/op_iput_short_quick.S */
/* File: x86/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINST                   # rINST <- v[A]
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movw    rINSTw, (%ecx,%eax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean_quick: /* 0xef */
/* File: x86/op_iget_boolean_quick.S */
/* File: x86/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movsbl (%ecx,%eax,1), %eax
    andb    $0xf,rINSTbl                   # rINST <- A
    SET_VREG %eax, rINST                    # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_byte_quick: /* 0xf0 */
/* File: x86/op_iget_byte_quick.S */
/* File: x86/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movsbl (%ecx,%eax,1), %eax
    andb    $0xf,rINSTbl                   # rINST <- A
    SET_VREG %eax, rINST                    # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_char_quick: /* 0xf1 */
/* File: x86/op_iget_char_quick.S */
/* File: x86/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movzwl (%ecx,%eax,1), %eax
    andb    $0xf,rINSTbl                   # rINST <- A
    SET_VREG %eax, rINST                    # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_iget_short_quick: /* 0xf2 */
/* File: x86/op_iget_short_quick.S */
/* File: x86/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movswl (%ecx,%eax,1), %eax
    andb    $0xf,rINSTbl                   # rINST <- A
    SET_VREG %eax, rINST                    # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2


/* ------------------------------ */
    .balign 128
.L_op_invoke_lambda: /* 0xf3 */
/* Transfer stub to alternate interpreter */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_f4: /* 0xf4 */
/* File: x86/op_unused_f4.S */
/* File: x86/unused.S */
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
/* File: x86/op_unused_fa.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fb: /* 0xfb */
/* File: x86/op_unused_fb.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fc: /* 0xfc */
/* File: x86/op_unused_fc.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fd: /* 0xfd */
/* File: x86/op_unused_fd.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fe: /* 0xfe */
/* File: x86/op_unused_fe.S */
/* File: x86/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
    jmp     MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_ff: /* 0xff */
/* File: x86/op_unused_ff.S */
/* File: x86/unused.S */
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
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(0*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move: /* 0x01 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(1*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_from16: /* 0x02 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(2*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_16: /* 0x03 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(3*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide: /* 0x04 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(4*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_from16: /* 0x05 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(5*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_16: /* 0x06 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(6*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object: /* 0x07 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(7*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_from16: /* 0x08 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(8*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_16: /* 0x09 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(9*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result: /* 0x0a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(10*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_wide: /* 0x0b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(11*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_object: /* 0x0c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(12*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_exception: /* 0x0d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(13*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void: /* 0x0e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(14*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return: /* 0x0f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(15*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_wide: /* 0x10 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(16*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_object: /* 0x11 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(17*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_4: /* 0x12 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(18*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_16: /* 0x13 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(19*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const: /* 0x14 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(20*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_high16: /* 0x15 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(21*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_16: /* 0x16 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(22*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_32: /* 0x17 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(23*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide: /* 0x18 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(24*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_high16: /* 0x19 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(25*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string: /* 0x1a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(26*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string_jumbo: /* 0x1b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(27*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_class: /* 0x1c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(28*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_enter: /* 0x1d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(29*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_exit: /* 0x1e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(30*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_check_cast: /* 0x1f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(31*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_instance_of: /* 0x20 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(32*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_array_length: /* 0x21 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(33*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_instance: /* 0x22 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(34*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_array: /* 0x23 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(35*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array: /* 0x24 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(36*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array_range: /* 0x25 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(37*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_fill_array_data: /* 0x26 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(38*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_throw: /* 0x27 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(39*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto: /* 0x28 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(40*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_16: /* 0x29 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(41*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_32: /* 0x2a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(42*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_packed_switch: /* 0x2b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(43*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sparse_switch: /* 0x2c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(44*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_float: /* 0x2d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(45*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_float: /* 0x2e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(46*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_double: /* 0x2f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(47*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_double: /* 0x30 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(48*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmp_long: /* 0x31 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(49*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eq: /* 0x32 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(50*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ne: /* 0x33 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(51*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lt: /* 0x34 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(52*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ge: /* 0x35 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(53*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gt: /* 0x36 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(54*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_le: /* 0x37 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(55*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eqz: /* 0x38 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(56*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_nez: /* 0x39 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(57*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ltz: /* 0x3a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(58*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gez: /* 0x3b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(59*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gtz: /* 0x3c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(60*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lez: /* 0x3d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(61*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3e: /* 0x3e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(62*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3f: /* 0x3f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(63*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_40: /* 0x40 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(64*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_41: /* 0x41 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(65*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_42: /* 0x42 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(66*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_43: /* 0x43 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(67*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget: /* 0x44 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(68*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_wide: /* 0x45 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(69*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_object: /* 0x46 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(70*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_boolean: /* 0x47 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(71*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_byte: /* 0x48 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(72*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_char: /* 0x49 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(73*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_short: /* 0x4a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(74*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput: /* 0x4b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(75*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_wide: /* 0x4c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(76*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_object: /* 0x4d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(77*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_boolean: /* 0x4e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(78*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_byte: /* 0x4f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(79*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_char: /* 0x50 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(80*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_short: /* 0x51 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(81*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget: /* 0x52 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(82*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide: /* 0x53 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(83*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object: /* 0x54 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(84*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean: /* 0x55 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(85*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte: /* 0x56 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(86*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char: /* 0x57 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(87*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short: /* 0x58 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(88*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput: /* 0x59 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(89*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide: /* 0x5a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(90*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object: /* 0x5b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(91*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean: /* 0x5c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(92*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte: /* 0x5d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(93*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char: /* 0x5e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(94*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short: /* 0x5f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(95*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget: /* 0x60 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(96*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_wide: /* 0x61 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(97*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_object: /* 0x62 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(98*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_boolean: /* 0x63 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(99*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_byte: /* 0x64 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(100*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_char: /* 0x65 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(101*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_short: /* 0x66 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(102*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput: /* 0x67 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(103*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_wide: /* 0x68 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(104*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_object: /* 0x69 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(105*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_boolean: /* 0x6a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(106*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_byte: /* 0x6b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(107*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_char: /* 0x6c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(108*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_short: /* 0x6d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(109*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual: /* 0x6e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(110*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super: /* 0x6f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(111*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct: /* 0x70 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(112*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static: /* 0x71 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(113*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface: /* 0x72 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(114*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void_no_barrier: /* 0x73 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(115*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range: /* 0x74 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(116*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super_range: /* 0x75 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(117*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct_range: /* 0x76 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(118*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static_range: /* 0x77 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(119*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface_range: /* 0x78 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(120*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_79: /* 0x79 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(121*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_7a: /* 0x7a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(122*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_int: /* 0x7b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(123*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_int: /* 0x7c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(124*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_long: /* 0x7d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(125*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_long: /* 0x7e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(126*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_float: /* 0x7f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(127*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_double: /* 0x80 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(128*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_long: /* 0x81 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(129*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_float: /* 0x82 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(130*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_double: /* 0x83 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(131*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_int: /* 0x84 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(132*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_float: /* 0x85 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(133*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_double: /* 0x86 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(134*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_int: /* 0x87 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(135*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_long: /* 0x88 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(136*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_double: /* 0x89 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(137*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_int: /* 0x8a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(138*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_long: /* 0x8b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(139*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_float: /* 0x8c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(140*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_byte: /* 0x8d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(141*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_char: /* 0x8e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(142*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_short: /* 0x8f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(143*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int: /* 0x90 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(144*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int: /* 0x91 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(145*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int: /* 0x92 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(146*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int: /* 0x93 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(147*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int: /* 0x94 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(148*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int: /* 0x95 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(149*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int: /* 0x96 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(150*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int: /* 0x97 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(151*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int: /* 0x98 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(152*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int: /* 0x99 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(153*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int: /* 0x9a */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(154*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long: /* 0x9b */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(155*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long: /* 0x9c */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(156*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long: /* 0x9d */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(157*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long: /* 0x9e */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(158*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long: /* 0x9f */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(159*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long: /* 0xa0 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(160*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long: /* 0xa1 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(161*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long: /* 0xa2 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(162*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long: /* 0xa3 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(163*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long: /* 0xa4 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(164*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long: /* 0xa5 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(165*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float: /* 0xa6 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(166*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float: /* 0xa7 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(167*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float: /* 0xa8 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(168*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float: /* 0xa9 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(169*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float: /* 0xaa */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(170*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double: /* 0xab */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(171*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double: /* 0xac */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(172*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double: /* 0xad */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(173*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double: /* 0xae */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(174*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double: /* 0xaf */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(175*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_2addr: /* 0xb0 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(176*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int_2addr: /* 0xb1 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(177*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_2addr: /* 0xb2 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(178*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_2addr: /* 0xb3 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(179*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_2addr: /* 0xb4 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(180*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_2addr: /* 0xb5 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(181*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_2addr: /* 0xb6 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(182*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_2addr: /* 0xb7 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(183*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_2addr: /* 0xb8 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(184*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_2addr: /* 0xb9 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(185*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_2addr: /* 0xba */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(186*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long_2addr: /* 0xbb */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(187*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long_2addr: /* 0xbc */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(188*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long_2addr: /* 0xbd */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(189*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long_2addr: /* 0xbe */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(190*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long_2addr: /* 0xbf */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(191*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long_2addr: /* 0xc0 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(192*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long_2addr: /* 0xc1 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(193*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long_2addr: /* 0xc2 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(194*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long_2addr: /* 0xc3 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(195*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long_2addr: /* 0xc4 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(196*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long_2addr: /* 0xc5 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(197*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float_2addr: /* 0xc6 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(198*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float_2addr: /* 0xc7 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(199*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float_2addr: /* 0xc8 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(200*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float_2addr: /* 0xc9 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(201*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float_2addr: /* 0xca */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(202*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double_2addr: /* 0xcb */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(203*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double_2addr: /* 0xcc */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(204*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double_2addr: /* 0xcd */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(205*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double_2addr: /* 0xce */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(206*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double_2addr: /* 0xcf */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(207*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit16: /* 0xd0 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(208*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int: /* 0xd1 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(209*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit16: /* 0xd2 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(210*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit16: /* 0xd3 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(211*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit16: /* 0xd4 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(212*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit16: /* 0xd5 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(213*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit16: /* 0xd6 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(214*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit16: /* 0xd7 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(215*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit8: /* 0xd8 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(216*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int_lit8: /* 0xd9 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(217*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit8: /* 0xda */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(218*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit8: /* 0xdb */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(219*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit8: /* 0xdc */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(220*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit8: /* 0xdd */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(221*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit8: /* 0xde */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(222*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit8: /* 0xdf */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(223*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_lit8: /* 0xe0 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(224*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_lit8: /* 0xe1 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(225*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_lit8: /* 0xe2 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(226*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_quick: /* 0xe3 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(227*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide_quick: /* 0xe4 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(228*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object_quick: /* 0xe5 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(229*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_quick: /* 0xe6 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(230*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide_quick: /* 0xe7 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(231*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object_quick: /* 0xe8 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(232*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_quick: /* 0xe9 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(233*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range_quick: /* 0xea */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(234*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean_quick: /* 0xeb */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(235*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte_quick: /* 0xec */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(236*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char_quick: /* 0xed */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(237*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short_quick: /* 0xee */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(238*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean_quick: /* 0xef */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(239*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte_quick: /* 0xf0 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(240*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char_quick: /* 0xf1 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(241*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short_quick: /* 0xf2 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(242*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_lambda: /* 0xf3 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(243*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_f4: /* 0xf4 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(244*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_capture_variable: /* 0xf5 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(245*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_create_lambda: /* 0xf6 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(246*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_liberate_variable: /* 0xf7 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(247*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_box_lambda: /* 0xf8 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(248*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unbox_lambda: /* 0xf9 */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(249*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fa: /* 0xfa */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(250*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fb: /* 0xfb */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(251*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fc: /* 0xfc */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(252*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fd: /* 0xfd */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(253*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fe: /* 0xfe */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(254*128)

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_ff: /* 0xff */
/* File: x86/alt_stub.S */
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

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(255*128)

    .balign 128
    SIZE(SYMBOL(artMterpAsmAltInstructionStart),SYMBOL(artMterpAsmAltInstructionStart))
    .global SYMBOL(artMterpAsmAltInstructionEnd)
SYMBOL(artMterpAsmAltInstructionEnd):
/* File: x86/footer.S */
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
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogDivideByZeroException)
#endif
    jmp     MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogArrayIndexException)
#endif
    jmp     MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogNegativeArraySizeException)
#endif
    jmp     MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogNoSuchMethodException)
#endif
    jmp     MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogNullObjectException)
#endif
    jmp     MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG0(%esp)
    call    SYMBOL(MterpLogExceptionThrownException)
#endif
    jmp     MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG0(%esp)
    movl    THREAD_FLAGS_OFFSET(%eax), %eax
    movl    %eax, OUT_ARG2(%esp)
    call    SYMBOL(MterpLogSuspendFallback)
#endif
    jmp     MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    movl    rSELF, %eax
    testl   $-1, THREAD_EXCEPTION_OFFSET(%eax)
    jz      MterpFallback
    /* intentional fallthrough - handle pending exception. */

/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpHandleException)
    testb   %al, %al
    jz      MterpExceptionReturn
    movl    OFF_FP_CODE_ITEM(rFP), %eax
    movl    OFF_FP_DEX_PC(rFP), %ecx
    lea     CODEITEM_INSNS_OFFSET(%eax), rPC
    lea     (rPC, %ecx, 2), rPC
    movl    rPC, OFF_FP_DEX_PC_PTR(rFP)
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
    cmpw    $JIT_CHECK_OSR, rPROFILE
    je      .L_osr_check
    decw    rPROFILE
    je      .L_add_batch                    # counted down to zero - report
.L_resume_backward_branch:
    movl    rSELF, %eax
    testl   $(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    leal    (rPC, rINST, 2), rPC
    FETCH_INST
    jnz     .L_suspend_request_pending
    REFRESH_IBASE
    GOTO_NEXT

.L_suspend_request_pending:
    EXPORT_PC
    movl    %eax, OUT_ARG0(%esp)            # rSELF in eax
    call    SYMBOL(MterpSuspendCheck)       # (self)
    testb   %al, %al
    jnz     MterpFallback
    REFRESH_IBASE                           # might have changed during suspend
    GOTO_NEXT

.L_no_count_backwards:
    cmpw    $JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    jne     .L_resume_backward_branch
.L_osr_check:
    EXPORT_PC
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jz      .L_resume_backward_branch
    jmp     MterpOnStackReplacement

.L_forward_branch:
    cmpw    $JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    je      .L_check_osr_forward
.L_resume_forward_branch:
    leal    (rPC, rINST, 2), rPC
    FETCH_INST
    GOTO_NEXT

.L_check_osr_forward:
    EXPORT_PC
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    REFRESH_IBASE
    jz      .L_resume_forward_branch
    jmp     MterpOnStackReplacement

.L_add_batch:
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG2(%esp)
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    jmp     .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    EXPORT_PC
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    $2, OUT_ARG2(%esp)
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    REFRESH_IBASE
    jnz     MterpOnStackReplacement
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rINST, OUT_ARG2(%esp)
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
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogFallback)
#endif
MterpCommonFallback:
    xor     %eax, %eax
    jmp     MterpDone

/*
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    movl    $1, %eax
    jmp     MterpDone
MterpReturn:
    movl    OFF_FP_RESULT_REGISTER(rFP), %edx
    movl    %eax, (%edx)
    movl    %ecx, 4(%edx)
    mov     $1, %eax
MterpDone:
/*
 * At this point, we expect rPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending rPROFILE and the cached hotness counter).  rPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    cmpw    $0, rPROFILE
    jle     MRestoreFrame                   # if > 0, we may have some counts to report.

    movl    %eax, rINST                     # stash return value
    /* Report cached hotness counts */
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG2(%esp)
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    movl    rINST, %eax                     # restore return value

    /* pop up frame */
MRestoreFrame:
    addl    $FRAME_SIZE, %esp
    .cfi_adjust_cfa_offset -FRAME_SIZE

    /* Restore callee save register */
    POP     %ebx
    POP     %esi
    POP     %edi
    POP     %ebp
    ret
    .cfi_endproc
    SIZE(ExecuteMterpImpl,ExecuteMterpImpl)

