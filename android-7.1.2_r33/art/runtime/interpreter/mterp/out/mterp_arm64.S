/*
 * This file was generated automatically by gen-mterp.py for 'arm64'.
 *
 * --> DO NOT EDIT <--
 */

/* File: arm64/header.S */
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

  Some (hopefully) temporary ugliness.  We'll treat xFP as pointing to the
  base of the vreg array within the shadow frame.  Access the other fields,
  dex_pc_, method_ and number_of_vregs_ via negative offsets.  For now, we'll continue
  the shadow frame mechanism of double-storing object references - via xFP &
  number_of_vregs_.

 */

/*
ARM64 Runtime register usage conventions.

  r0     : w0 is 32-bit return register and x0 is 64-bit.
  r0-r7  : Argument registers.
  r8-r15 : Caller save registers (used as temporary registers).
  r16-r17: Also known as ip0-ip1, respectively. Used as scratch registers by
           the linker, by the trampolines and other stubs (the backend uses
           these as temporary registers).
  r18    : Caller save register (used as temporary register).
  r19    : Pointer to thread-local storage.
  r20-r29: Callee save registers.
  r30    : (lr) is reserved (the link register).
  rsp    : (sp) is reserved (the stack pointer).
  rzr    : (zr) is reserved (the zero register).

  Floating-point registers
  v0-v31

  v0     : s0 is return register for singles (32-bit) and d0 for doubles (64-bit).
           This is analogous to the C/C++ (hard-float) calling convention.
  v0-v7  : Floating-point argument registers in both Dalvik and C/C++ conventions.
           Also used as temporary and codegen scratch registers.

  v0-v7 and v16-v31 : trashed across C calls.
  v8-v15 : bottom 64-bits preserved across C calls (d8-d15 are preserved).

  v16-v31: Used as codegen temp/scratch.
  v8-v15 : Can be used for promotion.

  Must maintain 16-byte stack alignment.

Mterp notes:

The following registers have fixed assignments:

  reg nick      purpose
  x20  xPC       interpreted program counter, used for fetching instructions
  x21  xFP       interpreted frame pointer, used for accessing locals and args
  x22  xSELF     self (Thread) pointer
  x23  xINST     first 16-bit code unit of current instruction
  x24  xIBASE    interpreted instruction base pointer, used for computed goto
  x25  xREFS     base of object references in shadow frame  (ideally, we'll get rid of this later).
  x26  wPROFILE  jit profile hotness countdown
  x16  ip        scratch reg
  x17  ip2       scratch reg (used by macros)

Macros are provided for common operations.  They MUST NOT alter unspecified registers or condition
codes.
*/

/*
 * This is a #include, not a %include, because we want the C pre-processor
 * to expand the macros into assembler assignment statements.
 */
#include "asm_support.h"

#define MTERP_PROFILE_BRANCHES 1
#define MTERP_LOGGING 0

/* During bringup, we'll use the shadow frame model instead of xFP */
/* single-purpose registers, given names for clarity */
#define xPC      x20
#define xFP      x21
#define xSELF    x22
#define xINST    x23
#define wINST    w23
#define xIBASE   x24
#define xREFS    x25
#define wPROFILE w26
#define xPROFILE x26
#define ip       x16
#define ip2      x17

/*
 * Instead of holding a pointer to the shadow frame, we keep xFP at the base of the vregs.  So,
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
#define OFF_FP_SHADOWFRAME OFF_FP(0)

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
    str  xPC, [xFP, #OFF_FP_DEX_PC_PTR]
.endm

/*
 * Fetch the next instruction from xPC into wINST.  Does not advance xPC.
 */
.macro FETCH_INST
    ldrh    wINST, [xPC]
.endm

/*
 * Fetch the next instruction from the specified offset.  Advances xPC
 * to point to the next instruction.  "_count" is in 16-bit code units.
 *
 * Because of the limited size of immediate constants on ARM, this is only
 * suitable for small forward movements (i.e. don't try to implement "goto"
 * with this).
 *
 * This must come AFTER anything that can throw an exception, or the
 * exception catch may miss.  (This also implies that it must come after
 * EXPORT_PC.)
 */
.macro FETCH_ADVANCE_INST count
    ldrh    wINST, [xPC, #((\count)*2)]!
.endm

/*
 * The operation performed here is similar to FETCH_ADVANCE_INST, except the
 * src and dest registers are parameterized (not hard-wired to xPC and xINST).
 */
.macro PREFETCH_ADVANCE_INST dreg, sreg, count
    ldrh    \dreg, [\sreg, #((\count)*2)]!
.endm

/*
 * Similar to FETCH_ADVANCE_INST, but does not update xPC.  Used to load
 * xINST ahead of possible exception point.  Be sure to manually advance xPC
 * later.
 */
.macro PREFETCH_INST count
    ldrh    wINST, [xPC, #((\count)*2)]
.endm

/* Advance xPC by some number of code units. */
.macro ADVANCE count
  add  xPC, xPC, #((\count)*2)
.endm

/*
 * Fetch the next instruction from an offset specified by _reg and advance xPC.
 * xPC to point to the next instruction.  "_reg" must specify the distance
 * in bytes, *not* 16-bit code units, and may be a signed value.  Must not set flags.
 *
 */
.macro FETCH_ADVANCE_INST_RB reg
    add     xPC, xPC, \reg, sxtw
    ldrh    wINST, [xPC]
.endm

/*
 * Fetch a half-word code unit from an offset past the current PC.  The
 * "_count" value is in 16-bit code units.  Does not advance xPC.
 *
 * The "_S" variant works the same but treats the value as signed.
 */
.macro FETCH reg, count
    ldrh    \reg, [xPC, #((\count)*2)]
.endm

.macro FETCH_S reg, count
    ldrsh   \reg, [xPC, #((\count)*2)]
.endm

/*
 * Fetch one byte from an offset past the current PC.  Pass in the same
 * "_count" as you would for FETCH, and an additional 0/1 indicating which
 * byte of the halfword you want (lo/hi).
 */
.macro FETCH_B reg, count, byte
    ldrb     \reg, [xPC, #((\count)*2+(\byte))]
.endm

/*
 * Put the instruction's opcode field into the specified register.
 */
.macro GET_INST_OPCODE reg
    and     \reg, xINST, #255
.endm

/*
 * Put the prefetched instruction's opcode field into the specified register.
 */
.macro GET_PREFETCHED_OPCODE oreg, ireg
    and     \oreg, \ireg, #255
.endm

/*
 * Begin executing the opcode in _reg.  Clobbers reg
 */

.macro GOTO_OPCODE reg
    add     \reg, xIBASE, \reg, lsl #7
    br      \reg
.endm
.macro GOTO_OPCODE_BASE base,reg
    add     \reg, \base, \reg, lsl #7
    br      \reg
.endm

/*
 * Get/set the 32-bit value from a Dalvik register.
 */
.macro GET_VREG reg, vreg
    ldr     \reg, [xFP, \vreg, uxtw #2]
.endm
.macro SET_VREG reg, vreg
    str     \reg, [xFP, \vreg, uxtw #2]
    str     wzr, [xREFS, \vreg, uxtw #2]
.endm
.macro SET_VREG_OBJECT reg, vreg, tmpreg
    str     \reg, [xFP, \vreg, uxtw #2]
    str     \reg, [xREFS, \vreg, uxtw #2]
.endm

/*
 * Get/set the 64-bit value from a Dalvik register.
 * TUNING: can we do better here?
 */
.macro GET_VREG_WIDE reg, vreg
    add     ip2, xFP, \vreg, lsl #2
    ldr     \reg, [ip2]
.endm
.macro SET_VREG_WIDE reg, vreg
    add     ip2, xFP, \vreg, lsl #2
    str     \reg, [ip2]
    add     ip2, xREFS, \vreg, lsl #2
    str     xzr, [ip2]
.endm

/*
 * Convert a virtual register index into an address.
 */
.macro VREG_INDEX_TO_ADDR reg, vreg
    add     \reg, xFP, \vreg, lsl #2   /* WARNING: handle shadow frame vreg zero if store */
.endm

/*
 * Refresh handler table.
 */
.macro REFRESH_IBASE
  ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]
.endm

/* File: arm64/entry.S */
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

    .text

/*
 * Interpreter entry point.
 * On entry:
 *  x0  Thread* self/
 *  x1  code_item
 *  x2  ShadowFrame
 *  x3  JValue* result_register
 *
 */
    .global ExecuteMterpImpl
    .type   ExecuteMterpImpl, %function
    .balign 16

ExecuteMterpImpl:
    .cfi_startproc
    stp     xPROFILE, x27, [sp, #-80]!
    stp     xIBASE, xREFS, [sp, #16]
    stp     xSELF, xINST, [sp, #32]
    stp     xPC, xFP, [sp, #48]
    stp     fp, lr, [sp, #64]
    add     fp, sp, #64

    /* Remember the return register */
    str     x3, [x2, #SHADOWFRAME_RESULT_REGISTER_OFFSET]

    /* Remember the code_item */
    str     x1, [x2, #SHADOWFRAME_CODE_ITEM_OFFSET]

    /* set up "named" registers */
    mov     xSELF, x0
    ldr     w0, [x2, #SHADOWFRAME_NUMBER_OF_VREGS_OFFSET]
    add     xFP, x2, #SHADOWFRAME_VREGS_OFFSET     // point to vregs.
    add     xREFS, xFP, w0, lsl #2                 // point to reference array in shadow frame
    ldr     w0, [x2, #SHADOWFRAME_DEX_PC_OFFSET]   // Get starting dex_pc.
    add     xPC, x1, #CODEITEM_INSNS_OFFSET        // Point to base of insns[]
    add     xPC, xPC, w0, lsl #1                   // Create direct pointer to 1st dex opcode
    EXPORT_PC

    /* Starting ibase */
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]

    /* Set up for backwards branches & osr profiling */
    ldr     x0, [xFP, #OFF_FP_METHOD]
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    bl      MterpSetUpHotnessCountdown
    mov     wPROFILE, w0                // Starting hotness countdown to xPROFILE

    /* start executing the instruction at rPC */
    FETCH_INST                          // load wINST from rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
    /* NOTE: no fallthrough */


    .global artMterpAsmInstructionStart
    .type   artMterpAsmInstructionStart, %function
artMterpAsmInstructionStart = .L_op_nop
    .text

/* ------------------------------ */
    .balign 128
.L_op_nop: /* 0x00 */
/* File: arm64/op_nop.S */
    FETCH_ADVANCE_INST 1                // advance to next instr, load rINST
    GET_INST_OPCODE ip                  // ip<- opcode from rINST
    GOTO_OPCODE ip                      // execute it

/* ------------------------------ */
    .balign 128
.L_op_move: /* 0x01 */
/* File: arm64/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    lsr     w1, wINST, #12              // x1<- B from 15:12
    ubfx    w0, wINST, #8, #4           // x0<- A from 11:8
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    GET_VREG w2, w1                     // x2<- fp[B]
    GET_INST_OPCODE ip                  // ip<- opcode from wINST
    .if 0
    SET_VREG_OBJECT w2, w0              // fp[A]<- x2
    .else
    SET_VREG w2, w0                     // fp[A]<- x2
    .endif
    GOTO_OPCODE ip                      // execute next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_from16: /* 0x02 */
/* File: arm64/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    FETCH w1, 1                         // r1<- BBBB
    lsr     w0, wINST, #8               // r0<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_VREG w2, w1                     // r2<- fp[BBBB]
    GET_INST_OPCODE ip                  // extract opcode from wINST
    .if 0
    SET_VREG_OBJECT w2, w0              // fp[AA]<- r2
    .else
    SET_VREG w2, w0                     // fp[AA]<- r2
    .endif
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_16: /* 0x03 */
/* File: arm64/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    FETCH w1, 2                         // w1<- BBBB
    FETCH w0, 1                         // w0<- AAAA
    FETCH_ADVANCE_INST 3                // advance xPC, load xINST
    GET_VREG w2, w1                     // w2<- fp[BBBB]
    GET_INST_OPCODE ip                  // extract opcode from xINST
    .if 0
    SET_VREG_OBJECT w2, w0              // fp[AAAA]<- w2
    .else
    SET_VREG w2, w0                     // fp[AAAA]<- w2
    .endif
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_wide: /* 0x04 */
/* File: arm64/op_move_wide.S */
    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE  x3, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE  x3, w2
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_wide_from16: /* 0x05 */
/* File: arm64/op_move_wide_from16.S */
    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH w3, 1                         // w3<- BBBB
    lsr     w2, wINST, #8               // w2<- AA
    GET_VREG_WIDE x3, w3
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x3, w2
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_wide_16: /* 0x06 */
/* File: arm64/op_move_wide_16.S */
    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH w3, 2                         // w3<- BBBB
    FETCH w2, 1                         // w2<- AAAA
    GET_VREG_WIDE x3, w3
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    SET_VREG_WIDE x3, w2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_object: /* 0x07 */
/* File: arm64/op_move_object.S */
/* File: arm64/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    lsr     w1, wINST, #12              // x1<- B from 15:12
    ubfx    w0, wINST, #8, #4           // x0<- A from 11:8
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    GET_VREG w2, w1                     // x2<- fp[B]
    GET_INST_OPCODE ip                  // ip<- opcode from wINST
    .if 1
    SET_VREG_OBJECT w2, w0              // fp[A]<- x2
    .else
    SET_VREG w2, w0                     // fp[A]<- x2
    .endif
    GOTO_OPCODE ip                      // execute next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_object_from16: /* 0x08 */
/* File: arm64/op_move_object_from16.S */
/* File: arm64/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    FETCH w1, 1                         // r1<- BBBB
    lsr     w0, wINST, #8               // r0<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_VREG w2, w1                     // r2<- fp[BBBB]
    GET_INST_OPCODE ip                  // extract opcode from wINST
    .if 1
    SET_VREG_OBJECT w2, w0              // fp[AA]<- r2
    .else
    SET_VREG w2, w0                     // fp[AA]<- r2
    .endif
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_object_16: /* 0x09 */
/* File: arm64/op_move_object_16.S */
/* File: arm64/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    FETCH w1, 2                         // w1<- BBBB
    FETCH w0, 1                         // w0<- AAAA
    FETCH_ADVANCE_INST 3                // advance xPC, load xINST
    GET_VREG w2, w1                     // w2<- fp[BBBB]
    GET_INST_OPCODE ip                  // extract opcode from xINST
    .if 1
    SET_VREG_OBJECT w2, w0              // fp[AAAA]<- w2
    .else
    SET_VREG w2, w0                     // fp[AAAA]<- w2
    .endif
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_result: /* 0x0a */
/* File: arm64/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    lsr     w2, wINST, #8               // r2<- AA
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    ldr     x0, [xFP, #OFF_FP_RESULT_REGISTER]  // get pointer to result JType.
    ldr     w0, [x0]                    // r0 <- result.i.
    GET_INST_OPCODE ip                  // extract opcode from wINST
    .if 0
    SET_VREG_OBJECT w0, w2, w1          // fp[AA]<- r0
    .else
    SET_VREG w0, w2                     // fp[AA]<- r0
    .endif
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_result_wide: /* 0x0b */
/* File: arm64/op_move_result_wide.S */
    /* for: move-result-wide */
    /* op vAA */
    lsr     w2, wINST, #8               // r2<- AA
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    ldr     x0, [xFP, #OFF_FP_RESULT_REGISTER]  // get pointer to result JType.
    ldr     x0, [x0]                    // r0 <- result.i.
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, x2                // fp[AA]<- r0
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_result_object: /* 0x0c */
/* File: arm64/op_move_result_object.S */
/* File: arm64/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    lsr     w2, wINST, #8               // r2<- AA
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    ldr     x0, [xFP, #OFF_FP_RESULT_REGISTER]  // get pointer to result JType.
    ldr     w0, [x0]                    // r0 <- result.i.
    GET_INST_OPCODE ip                  // extract opcode from wINST
    .if 1
    SET_VREG_OBJECT w0, w2, w1          // fp[AA]<- r0
    .else
    SET_VREG w0, w2                     // fp[AA]<- r0
    .endif
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_exception: /* 0x0d */
/* File: arm64/op_move_exception.S */
    /* move-exception vAA */
    lsr     w2, wINST, #8               // w2<- AA
    ldr     x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    mov     x1, #0                      // w1<- 0
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    SET_VREG_OBJECT w3, w2              // fp[AA]<- exception obj
    GET_INST_OPCODE ip                  // extract opcode from rINST
    str     x1, [xSELF, #THREAD_EXCEPTION_OFFSET]  // clear exception
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_return_void: /* 0x0e */
/* File: arm64/op_return_void.S */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .Lop_return_void_check
.Lop_return_void_return:
    mov     x0, #0
    b       MterpReturn
.Lop_return_void_check:
    bl      MterpSuspendCheck           // (self)
    b       .Lop_return_void_return

/* ------------------------------ */
    .balign 128
.L_op_return: /* 0x0f */
/* File: arm64/op_return.S */
    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .Lop_return_check
.Lop_return_return:
    lsr     w2, wINST, #8               // r2<- AA
    GET_VREG w0, w2                     // r0<- vAA
    b       MterpReturn
.Lop_return_check:
    bl      MterpSuspendCheck           // (self)
    b       .Lop_return_return

/* ------------------------------ */
    .balign 128
.L_op_return_wide: /* 0x10 */
/* File: arm64/op_return_wide.S */
    /*
     * Return a 64-bit value.
     */
    /* return-wide vAA */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .Lop_return_wide_check
.Lop_return_wide_return:
    lsr     w2, wINST, #8               // w2<- AA
    GET_VREG_WIDE x0, w2                // x0<- vAA
    b       MterpReturn
.Lop_return_wide_check:
    bl      MterpSuspendCheck           // (self)
    b       .Lop_return_wide_return

/* ------------------------------ */
    .balign 128
.L_op_return_object: /* 0x11 */
/* File: arm64/op_return_object.S */
/* File: arm64/op_return.S */
    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .Lop_return_object_check
.Lop_return_object_return:
    lsr     w2, wINST, #8               // r2<- AA
    GET_VREG w0, w2                     // r0<- vAA
    b       MterpReturn
.Lop_return_object_check:
    bl      MterpSuspendCheck           // (self)
    b       .Lop_return_object_return


/* ------------------------------ */
    .balign 128
.L_op_const_4: /* 0x12 */
/* File: arm64/op_const_4.S */
    /* const/4 vA, #+B */
    lsl     w1, wINST, #16              // w1<- Bxxx0000
    ubfx    w0, wINST, #8, #4           // w0<- A
    FETCH_ADVANCE_INST 1                // advance xPC, load wINST
    asr     w1, w1, #28                 // w1<- sssssssB (sign-extended)
    GET_INST_OPCODE ip                  // ip<- opcode from xINST
    SET_VREG w1, w0                     // fp[A]<- w1
    GOTO_OPCODE ip                      // execute next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_16: /* 0x13 */
/* File: arm64/op_const_16.S */
    /* const/16 vAA, #+BBBB */
    FETCH_S w0, 1                       // w0<- ssssBBBB (sign-extended
    lsr     w3, wINST, #8               // w3<- AA
    FETCH_ADVANCE_INST 2                // advance xPC, load wINST
    SET_VREG w0, w3                     // vAA<- w0
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const: /* 0x14 */
/* File: arm64/op_const.S */
    /* const vAA, #+BBBBbbbb */
    lsr     w3, wINST, #8               // w3<- AA
    FETCH w0, 1                         // w0<- bbbb (low
    FETCH w1, 2                         // w1<- BBBB (high
    FETCH_ADVANCE_INST 3                // advance rPC, load wINST
    orr     w0, w0, w1, lsl #16         // w0<- BBBBbbbb
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG w0, w3                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_high16: /* 0x15 */
/* File: arm64/op_const_high16.S */
    /* const/high16 vAA, #+BBBB0000 */
    FETCH   w0, 1                       // r0<- 0000BBBB (zero-extended
    lsr     w3, wINST, #8               // r3<- AA
    lsl     w0, w0, #16                 // r0<- BBBB0000
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    SET_VREG w0, w3                     // vAA<- r0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide_16: /* 0x16 */
/* File: arm64/op_const_wide_16.S */
    /* const-wide/16 vAA, #+BBBB */
    FETCH_S w0, 1                       // w0<- ssssBBBB (sign-extended
    lsr     w3, wINST, #8               // w3<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    sbfm    x0, x0, 0, 31
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w3
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide_32: /* 0x17 */
/* File: arm64/op_const_wide_32.S */
    /* const-wide/32 vAA, #+BBBBbbbb */
    FETCH w0, 1                         // w0<- 0000bbbb (low)
    lsr     w3, wINST, #8               // w3<- AA
    FETCH_S w2, 2                       // w2<- ssssBBBB (high)
    FETCH_ADVANCE_INST 3                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    orr     w0, w0, w2, lsl #16         // w0<- BBBBbbbb
    sbfm    x0, x0, 0, 31
    SET_VREG_WIDE x0, w3
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide: /* 0x18 */
/* File: arm64/op_const_wide.S */
    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    FETCH w0, 1                         // w0<- bbbb (low)
    FETCH w1, 2                         // w1<- BBBB (low middle)
    FETCH w2, 3                         // w2<- hhhh (high middle)
    FETCH w3, 4                         // w3<- HHHH (high)
    lsr     w4, wINST, #8               // r4<- AA
    FETCH_ADVANCE_INST 5                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    orr     w0, w0, w1, lsl #16         // w0<-         BBBBbbbb
    orr     x0, x0, x2, lsl #32         // w0<-     hhhhBBBBbbbb
    orr     x0, x0, x3, lsl #48         // w0<- HHHHhhhhBBBBbbbb
    SET_VREG_WIDE x0, w4
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide_high16: /* 0x19 */
/* File: arm64/op_const_wide_high16.S */
    /* const-wide/high16 vAA, #+BBBB000000000000 */
    FETCH w0, 1                         // w0<- 0000BBBB (zero-extended)
    lsr     w1, wINST, #8               // w1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    lsl     x0, x0, #48
    SET_VREG_WIDE x0, w1
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_string: /* 0x1a */
/* File: arm64/op_const_string.S */
    /* const/string vAA, String//BBBB */
    EXPORT_PC
    FETCH w0, 1                         // w0<- BBBB
    lsr     w1, wINST, #8               // w1<- AA
    add     x2, xFP, #OFF_FP_SHADOWFRAME
    mov     x3, xSELF
    bl      MterpConstString            // (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2                     // load rINST
    cbnz    w0, MterpPossibleException  // let reference interpreter deal with it.
    ADVANCE 2                           // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_string_jumbo: /* 0x1b */
/* File: arm64/op_const_string_jumbo.S */
    /* const/string vAA, String//BBBBBBBB */
    EXPORT_PC
    FETCH w0, 1                         // w0<- bbbb (low
    FETCH w2, 2                         // w2<- BBBB (high
    lsr     w1, wINST, #8               // w1<- AA
    orr     w0, w0, w2, lsl #16         // w1<- BBBBbbbb
    add     x2, xFP, #OFF_FP_SHADOWFRAME
    mov     x3, xSELF
    bl      MterpConstString            // (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 3                     // advance rPC
    cbnz    w0, MterpPossibleException      // let reference interpreter deal with it.
    ADVANCE 3                           // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_class: /* 0x1c */
/* File: arm64/op_const_class.S */
    /* const/class vAA, Class//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // w0<- BBBB
    lsr     w1, wINST, #8               // w1<- AA
    add     x2, xFP, #OFF_FP_SHADOWFRAME
    mov     x3, xSELF
    bl      MterpConstClass             // (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2
    cbnz    w0, MterpPossibleException
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_monitor_enter: /* 0x1d */
/* File: arm64/op_monitor_enter.S */
    /*
     * Synchronize on an object.
     */
    /* monitor-enter vAA */
    EXPORT_PC
    lsr      w2, wINST, #8               // w2<- AA
    GET_VREG w0, w2                      // w0<- vAA (object)
    mov      x1, xSELF                   // w1<- self
    bl       artLockObjectFromCode
    cbnz     w0, MterpException
    FETCH_ADVANCE_INST 1
    GET_INST_OPCODE ip                   // extract opcode from rINST
    GOTO_OPCODE ip                       // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_monitor_exit: /* 0x1e */
/* File: arm64/op_monitor_exit.S */
    /*
     * Unlock an object.
     *
     * Exceptions that occur when unlocking a monitor need to appear as
     * if they happened at the following instruction.  See the Dalvik
     * instruction spec.
     */
    /* monitor-exit vAA */
    EXPORT_PC
    lsr      w2, wINST, #8              // w2<- AA
    GET_VREG w0, w2                     // w0<- vAA (object)
    mov      x1, xSELF                  // w0<- self
    bl       artUnlockObjectFromCode    // w0<- success for unlock(self, obj)
    cbnz     w0, MterpException
    FETCH_ADVANCE_INST 1                // before throw: advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_check_cast: /* 0x1f */
/* File: arm64/op_check_cast.S */
    /*
     * Check to see if a cast from one class to another is allowed.
     */
    /* check-cast vAA, class//BBBB */
    EXPORT_PC
    FETCH    w0, 1                      // w0<- BBBB
    lsr      w1, wINST, #8              // w1<- AA
    VREG_INDEX_TO_ADDR x1, w1           // w1<- &object
    ldr      x2, [xFP, #OFF_FP_METHOD]  // w2<- method
    mov      x3, xSELF                  // w3<- self
    bl       MterpCheckCast             // (index, &obj, method, self)
    PREFETCH_INST 2
    cbnz     w0, MterpPossibleException
    ADVANCE  2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_instance_of: /* 0x20 */
/* File: arm64/op_instance_of.S */
    /*
     * Check to see if an object reference is an instance of a class.
     *
     * Most common situation is a non-null object, being compared against
     * an already-resolved class.
     */
    /* instance-of vA, vB, class//CCCC */
    EXPORT_PC
    FETCH     w0, 1                     // w0<- CCCC
    lsr       w1, wINST, #12            // w1<- B
    VREG_INDEX_TO_ADDR x1, w1           // w1<- &object
    ldr       x2, [xFP, #OFF_FP_METHOD] // w2<- method
    mov       x3, xSELF                 // w3<- self
    bl        MterpInstanceOf           // (index, &obj, method, self)
    ldr       x1, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr       w2, wINST, #8             // w2<- A+
    and       w2, w2, #15               // w2<- A
    PREFETCH_INST 2
    cbnz      x1, MterpException
    ADVANCE 2                           // advance rPC
    SET_VREG w0, w2                     // vA<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_array_length: /* 0x21 */
/* File: arm64/op_array_length.S */
    /*
     * Return the length of an array.
     */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG w0, w1                     // w0<- vB (object ref)
    cbz     w0, common_errNullObject    // yup, fail
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- array length
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w3, w2                     // vB<- length
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_new_instance: /* 0x22 */
/* File: arm64/op_new_instance.S */
    /*
     * Create a new instance of a class.
     */
    /* new-instance vAA, class//BBBB */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xSELF
    mov     w2, wINST
    bl      MterpNewInstance           // (shadow_frame, self, inst_data)
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 2               // advance rPC, load rINST
    GET_INST_OPCODE ip                 // extract opcode from rINST
    GOTO_OPCODE ip                     // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_new_array: /* 0x23 */
/* File: arm64/op_new_array.S */
    /*
     * Allocate an array of objects, specified with the array class
     * and a count.
     *
     * The verifier guarantees that this is an array class, so we don't
     * check for it here.
     */
    /* new-array vA, vB, class//CCCC */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     w2, wINST
    mov     x3, xSELF
    bl      MterpNewArray
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array: /* 0x24 */
/* File: arm64/op_filled_new_array.S */
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    /* op vB, {vD, vE, vF, vG, vA}, class//CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type//BBBB */
    .extern MterpFilledNewArray
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     x2, xSELF
    bl      MterpFilledNewArray
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array_range: /* 0x25 */
/* File: arm64/op_filled_new_array_range.S */
/* File: arm64/op_filled_new_array.S */
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    /* op vB, {vD, vE, vF, vG, vA}, class//CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type//BBBB */
    .extern MterpFilledNewArrayRange
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     x2, xSELF
    bl      MterpFilledNewArrayRange
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_fill_array_data: /* 0x26 */
/* File: arm64/op_fill_array_data.S */
    /* fill-array-data vAA, +BBBBBBBB */
    EXPORT_PC
    FETCH w0, 1                         // w0<- bbbb (lo)
    FETCH w1, 2                         // w1<- BBBB (hi)
    lsr     w3, wINST, #8               // w3<- AA
    orr     w1, w0, w1, lsl #16         // w1<- BBBBbbbb
    GET_VREG w0, w3                     // w0<- vAA (array object)
    add     x1, xPC, w1, lsl #1         // w1<- PC + BBBBbbbb*2 (array data off.)
    bl      MterpFillArrayData          // (obj, payload)
    cbz     w0, MterpPossibleException      // exception?
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_throw: /* 0x27 */
/* File: arm64/op_throw.S */
    /*
     * Throw an exception object in the current thread.
     */
    /* throw vAA */
    EXPORT_PC
    lsr      w2, wINST, #8               // r2<- AA
    GET_VREG w1, w2                      // r1<- vAA (exception object)
    cbz      w1, common_errNullObject
    str      x1, [xSELF, #THREAD_EXCEPTION_OFFSET]  // thread->exception<- obj
    b        MterpException

/* ------------------------------ */
    .balign 128
.L_op_goto: /* 0x28 */
/* File: arm64/op_goto.S */
    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    sbfx    wINST, wINST, #8, #8           // wINST<- ssssssAA (sign-extended)
    b       MterpCommonTakenBranchNoFlags

/* ------------------------------ */
    .balign 128
.L_op_goto_16: /* 0x29 */
/* File: arm64/op_goto_16.S */
    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
    FETCH_S wINST, 1                    // wINST<- ssssAAAA (sign-extended)
    b       MterpCommonTakenBranchNoFlags

/* ------------------------------ */
    .balign 128
.L_op_goto_32: /* 0x2a */
/* File: arm64/op_goto_32.S */
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
    FETCH w0, 1                         // w0<- aaaa (lo)
    FETCH w1, 2                         // w1<- AAAA (hi)
    orr     wINST, w0, w1, lsl #16      // wINST<- AAAAaaaa
    b       MterpCommonTakenBranchNoFlags

/* ------------------------------ */
    .balign 128
.L_op_packed_switch: /* 0x2b */
/* File: arm64/op_packed_switch.S */
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
    FETCH w0, 1                         // w0<- bbbb (lo)
    FETCH w1, 2                         // w1<- BBBB (hi)
    lsr     w3, wINST, #8               // w3<- AA
    orr     w0, w0, w1, lsl #16         // w0<- BBBBbbbb
    GET_VREG w1, w3                     // w1<- vAA
    add     x0, xPC, w0, lsl #1         // w0<- PC + BBBBbbbb*2
    bl      MterpDoPackedSwitch                       // w0<- code-unit branch offset
    sbfm    xINST, x0, 0, 31
    b       MterpCommonTakenBranchNoFlags

/* ------------------------------ */
    .balign 128
.L_op_sparse_switch: /* 0x2c */
/* File: arm64/op_sparse_switch.S */
/* File: arm64/op_packed_switch.S */
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
    FETCH w0, 1                         // w0<- bbbb (lo)
    FETCH w1, 2                         // w1<- BBBB (hi)
    lsr     w3, wINST, #8               // w3<- AA
    orr     w0, w0, w1, lsl #16         // w0<- BBBBbbbb
    GET_VREG w1, w3                     // w1<- vAA
    add     x0, xPC, w0, lsl #1         // w0<- PC + BBBBbbbb*2
    bl      MterpDoSparseSwitch                       // w0<- code-unit branch offset
    sbfm    xINST, x0, 0, 31
    b       MterpCommonTakenBranchNoFlags


/* ------------------------------ */
    .balign 128
.L_op_cmpl_float: /* 0x2d */
/* File: arm64/op_cmpl_float.S */
/* File: arm64/fcmp.S */
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     */
    /* op vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG s1, w2
    GET_VREG s2, w3
    mov     w0, #-1
    fcmp s1, s2
    csneg w0, w0, w0, le
    csel w0, wzr, w0, eq
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w4                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_cmpg_float: /* 0x2e */
/* File: arm64/op_cmpg_float.S */
/* File: arm64/fcmp.S */
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     */
    /* op vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG s1, w2
    GET_VREG s2, w3
    mov     w0, #1
    fcmp s1, s2
    csneg w0, w0, w0, pl
    csel w0, wzr, w0, eq
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w4                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_cmpl_double: /* 0x2f */
/* File: arm64/op_cmpl_double.S */
/* File: arm64/fcmp.S */
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     */
    /* op vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG_WIDE d1, w2
    GET_VREG_WIDE d2, w3
    mov     w0, #-1
    fcmp d1, d2
    csneg w0, w0, w0, le
    csel w0, wzr, w0, eq
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w4                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_cmpg_double: /* 0x30 */
/* File: arm64/op_cmpg_double.S */
/* File: arm64/fcmp.S */
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     */
    /* op vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG_WIDE d1, w2
    GET_VREG_WIDE d2, w3
    mov     w0, #1
    fcmp d1, d2
    csneg w0, w0, w0, pl
    csel w0, wzr, w0, eq
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w4                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_cmp_long: /* 0x31 */
/* File: arm64/op_cmp_long.S */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG_WIDE x1, w2
    GET_VREG_WIDE x2, w3
    cmp     x1, x2
    csinc   w0, wzr, wzr, eq
    csneg   w0, w0, w0, ge
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    SET_VREG w0, w4
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_if_eq: /* 0x32 */
/* File: arm64/op_if_eq.S */
/* File: arm64/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.eq MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_ne: /* 0x33 */
/* File: arm64/op_if_ne.S */
/* File: arm64/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.ne MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_lt: /* 0x34 */
/* File: arm64/op_if_lt.S */
/* File: arm64/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.lt MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_ge: /* 0x35 */
/* File: arm64/op_if_ge.S */
/* File: arm64/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.ge MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_gt: /* 0x36 */
/* File: arm64/op_if_gt.S */
/* File: arm64/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.gt MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_le: /* 0x37 */
/* File: arm64/op_if_le.S */
/* File: arm64/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.le MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_eqz: /* 0x38 */
/* File: arm64/op_if_eqz.S */
/* File: arm64/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.eq MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_nez: /* 0x39 */
/* File: arm64/op_if_nez.S */
/* File: arm64/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.ne MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_ltz: /* 0x3a */
/* File: arm64/op_if_ltz.S */
/* File: arm64/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.lt MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_gez: /* 0x3b */
/* File: arm64/op_if_gez.S */
/* File: arm64/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.ge MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_gtz: /* 0x3c */
/* File: arm64/op_if_gtz.S */
/* File: arm64/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.gt MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_lez: /* 0x3d */
/* File: arm64/op_if_lez.S */
/* File: arm64/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.le MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_unused_3e: /* 0x3e */
/* File: arm64/op_unused_3e.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_3f: /* 0x3f */
/* File: arm64/op_unused_3f.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_40: /* 0x40 */
/* File: arm64/op_unused_40.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_41: /* 0x41 */
/* File: arm64/op_unused_41.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_42: /* 0x42 */
/* File: arm64/op_unused_42.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_43: /* 0x43 */
/* File: arm64/op_unused_43.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_aget: /* 0x44 */
/* File: arm64/op_aget.S */
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     x0, common_errNullObject    // bail if null array object.
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, uxtw #2    // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    ldr   w2, [x0, #MIRROR_INT_ARRAY_DATA_OFFSET]     // w2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w2, w9                     // vAA<- w2
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aget_wide: /* 0x45 */
/* File: arm64/op_aget_wide.S */
    /*
     * Array get, 64 bits.  vAA <- vBB[vCC].
     *
     */
    /* aget-wide vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject        // yes, bail
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, lsl #3          // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    ldr     x2, [x0, #MIRROR_WIDE_ARRAY_DATA_OFFSET]  // x2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x2, w4
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aget_object: /* 0x46 */
/* File: arm64/op_aget_object.S */
    /*
     * Array object get.  vAA <- vBB[vCC].
     *
     * for: aget-object
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    FETCH_B w3, 1, 1                    // w3<- CC
    EXPORT_PC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    bl       artAGetObjectFromMterp     // (array, index)
    ldr      x1, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr      w2, wINST, #8               // w9<- AA
    PREFETCH_INST 2
    cbnz     w1, MterpException
    SET_VREG_OBJECT w0, w2
    ADVANCE 2
    GET_INST_OPCODE ip
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aget_boolean: /* 0x47 */
/* File: arm64/op_aget_boolean.S */
/* File: arm64/op_aget.S */
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     x0, common_errNullObject    // bail if null array object.
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, uxtw #0    // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    ldrb   w2, [x0, #MIRROR_BOOLEAN_ARRAY_DATA_OFFSET]     // w2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w2, w9                     // vAA<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aget_byte: /* 0x48 */
/* File: arm64/op_aget_byte.S */
/* File: arm64/op_aget.S */
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     x0, common_errNullObject    // bail if null array object.
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, uxtw #0    // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    ldrsb   w2, [x0, #MIRROR_BYTE_ARRAY_DATA_OFFSET]     // w2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w2, w9                     // vAA<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aget_char: /* 0x49 */
/* File: arm64/op_aget_char.S */
/* File: arm64/op_aget.S */
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     x0, common_errNullObject    // bail if null array object.
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, uxtw #1    // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    ldrh   w2, [x0, #MIRROR_CHAR_ARRAY_DATA_OFFSET]     // w2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w2, w9                     // vAA<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aget_short: /* 0x4a */
/* File: arm64/op_aget_short.S */
/* File: arm64/op_aget.S */
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     x0, common_errNullObject    // bail if null array object.
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, uxtw #1    // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    ldrsh   w2, [x0, #MIRROR_SHORT_ARRAY_DATA_OFFSET]     // w2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w2, w9                     // vAA<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput: /* 0x4b */
/* File: arm64/op_aput.S */
    /*
     * Array put, 32 bits or less.  vBB[vCC] <- vAA.
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aput, aput-boolean, aput-byte, aput-char, aput-short
     *
     * NOTE: this assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]     // w3<- arrayObj->length
    add     x0, x0, w1, lsl #2     // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_VREG w2, w9                     // w2<- vAA
    GET_INST_OPCODE ip                  // extract opcode from rINST
    str  w2, [x0, #MIRROR_INT_ARRAY_DATA_OFFSET]     // vBB[vCC]<- w2
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aput_wide: /* 0x4c */
/* File: arm64/op_aput_wide.S */
    /*
     * Array put, 64 bits.  vBB[vCC] <- vAA.
     *
     */
    /* aput-wide vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, lsl #3          // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    GET_VREG_WIDE x1, w4
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    str     x1, [x0, #MIRROR_WIDE_ARRAY_DATA_OFFSET]
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aput_object: /* 0x4d */
/* File: arm64/op_aput_object.S */
    /*
     * Store an object into an array.  vBB[vCC] <- vAA.
     */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     w2, wINST
    bl      MterpAputObject
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aput_boolean: /* 0x4e */
/* File: arm64/op_aput_boolean.S */
/* File: arm64/op_aput.S */
    /*
     * Array put, 32 bits or less.  vBB[vCC] <- vAA.
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aput, aput-boolean, aput-byte, aput-char, aput-short
     *
     * NOTE: this assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]     // w3<- arrayObj->length
    add     x0, x0, w1, lsl #0     // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_VREG w2, w9                     // w2<- vAA
    GET_INST_OPCODE ip                  // extract opcode from rINST
    strb  w2, [x0, #MIRROR_BOOLEAN_ARRAY_DATA_OFFSET]     // vBB[vCC]<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput_byte: /* 0x4f */
/* File: arm64/op_aput_byte.S */
/* File: arm64/op_aput.S */
    /*
     * Array put, 32 bits or less.  vBB[vCC] <- vAA.
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aput, aput-boolean, aput-byte, aput-char, aput-short
     *
     * NOTE: this assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]     // w3<- arrayObj->length
    add     x0, x0, w1, lsl #0     // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_VREG w2, w9                     // w2<- vAA
    GET_INST_OPCODE ip                  // extract opcode from rINST
    strb  w2, [x0, #MIRROR_BYTE_ARRAY_DATA_OFFSET]     // vBB[vCC]<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput_char: /* 0x50 */
/* File: arm64/op_aput_char.S */
/* File: arm64/op_aput.S */
    /*
     * Array put, 32 bits or less.  vBB[vCC] <- vAA.
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aput, aput-boolean, aput-byte, aput-char, aput-short
     *
     * NOTE: this assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]     // w3<- arrayObj->length
    add     x0, x0, w1, lsl #1     // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_VREG w2, w9                     // w2<- vAA
    GET_INST_OPCODE ip                  // extract opcode from rINST
    strh  w2, [x0, #MIRROR_CHAR_ARRAY_DATA_OFFSET]     // vBB[vCC]<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput_short: /* 0x51 */
/* File: arm64/op_aput_short.S */
/* File: arm64/op_aput.S */
    /*
     * Array put, 32 bits or less.  vBB[vCC] <- vAA.
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aput, aput-boolean, aput-byte, aput-char, aput-short
     *
     * NOTE: this assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]     // w3<- arrayObj->length
    add     x0, x0, w1, lsl #1     // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_VREG w2, w9                     // w2<- vAA
    GET_INST_OPCODE ip                  // extract opcode from rINST
    strh  w2, [x0, #MIRROR_SHORT_ARRAY_DATA_OFFSET]     // vBB[vCC]<- w2
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget: /* 0x52 */
/* File: arm64/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGet32InstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if 0
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_wide: /* 0x53 */
/* File: arm64/op_iget_wide.S */
    /*
     * 64-bit instance field get.
     *
     * for: iget-wide
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGet64InstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cmp      w3, #0
    cbnz     w3, MterpException            // bail out
    SET_VREG_WIDE x0, w2
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from wINST
    GOTO_OPCODE ip                         // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_object: /* 0x54 */
/* File: arm64/op_iget_object.S */
/* File: arm64/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGetObjInstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if 1
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean: /* 0x55 */
/* File: arm64/op_iget_boolean.S */
/* File: arm64/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGetBooleanInstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    uxtb w0, w0
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if 0
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_byte: /* 0x56 */
/* File: arm64/op_iget_byte.S */
/* File: arm64/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGetByteInstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    sxtb w0, w0
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if 0
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_char: /* 0x57 */
/* File: arm64/op_iget_char.S */
/* File: arm64/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGetCharInstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    uxth w0, w0
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if 0
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_short: /* 0x58 */
/* File: arm64/op_iget_short.S */
/* File: arm64/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGetShortInstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    sxth w0, w0
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if 0
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput: /* 0x59 */
/* File: arm64/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field//CCCC */
    .extern artSet32InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    GET_VREG w2, w2                     // w2<- fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet32InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_wide: /* 0x5a */
/* File: arm64/op_iput_wide.S */
    /* iput-wide vA, vB, field//CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    VREG_INDEX_TO_ADDR x2, x2           // w2<- &fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet64InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_object: /* 0x5b */
/* File: arm64/op_iput_object.S */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     w2, wINST
    mov     x3, xSELF
    bl      MterpIputObject
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_boolean: /* 0x5c */
/* File: arm64/op_iput_boolean.S */
/* File: arm64/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field//CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    GET_VREG w2, w2                     // w2<- fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet8InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_byte: /* 0x5d */
/* File: arm64/op_iput_byte.S */
/* File: arm64/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field//CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    GET_VREG w2, w2                     // w2<- fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet8InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_char: /* 0x5e */
/* File: arm64/op_iput_char.S */
/* File: arm64/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field//CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    GET_VREG w2, w2                     // w2<- fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet16InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_short: /* 0x5f */
/* File: arm64/op_iput_short.S */
/* File: arm64/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field//CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    GET_VREG w2, w2                     // w2<- fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet16InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sget: /* 0x60 */
/* File: arm64/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern artGet32StaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGet32StaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if 0
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip

/* ------------------------------ */
    .balign 128
.L_op_sget_wide: /* 0x61 */
/* File: arm64/op_sget_wide.S */
    /*
     * SGET_WIDE handler wrapper.
     *
     */
    /* sget-wide vAA, field//BBBB */

    .extern artGet64StaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGet64StaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w4, wINST, #8                 // w4<- AA
    cbnz  x3, MterpException            // bail out
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    SET_VREG_WIDE x0, w4
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sget_object: /* 0x62 */
/* File: arm64/op_sget_object.S */
/* File: arm64/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern artGetObjStaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGetObjStaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if 1
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_boolean: /* 0x63 */
/* File: arm64/op_sget_boolean.S */
/* File: arm64/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern artGetBooleanStaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGetBooleanStaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    uxtb w0, w0
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if 0
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_byte: /* 0x64 */
/* File: arm64/op_sget_byte.S */
/* File: arm64/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern artGetByteStaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGetByteStaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    sxtb w0, w0
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if 0
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_char: /* 0x65 */
/* File: arm64/op_sget_char.S */
/* File: arm64/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern artGetCharStaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGetCharStaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    uxth w0, w0
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if 0
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_short: /* 0x66 */
/* File: arm64/op_sget_short.S */
/* File: arm64/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern artGetShortStaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGetShortStaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    sxth w0, w0
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if 0
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sput: /* 0x67 */
/* File: arm64/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // r0<- field ref BBBB
    lsr     w3, wINST, #8               // r3<- AA
    GET_VREG w1, w3                     // r1<= fp[AA]
    ldr     x2, [xFP, #OFF_FP_METHOD]
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet32StaticFromCode
    cbnz    w0, MterpException          // 0 on success
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sput_wide: /* 0x68 */
/* File: arm64/op_sput_wide.S */
    /*
     * SPUT_WIDE handler wrapper.
     *
     */
    /* sput-wide vAA, field//BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC
    FETCH   w0, 1                       // w0<- field ref BBBB
    ldr     x1, [xFP, #OFF_FP_METHOD]
    lsr     w2, wINST, #8               // w3<- AA
    VREG_INDEX_TO_ADDR x2, w2
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet64IndirectStaticFromMterp
    cbnz    w0, MterpException          // 0 on success, -1 on failure
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sput_object: /* 0x69 */
/* File: arm64/op_sput_object.S */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     x2, xINST
    mov     x3, xSELF
    bl      MterpSputObject
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sput_boolean: /* 0x6a */
/* File: arm64/op_sput_boolean.S */
/* File: arm64/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // r0<- field ref BBBB
    lsr     w3, wINST, #8               // r3<- AA
    GET_VREG w1, w3                     // r1<= fp[AA]
    ldr     x2, [xFP, #OFF_FP_METHOD]
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet8StaticFromCode
    cbnz    w0, MterpException          // 0 on success
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sput_byte: /* 0x6b */
/* File: arm64/op_sput_byte.S */
/* File: arm64/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // r0<- field ref BBBB
    lsr     w3, wINST, #8               // r3<- AA
    GET_VREG w1, w3                     // r1<= fp[AA]
    ldr     x2, [xFP, #OFF_FP_METHOD]
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet8StaticFromCode
    cbnz    w0, MterpException          // 0 on success
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sput_char: /* 0x6c */
/* File: arm64/op_sput_char.S */
/* File: arm64/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // r0<- field ref BBBB
    lsr     w3, wINST, #8               // r3<- AA
    GET_VREG w1, w3                     // r1<= fp[AA]
    ldr     x2, [xFP, #OFF_FP_METHOD]
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet16StaticFromCode
    cbnz    w0, MterpException          // 0 on success
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sput_short: /* 0x6d */
/* File: arm64/op_sput_short.S */
/* File: arm64/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // r0<- field ref BBBB
    lsr     w3, wINST, #8               // r3<- AA
    GET_VREG w1, w3                     // r1<= fp[AA]
    ldr     x2, [xFP, #OFF_FP_METHOD]
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet16StaticFromCode
    cbnz    w0, MterpException          // 0 on success
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual: /* 0x6e */
/* File: arm64/op_invoke_virtual.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtual
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeVirtual
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip


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
/* File: arm64/op_invoke_super.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuper
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeSuper
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip


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
/* File: arm64/op_invoke_direct.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirect
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeDirect
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_static: /* 0x71 */
/* File: arm64/op_invoke_static.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStatic
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeStatic
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip




/* ------------------------------ */
    .balign 128
.L_op_invoke_interface: /* 0x72 */
/* File: arm64/op_invoke_interface.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterface
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeInterface
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip


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
/* File: arm64/op_return_void_no_barrier.S */
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .Lop_return_void_no_barrier_check
.Lop_return_void_no_barrier_return:
    mov     x0, #0
    b       MterpReturn
.Lop_return_void_no_barrier_check:
    bl      MterpSuspendCheck           // (self)
    b       .Lop_return_void_no_barrier_return

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range: /* 0x74 */
/* File: arm64/op_invoke_virtual_range.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualRange
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeVirtualRange
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_super_range: /* 0x75 */
/* File: arm64/op_invoke_super_range.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuperRange
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeSuperRange
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_direct_range: /* 0x76 */
/* File: arm64/op_invoke_direct_range.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirectRange
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeDirectRange
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_static_range: /* 0x77 */
/* File: arm64/op_invoke_static_range.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStaticRange
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeStaticRange
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_interface_range: /* 0x78 */
/* File: arm64/op_invoke_interface_range.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterfaceRange
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeInterfaceRange
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_unused_79: /* 0x79 */
/* File: arm64/op_unused_79.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_7a: /* 0x7a */
/* File: arm64/op_unused_7a.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_neg_int: /* 0x7b */
/* File: arm64/op_neg_int.S */
/* File: arm64/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
                               // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    sub     w0, wzr, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_not_int: /* 0x7c */
/* File: arm64/op_not_int.S */
/* File: arm64/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
                               // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    mvn     w0, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_neg_long: /* 0x7d */
/* File: arm64/op_neg_long.S */
/* File: arm64/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op x0".
     *
     * For: neg-long, not-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w4, wINST, #8, #4           // w4<- A
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    
    sub x0, xzr, x0
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-11 instructions */


/* ------------------------------ */
    .balign 128
.L_op_not_long: /* 0x7e */
/* File: arm64/op_not_long.S */
/* File: arm64/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op x0".
     *
     * For: neg-long, not-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w4, wINST, #8, #4           // w4<- A
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    
    mvn     x0, x0
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-11 instructions */


/* ------------------------------ */
    .balign 128
.L_op_neg_float: /* 0x7f */
/* File: arm64/op_neg_float.S */
/* File: arm64/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
    mov w4, #0x80000000                           // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    add     w0, w0, w4                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_neg_double: /* 0x80 */
/* File: arm64/op_neg_double.S */
/* File: arm64/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op x0".
     *
     * For: neg-long, not-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w4, wINST, #8, #4           // w4<- A
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    mov x1, #0x8000000000000000
    add     x0, x0, x1
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-11 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_long: /* 0x81 */
/* File: arm64/op_int_to_long.S */
/* File: arm64/funopWider.S */
    /*
     * Generic 32bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "x0 = op w0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG w0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    sbfm x0, x0, 0, 31                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4           // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_int_to_float: /* 0x82 */
/* File: arm64/op_int_to_float.S */
/* File: arm64/funopNarrow.S */
    /*
     * Generic 32bit-to-32bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "s0 = op w0".
     *
     * For: int-to-float, float-to-int
     * TODO: refactor all of the conversions - parameterize width and use same template.
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG w0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    scvtf s0, w0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG s0, w4                // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_int_to_double: /* 0x83 */
/* File: arm64/op_int_to_double.S */
/* File: arm64/funopWider.S */
    /*
     * Generic 32bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "d0 = op w0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG w0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    scvtf d0, w0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE d0, w4           // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_long_to_int: /* 0x84 */
/* File: arm64/op_long_to_int.S */
/* File: arm64/funopNarrower.S */
    /*
     * Generic 64bit-to-32bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "w0 = op x0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
                                  // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG w0, w4                // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_long_to_float: /* 0x85 */
/* File: arm64/op_long_to_float.S */
/* File: arm64/funopNarrower.S */
    /*
     * Generic 64bit-to-32bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "s0 = op x0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    scvtf s0, x0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG s0, w4                // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_long_to_double: /* 0x86 */
/* File: arm64/op_long_to_double.S */
/* File: arm64/funopWide.S */
    /*
     * Generic 64bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "d0 = op x0".
     *
     * For: long-to-double, double-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    scvtf d0, x0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE d0, w4           // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_float_to_int: /* 0x87 */
/* File: arm64/op_float_to_int.S */
/* File: arm64/funopNarrow.S */
    /*
     * Generic 32bit-to-32bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "w0 = op s0".
     *
     * For: int-to-float, float-to-int
     * TODO: refactor all of the conversions - parameterize width and use same template.
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG s0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    fcvtzs w0, s0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG w0, w4                // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_float_to_long: /* 0x88 */
/* File: arm64/op_float_to_long.S */
/* File: arm64/funopWider.S */
    /*
     * Generic 32bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "x0 = op s0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG s0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    fcvtzs x0, s0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4           // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_float_to_double: /* 0x89 */
/* File: arm64/op_float_to_double.S */
/* File: arm64/funopWider.S */
    /*
     * Generic 32bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "d0 = op s0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG s0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    fcvt  d0, s0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE d0, w4           // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_double_to_int: /* 0x8a */
/* File: arm64/op_double_to_int.S */
/* File: arm64/funopNarrower.S */
    /*
     * Generic 64bit-to-32bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "w0 = op d0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG_WIDE d0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    fcvtzs w0, d0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG w0, w4                // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_double_to_long: /* 0x8b */
/* File: arm64/op_double_to_long.S */
/* File: arm64/funopWide.S */
    /*
     * Generic 64bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "x0 = op d0".
     *
     * For: long-to-double, double-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG_WIDE d0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    fcvtzs x0, d0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4           // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_double_to_float: /* 0x8c */
/* File: arm64/op_double_to_float.S */
/* File: arm64/funopNarrower.S */
    /*
     * Generic 64bit-to-32bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "s0 = op d0".
     *
     * For: int-to-double, float-to-double, float-to-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w4, wINST, #8               // w4<- A+
    GET_VREG_WIDE d0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    and     w4, w4, #15                 // w4<- A
    fcvt s0, d0                              // d0<- op
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG s0, w4                // vA<- d0
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_int_to_byte: /* 0x8d */
/* File: arm64/op_int_to_byte.S */
/* File: arm64/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
                               // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    sxtb    w0, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_char: /* 0x8e */
/* File: arm64/op_int_to_char.S */
/* File: arm64/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
                               // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    uxth    w0, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_short: /* 0x8f */
/* File: arm64/op_int_to_short.S */
/* File: arm64/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
                               // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    sxth    w0, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_int: /* 0x90 */
/* File: arm64/op_add_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    add     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_int: /* 0x91 */
/* File: arm64/op_sub_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    sub     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int: /* 0x92 */
/* File: arm64/op_mul_int.S */
/* must be "mul w0, w1, w0" -- "w0, w0, w1" is illegal */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    mul     w0, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int: /* 0x93 */
/* File: arm64/op_div_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 1
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    sdiv     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_int: /* 0x94 */
/* File: arm64/op_rem_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 1
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    sdiv     w2, w0, w1                           // optional op; may set condition codes
    msub w0, w2, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_int: /* 0x95 */
/* File: arm64/op_and_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    and     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int: /* 0x96 */
/* File: arm64/op_or_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    orr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int: /* 0x97 */
/* File: arm64/op_xor_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    eor     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_int: /* 0x98 */
/* File: arm64/op_shl_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    lsl     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_int: /* 0x99 */
/* File: arm64/op_shr_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    asr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_int: /* 0x9a */
/* File: arm64/op_ushr_int.S */
/* File: arm64/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if 0
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    lsr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_long: /* 0x9b */
/* File: arm64/op_add_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 0
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    add x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_long: /* 0x9c */
/* File: arm64/op_sub_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 0
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    sub x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_long: /* 0x9d */
/* File: arm64/op_mul_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 0
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    mul x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_long: /* 0x9e */
/* File: arm64/op_div_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 1
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    sdiv x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_long: /* 0x9f */
/* File: arm64/op_rem_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 1
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    sdiv x3, x1, x2
    msub x0, x3, x2, x1                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_long: /* 0xa0 */
/* File: arm64/op_and_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 0
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    and x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_long: /* 0xa1 */
/* File: arm64/op_or_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 0
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    orr x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_long: /* 0xa2 */
/* File: arm64/op_xor_long.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x2, w2               // w2<- vCC
    GET_VREG_WIDE x1, w1               // w1<- vBB
    .if 0
    cbz     x2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    eor x0, x1, x2                              // x0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w4           // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_long: /* 0xa3 */
/* File: arm64/op_shl_long.S */
/* File: arm64/shiftWide.S */
    /*
     * 64-bit shift operation.
     *
     * For: shl-long, shr-long, ushr-long
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr      w3, wINST, #8               // w3<- AA
    lsr      w2, w0, #8                  // w2<- CC
    GET_VREG w2, w2                     // w2<- vCC (shift count)
    and      w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x1, w1                // x1<- vBB
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and      x2, x2, #63                 // Mask low 6
    lsl  x0, x1, x2                 // Do the shift.
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w3                // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_long: /* 0xa4 */
/* File: arm64/op_shr_long.S */
/* File: arm64/shiftWide.S */
    /*
     * 64-bit shift operation.
     *
     * For: shl-long, shr-long, ushr-long
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr      w3, wINST, #8               // w3<- AA
    lsr      w2, w0, #8                  // w2<- CC
    GET_VREG w2, w2                     // w2<- vCC (shift count)
    and      w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x1, w1                // x1<- vBB
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and      x2, x2, #63                 // Mask low 6
    asr  x0, x1, x2                 // Do the shift.
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w3                // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_long: /* 0xa5 */
/* File: arm64/op_ushr_long.S */
/* File: arm64/shiftWide.S */
    /*
     * 64-bit shift operation.
     *
     * For: shl-long, shr-long, ushr-long
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr      w3, wINST, #8               // w3<- AA
    lsr      w2, w0, #8                  // w2<- CC
    GET_VREG w2, w2                     // w2<- vCC (shift count)
    and      w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x1, w1                // x1<- vBB
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and      x2, x2, #63                 // Mask low 6
    lsr  x0, x1, x2                 // Do the shift.
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w3                // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_float: /* 0xa6 */
/* File: arm64/op_add_float.S */
/* File: arm64/fbinop.S */
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float
     * form: <op> s0, s0, s1
     */
    /* floatop vAA, vBB, vCC */
    FETCH w0, 1                         // r0<- CCBB
    lsr     w1, w0, #8                  // r2<- CC
    and     w0, w0, #255                // r1<- BB
    GET_VREG  s1, w1
    GET_VREG  s0, w0
    fadd   s0, s0, s1                              // s0<- op
    lsr     w1, wINST, #8               // r1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG  s0, w1
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sub_float: /* 0xa7 */
/* File: arm64/op_sub_float.S */
/* File: arm64/fbinop.S */
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float
     * form: <op> s0, s0, s1
     */
    /* floatop vAA, vBB, vCC */
    FETCH w0, 1                         // r0<- CCBB
    lsr     w1, w0, #8                  // r2<- CC
    and     w0, w0, #255                // r1<- BB
    GET_VREG  s1, w1
    GET_VREG  s0, w0
    fsub   s0, s0, s1                              // s0<- op
    lsr     w1, wINST, #8               // r1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG  s0, w1
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_mul_float: /* 0xa8 */
/* File: arm64/op_mul_float.S */
/* File: arm64/fbinop.S */
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float
     * form: <op> s0, s0, s1
     */
    /* floatop vAA, vBB, vCC */
    FETCH w0, 1                         // r0<- CCBB
    lsr     w1, w0, #8                  // r2<- CC
    and     w0, w0, #255                // r1<- BB
    GET_VREG  s1, w1
    GET_VREG  s0, w0
    fmul   s0, s0, s1                              // s0<- op
    lsr     w1, wINST, #8               // r1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG  s0, w1
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_div_float: /* 0xa9 */
/* File: arm64/op_div_float.S */
/* File: arm64/fbinop.S */
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float
     * form: <op> s0, s0, s1
     */
    /* floatop vAA, vBB, vCC */
    FETCH w0, 1                         // r0<- CCBB
    lsr     w1, w0, #8                  // r2<- CC
    and     w0, w0, #255                // r1<- BB
    GET_VREG  s1, w1
    GET_VREG  s0, w0
    fdiv   s0, s0, s1                              // s0<- op
    lsr     w1, wINST, #8               // r1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG  s0, w1
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_rem_float: /* 0xaa */
/* File: arm64/op_rem_float.S */
/* EABI doesn't define a float remainder function, but libm does */
/* File: arm64/fbinop.S */
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float
     * form: <op> s0, s0, s1
     */
    /* floatop vAA, vBB, vCC */
    FETCH w0, 1                         // r0<- CCBB
    lsr     w1, w0, #8                  // r2<- CC
    and     w0, w0, #255                // r1<- BB
    GET_VREG  s1, w1
    GET_VREG  s0, w0
    bl      fmodf                              // s0<- op
    lsr     w1, wINST, #8               // r1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG  s0, w1
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_add_double: /* 0xab */
/* File: arm64/op_add_double.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE d2, w2               // w2<- vCC
    GET_VREG_WIDE d1, w1               // w1<- vBB
    .if 0
    cbz     d2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    fadd d0, d1, d2                              // d0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w4           // vAA<- d0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_double: /* 0xac */
/* File: arm64/op_sub_double.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE d2, w2               // w2<- vCC
    GET_VREG_WIDE d1, w1               // w1<- vBB
    .if 0
    cbz     d2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    fsub d0, d1, d2                              // d0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w4           // vAA<- d0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_double: /* 0xad */
/* File: arm64/op_mul_double.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE d2, w2               // w2<- vCC
    GET_VREG_WIDE d1, w1               // w1<- vBB
    .if 0
    cbz     d2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    fmul d0, d1, d2                              // d0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w4           // vAA<- d0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_double: /* 0xae */
/* File: arm64/op_div_double.S */
/* File: arm64/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE d2, w2               // w2<- vCC
    GET_VREG_WIDE d1, w1               // w1<- vBB
    .if 0
    cbz     d2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    fdiv d0, d1, d2                              // d0<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w4           // vAA<- d0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_double: /* 0xaf */
/* File: arm64/op_rem_double.S */
    /* rem vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE d1, w2                // d1<- vCC
    GET_VREG_WIDE d0, w1                // d0<- vBB
    bl  fmod
    lsr     w4, wINST, #8               // w4<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w4                // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */

/* ------------------------------ */
    .balign 128
.L_op_add_int_2addr: /* 0xb0 */
/* File: arm64/op_add_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    add     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_int_2addr: /* 0xb1 */
/* File: arm64/op_sub_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    sub     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int_2addr: /* 0xb2 */
/* File: arm64/op_mul_int_2addr.S */
/* must be "mul w0, w1, w0" -- "w0, w0, w1" is illegal */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    mul     w0, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int_2addr: /* 0xb3 */
/* File: arm64/op_div_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 1
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    sdiv     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_int_2addr: /* 0xb4 */
/* File: arm64/op_rem_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 1
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    sdiv     w2, w0, w1                           // optional op; may set condition codes
    msub w0, w2, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_int_2addr: /* 0xb5 */
/* File: arm64/op_and_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    and     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int_2addr: /* 0xb6 */
/* File: arm64/op_or_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    orr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int_2addr: /* 0xb7 */
/* File: arm64/op_xor_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
                               // optional op; may set condition codes
    eor     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_int_2addr: /* 0xb8 */
/* File: arm64/op_shl_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    lsl     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_int_2addr: /* 0xb9 */
/* File: arm64/op_shr_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    asr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_2addr: /* 0xba */
/* File: arm64/op_ushr_int_2addr.S */
/* File: arm64/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    lsr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_long_2addr: /* 0xbb */
/* File: arm64/op_add_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 0
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    add     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_long_2addr: /* 0xbc */
/* File: arm64/op_sub_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 0
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    sub     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_long_2addr: /* 0xbd */
/* File: arm64/op_mul_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 0
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    mul     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_long_2addr: /* 0xbe */
/* File: arm64/op_div_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 1
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    sdiv     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_long_2addr: /* 0xbf */
/* File: arm64/op_rem_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 1
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    sdiv x3, x0, x1
    msub x0, x3, x1, x0                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_long_2addr: /* 0xc0 */
/* File: arm64/op_and_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 0
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    and     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_long_2addr: /* 0xc1 */
/* File: arm64/op_or_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 0
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    orr     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_long_2addr: /* 0xc2 */
/* File: arm64/op_xor_long_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE x1, w1               // x1<- vB
    GET_VREG_WIDE x0, w2               // x0<- vA
    .if 0
    cbz     x1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    eor     x0, x0, x1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_long_2addr: /* 0xc3 */
/* File: arm64/op_shl_long_2addr.S */
/* File: arm64/shiftWide2addr.S */
    /*
     * Generic 64-bit shift operation.
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG w1, w1                     // x1<- vB
    GET_VREG_WIDE x0, w2                // x0<- vA
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     x1, x1, #63                 // Mask low 6 bits.
    lsl x0, x0, x1
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_long_2addr: /* 0xc4 */
/* File: arm64/op_shr_long_2addr.S */
/* File: arm64/shiftWide2addr.S */
    /*
     * Generic 64-bit shift operation.
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG w1, w1                     // x1<- vB
    GET_VREG_WIDE x0, w2                // x0<- vA
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     x1, x1, #63                 // Mask low 6 bits.
    asr x0, x0, x1
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_long_2addr: /* 0xc5 */
/* File: arm64/op_ushr_long_2addr.S */
/* File: arm64/shiftWide2addr.S */
    /*
     * Generic 64-bit shift operation.
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG w1, w1                     // x1<- vB
    GET_VREG_WIDE x0, w2                // x0<- vA
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     x1, x1, #63                 // Mask low 6 bits.
    lsr x0, x0, x1
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_float_2addr: /* 0xc6 */
/* File: arm64/op_add_float_2addr.S */
/* File: arm64/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    GET_VREG s1, w3
    GET_VREG s0, w9
    fadd   s2, s0, s1                              // s2<- op
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG s2, w9
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sub_float_2addr: /* 0xc7 */
/* File: arm64/op_sub_float_2addr.S */
/* File: arm64/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    GET_VREG s1, w3
    GET_VREG s0, w9
    fsub   s2, s0, s1                              // s2<- op
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG s2, w9
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_mul_float_2addr: /* 0xc8 */
/* File: arm64/op_mul_float_2addr.S */
/* File: arm64/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    GET_VREG s1, w3
    GET_VREG s0, w9
    fmul   s2, s0, s1                              // s2<- op
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG s2, w9
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_div_float_2addr: /* 0xc9 */
/* File: arm64/op_div_float_2addr.S */
/* File: arm64/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    GET_VREG s1, w3
    GET_VREG s0, w9
    fdiv   s2, s0, s1                              // s2<- op
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG s2, w9
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_rem_float_2addr: /* 0xca */
/* File: arm64/op_rem_float_2addr.S */
    /* rem vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    GET_VREG s1, w3
    GET_VREG s0, w9
    bl  fmodf
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG s0, w9
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_add_double_2addr: /* 0xcb */
/* File: arm64/op_add_double_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE d1, w1               // x1<- vB
    GET_VREG_WIDE d0, w2               // x0<- vA
    .if 0
    cbz     d1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    fadd     d0, d0, d1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_double_2addr: /* 0xcc */
/* File: arm64/op_sub_double_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE d1, w1               // x1<- vB
    GET_VREG_WIDE d0, w2               // x0<- vA
    .if 0
    cbz     d1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    fsub     d0, d0, d1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_double_2addr: /* 0xcd */
/* File: arm64/op_mul_double_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE d1, w1               // x1<- vB
    GET_VREG_WIDE d0, w2               // x0<- vA
    .if 0
    cbz     d1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    fmul     d0, d0, d1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_double_2addr: /* 0xce */
/* File: arm64/op_div_double_2addr.S */
/* File: arm64/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE d1, w1               // x1<- vB
    GET_VREG_WIDE d0, w2               // x0<- vA
    .if 0
    cbz     d1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    
    fdiv     d0, d0, d1                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_double_2addr: /* 0xcf */
/* File: arm64/op_rem_double_2addr.S */
    /* rem vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE d1, w1                // d1<- vB
    GET_VREG_WIDE d0, w2                // d0<- vA
    bl fmod
    ubfx    w2, wINST, #8, #4           // w2<- A (need to reload - killed across call)
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w2                // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */

/* ------------------------------ */
    .balign 128
.L_op_add_int_lit16: /* 0xd0 */
/* File: arm64/op_add_int_lit16.S */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    add     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rsub_int: /* 0xd1 */
/* File: arm64/op_rsub_int.S */
/* this op is "rsub-int", but can be thought of as "rsub-int/lit16" */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    sub     w0, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit16: /* 0xd2 */
/* File: arm64/op_mul_int_lit16.S */
/* must be "mul w0, w1, w0" -- "w0, w0, w1" is illegal */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    mul     w0, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int_lit16: /* 0xd3 */
/* File: arm64/op_div_int_lit16.S */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 1
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    sdiv w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit16: /* 0xd4 */
/* File: arm64/op_rem_int_lit16.S */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 1
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    sdiv w3, w0, w1
    msub w0, w3, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_int_lit16: /* 0xd5 */
/* File: arm64/op_and_int_lit16.S */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    and     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit16: /* 0xd6 */
/* File: arm64/op_or_int_lit16.S */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    orr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit16: /* 0xd7 */
/* File: arm64/op_xor_int_lit16.S */
/* File: arm64/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    eor     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_int_lit8: /* 0xd8 */
/* File: arm64/op_add_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    add     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rsub_int_lit8: /* 0xd9 */
/* File: arm64/op_rsub_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    sub     w0, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit8: /* 0xda */
/* File: arm64/op_mul_int_lit8.S */
/* must be "mul w0, w1, w0" -- "w0, w0, w1" is illegal */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    mul     w0, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int_lit8: /* 0xdb */
/* File: arm64/op_div_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 1
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    sdiv     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit8: /* 0xdc */
/* File: arm64/op_rem_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 1
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    sdiv w3, w0, w1                           // optional op; may set condition codes
    msub w0, w3, w1, w0                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_int_lit8: /* 0xdd */
/* File: arm64/op_and_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    and     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit8: /* 0xde */
/* File: arm64/op_or_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    orr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit8: /* 0xdf */
/* File: arm64/op_xor_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
                               // optional op; may set condition codes
    eor     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_int_lit8: /* 0xe0 */
/* File: arm64/op_shl_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    lsl     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_int_lit8: /* 0xe1 */
/* File: arm64/op_shr_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    asr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_lit8: /* 0xe2 */
/* File: arm64/op_ushr_int_lit8.S */
/* File: arm64/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if 0
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and     w1, w1, #31                           // optional op; may set condition codes
    lsr     w0, w0, w1                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_iget_quick: /* 0xe3 */
/* File: arm64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     x3, #0                      // check object for null
    beq     common_errNullObject        // object was null
    ldr   w0, [x3, x1]                // w0<- obj.field
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    SET_VREG w0, w2                     // fp[A]<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_wide_quick: /* 0xe4 */
/* File: arm64/op_iget_wide_quick.S */
    /* iget-wide-quick vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w4, 1                         // w4<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cbz     w3, common_errNullObject        // object was null
    add     x4, x3, x4                  // create direct pointer
    ldr     x0, [x4]
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    SET_VREG_WIDE x0, w2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_object_quick: /* 0xe5 */
/* File: arm64/op_iget_object_quick.S */
    /* For: iget-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    EXPORT_PC
    GET_VREG w0, w2                     // w0<- object we're operating on
    bl      artIGetObjectFromMterp      // (obj, offset)
    ldr     x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx    w2, wINST, #8, #4           // w2<- A
    PREFETCH_INST 2
    cbnz    w3, MterpPossibleException      // bail out
    SET_VREG_OBJECT w0, w2              // fp[A]<- w0
    ADVANCE 2                           // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_quick: /* 0xe6 */
/* File: arm64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- fp[B], the object pointer
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     w3, #0                      // check object for null
    cbz     w3, common_errNullObject    // object was null
    GET_VREG w0, w2                     // w0<- fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    str     w0, [x3, x1]             // obj.field<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_wide_quick: /* 0xe7 */
/* File: arm64/op_iput_wide_quick.S */
    /* iput-wide-quick vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w3, 1                         // w3<- field byte offset
    GET_VREG w2, w2                     // w2<- fp[B], the object pointer
    ubfx    w0, wINST, #8, #4           // w0<- A
    cmp     w2, #0                      // check object for null
    beq     common_errNullObject        // object was null
    GET_VREG_WIDE x0, w0                // x0-< fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    add     x1, x2, x3                  // create a direct pointer
    str     x0, [x1]
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_object_quick: /* 0xe8 */
/* File: arm64/op_iput_object_quick.S */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     w2, wINST
    bl      MterpIputObjectQuick
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_quick: /* 0xe9 */
/* File: arm64/op_invoke_virtual_quick.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuick
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeVirtualQuick
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range_quick: /* 0xea */
/* File: arm64/op_invoke_virtual_range_quick.S */
/* File: arm64/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuickRange
    EXPORT_PC
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xPC
    mov     x3, xINST
    bl      MterpInvokeVirtualQuickRange
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_iput_boolean_quick: /* 0xeb */
/* File: arm64/op_iput_boolean_quick.S */
/* File: arm64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- fp[B], the object pointer
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     w3, #0                      // check object for null
    cbz     w3, common_errNullObject    // object was null
    GET_VREG w0, w2                     // w0<- fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    strb     w0, [x3, x1]             // obj.field<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_byte_quick: /* 0xec */
/* File: arm64/op_iput_byte_quick.S */
/* File: arm64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- fp[B], the object pointer
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     w3, #0                      // check object for null
    cbz     w3, common_errNullObject    // object was null
    GET_VREG w0, w2                     // w0<- fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    strb     w0, [x3, x1]             // obj.field<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_char_quick: /* 0xed */
/* File: arm64/op_iput_char_quick.S */
/* File: arm64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- fp[B], the object pointer
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     w3, #0                      // check object for null
    cbz     w3, common_errNullObject    // object was null
    GET_VREG w0, w2                     // w0<- fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    strh     w0, [x3, x1]             // obj.field<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_short_quick: /* 0xee */
/* File: arm64/op_iput_short_quick.S */
/* File: arm64/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- fp[B], the object pointer
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     w3, #0                      // check object for null
    cbz     w3, common_errNullObject    // object was null
    GET_VREG w0, w2                     // w0<- fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    strh     w0, [x3, x1]             // obj.field<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean_quick: /* 0xef */
/* File: arm64/op_iget_boolean_quick.S */
/* File: arm64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     x3, #0                      // check object for null
    beq     common_errNullObject        // object was null
    ldrb   w0, [x3, x1]                // w0<- obj.field
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    SET_VREG w0, w2                     // fp[A]<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_byte_quick: /* 0xf0 */
/* File: arm64/op_iget_byte_quick.S */
/* File: arm64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     x3, #0                      // check object for null
    beq     common_errNullObject        // object was null
    ldrsb   w0, [x3, x1]                // w0<- obj.field
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    SET_VREG w0, w2                     // fp[A]<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_char_quick: /* 0xf1 */
/* File: arm64/op_iget_char_quick.S */
/* File: arm64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     x3, #0                      // check object for null
    beq     common_errNullObject        // object was null
    ldrh   w0, [x3, x1]                // w0<- obj.field
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    SET_VREG w0, w2                     // fp[A]<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_short_quick: /* 0xf2 */
/* File: arm64/op_iget_short_quick.S */
/* File: arm64/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     x3, #0                      // check object for null
    beq     common_errNullObject        // object was null
    ldrsh   w0, [x3, x1]                // w0<- obj.field
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    
    SET_VREG w0, w2                     // fp[A]<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_invoke_lambda: /* 0xf3 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_f4: /* 0xf4 */
/* File: arm64/op_unused_f4.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_capture_variable: /* 0xf5 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_create_lambda: /* 0xf6 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_liberate_variable: /* 0xf7 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_box_lambda: /* 0xf8 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unbox_lambda: /* 0xf9 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fa: /* 0xfa */
/* File: arm64/op_unused_fa.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fb: /* 0xfb */
/* File: arm64/op_unused_fb.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fc: /* 0xfc */
/* File: arm64/op_unused_fc.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fd: /* 0xfd */
/* File: arm64/op_unused_fd.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fe: /* 0xfe */
/* File: arm64/op_unused_fe.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_ff: /* 0xff */
/* File: arm64/op_unused_ff.S */
/* File: arm64/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


    .balign 128
    .size   artMterpAsmInstructionStart, .-artMterpAsmInstructionStart
    .global artMterpAsmInstructionEnd
artMterpAsmInstructionEnd:

/*
 * ===========================================================================
 *  Sister implementations
 * ===========================================================================
 */
    .global artMterpAsmSisterStart
    .type   artMterpAsmSisterStart, %function
    .text
    .balign 4
artMterpAsmSisterStart:

    .size   artMterpAsmSisterStart, .-artMterpAsmSisterStart
    .global artMterpAsmSisterEnd
artMterpAsmSisterEnd:


    .global artMterpAsmAltInstructionStart
    .type   artMterpAsmAltInstructionStart, %function
    .text

artMterpAsmAltInstructionStart = .L_ALT_op_nop
/* ------------------------------ */
    .balign 128
.L_ALT_op_nop: /* 0x00 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (0 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move: /* 0x01 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (1 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_from16: /* 0x02 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (2 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_16: /* 0x03 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (3 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide: /* 0x04 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (4 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_from16: /* 0x05 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (5 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_16: /* 0x06 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (6 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object: /* 0x07 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (7 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_from16: /* 0x08 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (8 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_16: /* 0x09 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (9 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result: /* 0x0a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (10 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_wide: /* 0x0b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (11 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_object: /* 0x0c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (12 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_exception: /* 0x0d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (13 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void: /* 0x0e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (14 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return: /* 0x0f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (15 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_wide: /* 0x10 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (16 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_object: /* 0x11 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (17 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_4: /* 0x12 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (18 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_16: /* 0x13 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (19 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const: /* 0x14 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (20 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_high16: /* 0x15 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (21 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_16: /* 0x16 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (22 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_32: /* 0x17 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (23 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide: /* 0x18 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (24 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_high16: /* 0x19 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (25 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string: /* 0x1a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (26 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string_jumbo: /* 0x1b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (27 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_class: /* 0x1c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (28 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_enter: /* 0x1d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (29 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_exit: /* 0x1e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (30 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_check_cast: /* 0x1f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (31 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_instance_of: /* 0x20 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (32 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_array_length: /* 0x21 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (33 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_instance: /* 0x22 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (34 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_array: /* 0x23 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (35 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array: /* 0x24 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (36 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array_range: /* 0x25 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (37 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_fill_array_data: /* 0x26 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (38 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_throw: /* 0x27 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (39 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto: /* 0x28 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (40 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_16: /* 0x29 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (41 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_32: /* 0x2a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (42 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_packed_switch: /* 0x2b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (43 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sparse_switch: /* 0x2c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (44 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_float: /* 0x2d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (45 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_float: /* 0x2e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (46 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_double: /* 0x2f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (47 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_double: /* 0x30 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (48 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmp_long: /* 0x31 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (49 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eq: /* 0x32 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (50 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ne: /* 0x33 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (51 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lt: /* 0x34 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (52 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ge: /* 0x35 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (53 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gt: /* 0x36 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (54 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_le: /* 0x37 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (55 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eqz: /* 0x38 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (56 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_nez: /* 0x39 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (57 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ltz: /* 0x3a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (58 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gez: /* 0x3b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (59 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gtz: /* 0x3c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (60 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lez: /* 0x3d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (61 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3e: /* 0x3e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (62 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3f: /* 0x3f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (63 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_40: /* 0x40 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (64 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_41: /* 0x41 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (65 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_42: /* 0x42 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (66 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_43: /* 0x43 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (67 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget: /* 0x44 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (68 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_wide: /* 0x45 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (69 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_object: /* 0x46 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (70 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_boolean: /* 0x47 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (71 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_byte: /* 0x48 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (72 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_char: /* 0x49 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (73 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_short: /* 0x4a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (74 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput: /* 0x4b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (75 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_wide: /* 0x4c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (76 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_object: /* 0x4d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (77 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_boolean: /* 0x4e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (78 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_byte: /* 0x4f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (79 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_char: /* 0x50 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (80 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_short: /* 0x51 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (81 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget: /* 0x52 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (82 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide: /* 0x53 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (83 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object: /* 0x54 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (84 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean: /* 0x55 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (85 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte: /* 0x56 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (86 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char: /* 0x57 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (87 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short: /* 0x58 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (88 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput: /* 0x59 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (89 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide: /* 0x5a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (90 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object: /* 0x5b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (91 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean: /* 0x5c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (92 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte: /* 0x5d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (93 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char: /* 0x5e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (94 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short: /* 0x5f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (95 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget: /* 0x60 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (96 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_wide: /* 0x61 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (97 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_object: /* 0x62 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (98 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_boolean: /* 0x63 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (99 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_byte: /* 0x64 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (100 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_char: /* 0x65 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (101 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_short: /* 0x66 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (102 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput: /* 0x67 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (103 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_wide: /* 0x68 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (104 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_object: /* 0x69 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (105 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_boolean: /* 0x6a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (106 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_byte: /* 0x6b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (107 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_char: /* 0x6c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (108 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_short: /* 0x6d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (109 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual: /* 0x6e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (110 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super: /* 0x6f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (111 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct: /* 0x70 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (112 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static: /* 0x71 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (113 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface: /* 0x72 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (114 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void_no_barrier: /* 0x73 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (115 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range: /* 0x74 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (116 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super_range: /* 0x75 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (117 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct_range: /* 0x76 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (118 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static_range: /* 0x77 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (119 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface_range: /* 0x78 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (120 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_79: /* 0x79 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (121 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_7a: /* 0x7a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (122 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_int: /* 0x7b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (123 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_int: /* 0x7c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (124 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_long: /* 0x7d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (125 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_long: /* 0x7e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (126 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_float: /* 0x7f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (127 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_double: /* 0x80 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (128 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_long: /* 0x81 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (129 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_float: /* 0x82 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (130 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_double: /* 0x83 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (131 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_int: /* 0x84 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (132 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_float: /* 0x85 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (133 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_double: /* 0x86 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (134 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_int: /* 0x87 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (135 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_long: /* 0x88 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (136 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_double: /* 0x89 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (137 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_int: /* 0x8a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (138 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_long: /* 0x8b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (139 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_float: /* 0x8c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (140 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_byte: /* 0x8d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (141 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_char: /* 0x8e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (142 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_short: /* 0x8f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (143 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int: /* 0x90 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (144 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int: /* 0x91 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (145 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int: /* 0x92 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (146 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int: /* 0x93 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (147 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int: /* 0x94 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (148 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int: /* 0x95 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (149 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int: /* 0x96 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (150 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int: /* 0x97 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (151 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int: /* 0x98 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (152 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int: /* 0x99 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (153 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int: /* 0x9a */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (154 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long: /* 0x9b */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (155 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long: /* 0x9c */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (156 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long: /* 0x9d */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (157 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long: /* 0x9e */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (158 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long: /* 0x9f */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (159 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long: /* 0xa0 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (160 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long: /* 0xa1 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (161 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long: /* 0xa2 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (162 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long: /* 0xa3 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (163 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long: /* 0xa4 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (164 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long: /* 0xa5 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (165 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float: /* 0xa6 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (166 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float: /* 0xa7 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (167 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float: /* 0xa8 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (168 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float: /* 0xa9 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (169 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float: /* 0xaa */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (170 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double: /* 0xab */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (171 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double: /* 0xac */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (172 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double: /* 0xad */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (173 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double: /* 0xae */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (174 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double: /* 0xaf */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (175 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_2addr: /* 0xb0 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (176 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int_2addr: /* 0xb1 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (177 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_2addr: /* 0xb2 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (178 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_2addr: /* 0xb3 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (179 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_2addr: /* 0xb4 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (180 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_2addr: /* 0xb5 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (181 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_2addr: /* 0xb6 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (182 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_2addr: /* 0xb7 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (183 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_2addr: /* 0xb8 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (184 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_2addr: /* 0xb9 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (185 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_2addr: /* 0xba */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (186 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long_2addr: /* 0xbb */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (187 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long_2addr: /* 0xbc */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (188 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long_2addr: /* 0xbd */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (189 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long_2addr: /* 0xbe */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (190 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long_2addr: /* 0xbf */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (191 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long_2addr: /* 0xc0 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (192 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long_2addr: /* 0xc1 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (193 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long_2addr: /* 0xc2 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (194 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long_2addr: /* 0xc3 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (195 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long_2addr: /* 0xc4 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (196 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long_2addr: /* 0xc5 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (197 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float_2addr: /* 0xc6 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (198 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float_2addr: /* 0xc7 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (199 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float_2addr: /* 0xc8 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (200 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float_2addr: /* 0xc9 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (201 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float_2addr: /* 0xca */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (202 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double_2addr: /* 0xcb */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (203 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double_2addr: /* 0xcc */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (204 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double_2addr: /* 0xcd */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (205 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double_2addr: /* 0xce */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (206 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double_2addr: /* 0xcf */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (207 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit16: /* 0xd0 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (208 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int: /* 0xd1 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (209 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit16: /* 0xd2 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (210 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit16: /* 0xd3 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (211 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit16: /* 0xd4 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (212 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit16: /* 0xd5 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (213 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit16: /* 0xd6 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (214 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit16: /* 0xd7 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (215 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit8: /* 0xd8 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (216 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int_lit8: /* 0xd9 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (217 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit8: /* 0xda */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (218 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit8: /* 0xdb */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (219 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit8: /* 0xdc */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (220 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit8: /* 0xdd */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (221 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit8: /* 0xde */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (222 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit8: /* 0xdf */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (223 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_lit8: /* 0xe0 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (224 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_lit8: /* 0xe1 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (225 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_lit8: /* 0xe2 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (226 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_quick: /* 0xe3 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (227 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide_quick: /* 0xe4 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (228 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object_quick: /* 0xe5 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (229 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_quick: /* 0xe6 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (230 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide_quick: /* 0xe7 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (231 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object_quick: /* 0xe8 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (232 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_quick: /* 0xe9 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (233 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range_quick: /* 0xea */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (234 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean_quick: /* 0xeb */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (235 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte_quick: /* 0xec */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (236 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char_quick: /* 0xed */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (237 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short_quick: /* 0xee */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (238 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean_quick: /* 0xef */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (239 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte_quick: /* 0xf0 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (240 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char_quick: /* 0xf1 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (241 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short_quick: /* 0xf2 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (242 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_lambda: /* 0xf3 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (243 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_f4: /* 0xf4 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (244 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_capture_variable: /* 0xf5 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (245 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_create_lambda: /* 0xf6 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (246 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_liberate_variable: /* 0xf7 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (247 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_box_lambda: /* 0xf8 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (248 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unbox_lambda: /* 0xf9 */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (249 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fa: /* 0xfa */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (250 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fb: /* 0xfb */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (251 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fc: /* 0xfc */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (252 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fd: /* 0xfd */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (253 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fe: /* 0xfe */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (254 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_ff: /* 0xff */
/* File: arm64/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]            // refresh IBASE.
    adr    lr, artMterpAsmInstructionStart + (255 * 128)       // Addr of primary handler.
    mov    x0, xSELF
    add    x1, xFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     // (self, shadow_frame) Note: tail call.

    .balign 128
    .size   artMterpAsmAltInstructionStart, .-artMterpAsmAltInstructionStart
    .global artMterpAsmAltInstructionEnd
artMterpAsmAltInstructionEnd:
/* File: arm64/footer.S */
/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */


/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogDivideByZeroException
#endif
    b MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogArrayIndexException
#endif
    b MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNegativeArraySizeException
#endif
    b MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNoSuchMethodException
#endif
    b MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNullObjectException
#endif
    b MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogExceptionThrownException
#endif
    b MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    ldr  x2, [xSELF, #THREAD_FLAGS_OFFSET]
    bl MterpLogSuspendFallback
#endif
    b MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    ldr     x0, [xSELF, #THREAD_EXCEPTION_OFFSET]
    cbz     x0, MterpFallback                       // If not, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    bl      MterpHandleException                    // (self, shadow_frame)
    cbz     w0, MterpExceptionReturn                // no local catch, back to caller.
    ldr     x0, [xFP, #OFF_FP_CODE_ITEM]
    ldr     w1, [xFP, #OFF_FP_DEX_PC]
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]
    add     xPC, x0, #CODEITEM_INSNS_OFFSET
    add     xPC, xPC, x1, lsl #1                    // generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    /* resume execution at catch block */
    EXPORT_PC
    FETCH_INST
    GET_INST_OPCODE ip
    GOTO_OPCODE ip
    /* NOTE: no fallthrough */
/*
 * Common handling for branches with support for Jit profiling.
 * On entry:
 *    wINST          <= signed offset
 *    wPROFILE       <= signed hotness countdown (expanded to 32 bits)
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
MterpCommonTakenBranchNoFlags:
    cmp     wINST, #0
    b.gt    .L_forward_branch           // don't add forward branches to hotness
    tbnz    wPROFILE, #31, .L_no_count_backwards  // go if negative
    subs    wPROFILE, wPROFILE, #1      // countdown
    b.eq    .L_add_batch                // counted down to zero - report
.L_resume_backward_branch:
    ldr     lr, [xSELF, #THREAD_FLAGS_OFFSET]
    add     w2, wINST, wINST            // w2<- byte offset
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    REFRESH_IBASE
    ands    lr, lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .L_suspend_request_pending
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

.L_suspend_request_pending:
    EXPORT_PC
    mov     x0, xSELF
    bl      MterpSuspendCheck           // (self)
    cbnz    x0, MterpFallback
    REFRESH_IBASE                       // might have changed during suspend
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

.L_no_count_backwards:
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.ne    .L_resume_backward_branch
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  // (self, shadow_frame, offset)
    cbnz    x0, MterpOnStackReplacement
    b       .L_resume_backward_branch

.L_forward_branch:
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_osr_forward
.L_resume_forward_branch:
    add     w2, wINST, wINST            // w2<- byte offset
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

.L_check_osr_forward:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  // (self, shadow_frame, offset)
    cbnz    x0, MterpOnStackReplacement
    b       .L_resume_forward_branch

.L_add_batch:
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    strh    wPROFILE, [x1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    ldr     x0, [xFP, #OFF_FP_METHOD]
    mov     x2, xSELF
    bl      MterpAddHotnessBatch        // (method, shadow_frame, self)
    mov     wPROFILE, w0                // restore new hotness countdown to wPROFILE
    b       .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, #2
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  // (self, shadow_frame, offset)
    cbnz    x0, MterpOnStackReplacement
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/*
 * Check for suspend check request.  Assumes wINST already loaded, xPC advanced and
 * still needs to get the opcode and branch to it, and flags are in lr.
 */
MterpCheckSuspendAndContinue:
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]  // refresh xIBASE
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    check1
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
check1:
    EXPORT_PC
    mov     x0, xSELF
    bl      MterpSuspendCheck           // (self)
    cbnz    x0, MterpFallback           // Something in the environment changed, switch interpreters
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    sbfm x2, xINST, 0, 31
    bl MterpLogOSR
#endif
    mov  x0, #1                         // Signal normal return
    b    MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogFallback
#endif
MterpCommonFallback:
    mov     x0, #0                                  // signal retry with reference interpreter.
    b       MterpDone

/*
 * We pushed some registers on the stack in ExecuteMterpImpl, then saved
 * SP and LR.  Here we restore SP, restore the registers, and then restore
 * LR to PC.
 *
 * On entry:
 *  uint32_t* xFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    mov     x0, #1                                  // signal return to caller.
    b MterpDone
MterpReturn:
    ldr     x2, [xFP, #OFF_FP_RESULT_REGISTER]
    str     x0, [x2]
    mov     x0, #1                                  // signal return to caller.
MterpDone:
/*
 * At this point, we expect wPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending wPROFILE and the cached hotness counter).  wPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    cmp     wPROFILE, #0
    bgt     MterpProfileActive                      // if > 0, we may have some counts to report.
    ldp     fp, lr, [sp, #64]
    ldp     xPC, xFP, [sp, #48]
    ldp     xSELF, xINST, [sp, #32]
    ldp     xIBASE, xREFS, [sp, #16]
    ldp     xPROFILE, x27, [sp], #80
    ret

MterpProfileActive:
    mov     xINST, x0                               // stash return value
    /* Report cached hotness counts */
    ldr     x0, [xFP, #OFF_FP_METHOD]
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xSELF
    strh    wPROFILE, [x1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    bl      MterpAddHotnessBatch                    // (method, shadow_frame, self)
    mov     x0, xINST                               // restore return value
    ldp     fp, lr, [sp, #64]
    ldp     xPC, xFP, [sp, #48]
    ldp     xSELF, xINST, [sp, #32]
    ldp     xIBASE, xREFS, [sp, #16]
    ldp     xPROFILE, x27, [sp], #80
    ret

    .cfi_endproc
    .size   ExecuteMterpImpl, .-ExecuteMterpImpl


