/*
 * This file was generated automatically by gen-mterp.py for 'arm'.
 *
 * --> DO NOT EDIT <--
 */

/* File: arm/header.S */
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
ARM EABI general notes:

r0-r3 hold first 4 args to a method; they are not preserved across method calls
r4-r8 are available for general use
r9 is given special treatment in some situations, but not for us
r10 (sl) seems to be generally available
r11 (fp) is used by gcc (unless -fomit-frame-pointer is set)
r12 (ip) is scratch -- not preserved across method calls
r13 (sp) should be managed carefully in case a signal arrives
r14 (lr) must be preserved
r15 (pc) can be tinkered with directly

r0 holds returns of <= 4 bytes
r0-r1 hold returns of 8 bytes, low word in r0

Callee must save/restore r4+ (except r12) if it modifies them.  If VFP
is present, registers s16-s31 (a/k/a d8-d15, a/k/a q4-q7) must be preserved,
s0-s15 (d0-d7, q0-a3) do not need to be.

Stack is "full descending".  Only the arguments that don't fit in the first 4
registers are placed on the stack.  "sp" points at the first stacked argument
(i.e. the 5th arg).

VFP: single-precision results in s0, double-precision results in d0.

In the EABI, "sp" must be 64-bit aligned on entry to a function, and any
64-bit quantities (long long, double) must be 64-bit aligned.
*/

/*
Mterp and ARM notes:

The following registers have fixed assignments:

  reg nick      purpose
  r4  rPC       interpreted program counter, used for fetching instructions
  r5  rFP       interpreted frame pointer, used for accessing locals and args
  r6  rSELF     self (Thread) pointer
  r7  rINST     first 16-bit code unit of current instruction
  r8  rIBASE    interpreted instruction base pointer, used for computed goto
  r10 rPROFILE  branch profiling countdown
  r11 rREFS     base of object references in shadow frame  (ideally, we'll get rid of this later).

Macros are provided for common operations.  Each macro MUST emit only
one instruction to make instruction-counting easier.  They MUST NOT alter
unspecified registers or condition codes.
*/

/*
 * This is a #include, not a %include, because we want the C pre-processor
 * to expand the macros into assembler assignment statements.
 */
#include "asm_support.h"

#define MTERP_PROFILE_BRANCHES 1
#define MTERP_LOGGING 0

/* During bringup, we'll use the shadow frame model instead of rFP */
/* single-purpose registers, given names for clarity */
#define rPC      r4
#define rFP      r5
#define rSELF    r6
#define rINST    r7
#define rIBASE   r8
#define rPROFILE r10
#define rREFS    r11

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
    str  rPC, [rFP, #OFF_FP_DEX_PC_PTR]
.endm

.macro EXPORT_DEX_PC tmp
    ldr  \tmp, [rFP, #OFF_FP_CODE_ITEM]
    str  rPC, [rFP, #OFF_FP_DEX_PC_PTR]
    add  \tmp, #CODEITEM_INSNS_OFFSET
    sub  \tmp, rPC, \tmp
    asr  \tmp, #1
    str  \tmp, [rFP, #OFF_FP_DEX_PC]
.endm

/*
 * Fetch the next instruction from rPC into rINST.  Does not advance rPC.
 */
.macro FETCH_INST
    ldrh    rINST, [rPC]
.endm

/*
 * Fetch the next instruction from the specified offset.  Advances rPC
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
    ldrh    rINST, [rPC, #((\count)*2)]!
.endm

/*
 * The operation performed here is similar to FETCH_ADVANCE_INST, except the
 * src and dest registers are parameterized (not hard-wired to rPC and rINST).
 */
.macro PREFETCH_ADVANCE_INST dreg, sreg, count
    ldrh    \dreg, [\sreg, #((\count)*2)]!
.endm

/*
 * Similar to FETCH_ADVANCE_INST, but does not update rPC.  Used to load
 * rINST ahead of possible exception point.  Be sure to manually advance rPC
 * later.
 */
.macro PREFETCH_INST count
    ldrh    rINST, [rPC, #((\count)*2)]
.endm

/* Advance rPC by some number of code units. */
.macro ADVANCE count
  add  rPC, #((\count)*2)
.endm

/*
 * Fetch the next instruction from an offset specified by _reg.  Updates
 * rPC to point to the next instruction.  "_reg" must specify the distance
 * in bytes, *not* 16-bit code units, and may be a signed value.
 *
 * We want to write "ldrh rINST, [rPC, _reg, lsl #1]!", but some of the
 * bits that hold the shift distance are used for the half/byte/sign flags.
 * In some cases we can pre-double _reg for free, so we require a byte offset
 * here.
 */
.macro FETCH_ADVANCE_INST_RB reg
    ldrh    rINST, [rPC, \reg]!
.endm

/*
 * Fetch a half-word code unit from an offset past the current PC.  The
 * "_count" value is in 16-bit code units.  Does not advance rPC.
 *
 * The "_S" variant works the same but treats the value as signed.
 */
.macro FETCH reg, count
    ldrh    \reg, [rPC, #((\count)*2)]
.endm

.macro FETCH_S reg, count
    ldrsh   \reg, [rPC, #((\count)*2)]
.endm

/*
 * Fetch one byte from an offset past the current PC.  Pass in the same
 * "_count" as you would for FETCH, and an additional 0/1 indicating which
 * byte of the halfword you want (lo/hi).
 */
.macro FETCH_B reg, count, byte
    ldrb     \reg, [rPC, #((\count)*2+(\byte))]
.endm

/*
 * Put the instruction's opcode field into the specified register.
 */
.macro GET_INST_OPCODE reg
    and     \reg, rINST, #255
.endm

/*
 * Put the prefetched instruction's opcode field into the specified register.
 */
.macro GET_PREFETCHED_OPCODE oreg, ireg
    and     \oreg, \ireg, #255
.endm

/*
 * Begin executing the opcode in _reg.  Because this only jumps within the
 * interpreter, we don't have to worry about pre-ARMv5 THUMB interwork.
 */
.macro GOTO_OPCODE reg
    add     pc, rIBASE, \reg, lsl #7
.endm
.macro GOTO_OPCODE_BASE base,reg
    add     pc, \base, \reg, lsl #7
.endm

/*
 * Get/set the 32-bit value from a Dalvik register.
 */
.macro GET_VREG reg, vreg
    ldr     \reg, [rFP, \vreg, lsl #2]
.endm
.macro SET_VREG reg, vreg
    str     \reg, [rFP, \vreg, lsl #2]
    mov     \reg, #0
    str     \reg, [rREFS, \vreg, lsl #2]
.endm
.macro SET_VREG_OBJECT reg, vreg, tmpreg
    str     \reg, [rFP, \vreg, lsl #2]
    str     \reg, [rREFS, \vreg, lsl #2]
.endm
.macro SET_VREG_SHADOW reg, vreg
    str     \reg, [rREFS, \vreg, lsl #2]
.endm

/*
 * Clear the corresponding shadow regs for a vreg pair
 */
.macro CLEAR_SHADOW_PAIR vreg, tmp1, tmp2
    mov     \tmp1, #0
    add     \tmp2, \vreg, #1
    SET_VREG_SHADOW \tmp1, \vreg
    SET_VREG_SHADOW \tmp1, \tmp2
.endm

/*
 * Convert a virtual register index into an address.
 */
.macro VREG_INDEX_TO_ADDR reg, vreg
    add     \reg, rFP, \vreg, lsl #2   /* WARNING/FIXME: handle shadow frame vreg zero if store */
.endm

/*
 * Refresh handler table.
 */
.macro REFRESH_IBASE
  ldr     rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]
.endm

/* File: arm/entry.S */
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
    .align  2
    .global ExecuteMterpImpl
    .type   ExecuteMterpImpl, %function

/*
 * On entry:
 *  r0  Thread* self/
 *  r1  code_item
 *  r2  ShadowFrame
 *  r3  JValue* result_register
 *
 */

ExecuteMterpImpl:
    .fnstart
    .save {r3-r10,fp,lr}
    stmfd   sp!, {r3-r10,fp,lr}         @ save 10 regs, (r3 just to align 64)

    /* Remember the return register */
    str     r3, [r2, #SHADOWFRAME_RESULT_REGISTER_OFFSET]

    /* Remember the code_item */
    str     r1, [r2, #SHADOWFRAME_CODE_ITEM_OFFSET]

    /* set up "named" registers */
    mov     rSELF, r0
    ldr     r0, [r2, #SHADOWFRAME_NUMBER_OF_VREGS_OFFSET]
    add     rFP, r2, #SHADOWFRAME_VREGS_OFFSET     @ point to vregs.
    VREG_INDEX_TO_ADDR rREFS, r0                   @ point to reference array in shadow frame
    ldr     r0, [r2, #SHADOWFRAME_DEX_PC_OFFSET]   @ Get starting dex_pc.
    add     rPC, r1, #CODEITEM_INSNS_OFFSET        @ Point to base of insns[]
    add     rPC, rPC, r0, lsl #1                   @ Create direct pointer to 1st dex opcode
    EXPORT_PC

    /* Starting ibase */
    ldr     rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]

    /* Set up for backwards branches & osr profiling */
    ldr     r0, [rFP, #OFF_FP_METHOD]
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    bl      MterpSetUpHotnessCountdown
    mov     rPROFILE, r0                @ Starting hotness countdown to rPROFILE

    /* start executing the instruction at rPC */
    FETCH_INST                          @ load rINST from rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
    /* NOTE: no fallthrough */


    .global artMterpAsmInstructionStart
    .type   artMterpAsmInstructionStart, %function
artMterpAsmInstructionStart = .L_op_nop
    .text

/* ------------------------------ */
    .balign 128
.L_op_nop: /* 0x00 */
/* File: arm/op_nop.S */
    FETCH_ADVANCE_INST 1                @ advance to next instr, load rINST
    GET_INST_OPCODE ip                  @ ip<- opcode from rINST
    GOTO_OPCODE ip                      @ execute it

/* ------------------------------ */
    .balign 128
.L_op_move: /* 0x01 */
/* File: arm/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B from 15:12
    ubfx    r0, rINST, #8, #4           @ r0<- A from 11:8
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[B]
    GET_INST_OPCODE ip                  @ ip<- opcode from rINST
    .if 0
    SET_VREG_OBJECT r2, r0              @ fp[A]<- r2
    .else
    SET_VREG r2, r0                     @ fp[A]<- r2
    .endif
    GOTO_OPCODE ip                      @ execute next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_from16: /* 0x02 */
/* File: arm/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    FETCH r1, 1                         @ r1<- BBBB
    mov     r0, rINST, lsr #8           @ r0<- AA
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[BBBB]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if 0
    SET_VREG_OBJECT r2, r0              @ fp[AA]<- r2
    .else
    SET_VREG r2, r0                     @ fp[AA]<- r2
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_16: /* 0x03 */
/* File: arm/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    FETCH r1, 2                         @ r1<- BBBB
    FETCH r0, 1                         @ r0<- AAAA
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[BBBB]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if 0
    SET_VREG_OBJECT r2, r0              @ fp[AAAA]<- r2
    .else
    SET_VREG r2, r0                     @ fp[AAAA]<- r2
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_wide: /* 0x04 */
/* File: arm/op_move_wide.S */
    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r2, rINST        @ r2<- &fp[A]
    ldmia   r3, {r0-r1}                 @ r0/r1<- fp[B]
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r2, {r0-r1}                 @ fp[A]<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_wide_from16: /* 0x05 */
/* File: arm/op_move_wide_from16.S */
    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH r3, 1                         @ r3<- BBBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BBBB]
    VREG_INDEX_TO_ADDR r2, rINST        @ r2<- &fp[AA]
    ldmia   r3, {r0-r1}                 @ r0/r1<- fp[BBBB]
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r2, {r0-r1}                 @ fp[AA]<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_wide_16: /* 0x06 */
/* File: arm/op_move_wide_16.S */
    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH r3, 2                         @ r3<- BBBB
    FETCH r2, 1                         @ r2<- AAAA
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BBBB]
    VREG_INDEX_TO_ADDR lr, r2           @ r2<- &fp[AAAA]
    ldmia   r3, {r0-r1}                 @ r0/r1<- fp[BBBB]
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    CLEAR_SHADOW_PAIR r2, r3, ip        @ Zero out the shadow regs
    stmia   lr, {r0-r1}                 @ fp[AAAA]<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_object: /* 0x07 */
/* File: arm/op_move_object.S */
/* File: arm/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B from 15:12
    ubfx    r0, rINST, #8, #4           @ r0<- A from 11:8
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[B]
    GET_INST_OPCODE ip                  @ ip<- opcode from rINST
    .if 1
    SET_VREG_OBJECT r2, r0              @ fp[A]<- r2
    .else
    SET_VREG r2, r0                     @ fp[A]<- r2
    .endif
    GOTO_OPCODE ip                      @ execute next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_object_from16: /* 0x08 */
/* File: arm/op_move_object_from16.S */
/* File: arm/op_move_from16.S */
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    FETCH r1, 1                         @ r1<- BBBB
    mov     r0, rINST, lsr #8           @ r0<- AA
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[BBBB]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if 1
    SET_VREG_OBJECT r2, r0              @ fp[AA]<- r2
    .else
    SET_VREG r2, r0                     @ fp[AA]<- r2
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_object_16: /* 0x09 */
/* File: arm/op_move_object_16.S */
/* File: arm/op_move_16.S */
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    FETCH r1, 2                         @ r1<- BBBB
    FETCH r0, 1                         @ r0<- AAAA
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[BBBB]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if 1
    SET_VREG_OBJECT r2, r0              @ fp[AAAA]<- r2
    .else
    SET_VREG r2, r0                     @ fp[AAAA]<- r2
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_result: /* 0x0a */
/* File: arm/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    mov     r2, rINST, lsr #8           @ r2<- AA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    ldr     r0, [rFP, #OFF_FP_RESULT_REGISTER]  @ get pointer to result JType.
    ldr     r0, [r0]                    @ r0 <- result.i.
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if 0
    SET_VREG_OBJECT r0, r2, r1          @ fp[AA]<- r0
    .else
    SET_VREG r0, r2                     @ fp[AA]<- r0
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_result_wide: /* 0x0b */
/* File: arm/op_move_result_wide.S */
    /* move-result-wide vAA */
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    ldr     r3, [rFP, #OFF_FP_RESULT_REGISTER]
    VREG_INDEX_TO_ADDR r2, rINST        @ r2<- &fp[AA]
    ldmia   r3, {r0-r1}                 @ r0/r1<- retval.j
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    stmia   r2, {r0-r1}                 @ fp[AA]<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_move_result_object: /* 0x0c */
/* File: arm/op_move_result_object.S */
/* File: arm/op_move_result.S */
    /* for: move-result, move-result-object */
    /* op vAA */
    mov     r2, rINST, lsr #8           @ r2<- AA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    ldr     r0, [rFP, #OFF_FP_RESULT_REGISTER]  @ get pointer to result JType.
    ldr     r0, [r0]                    @ r0 <- result.i.
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if 1
    SET_VREG_OBJECT r0, r2, r1          @ fp[AA]<- r0
    .else
    SET_VREG r0, r2                     @ fp[AA]<- r0
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_move_exception: /* 0x0d */
/* File: arm/op_move_exception.S */
    /* move-exception vAA */
    mov     r2, rINST, lsr #8           @ r2<- AA
    ldr     r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov     r1, #0                      @ r1<- 0
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    SET_VREG_OBJECT r3, r2              @ fp[AA]<- exception obj
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    str     r1, [rSELF, #THREAD_EXCEPTION_OFFSET]  @ clear exception
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_return_void: /* 0x0e */
/* File: arm/op_return_void.S */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov    r0, #0
    mov    r1, #0
    b      MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return: /* 0x0f */
/* File: arm/op_return.S */
    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov     r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r0, r2                     @ r0<- vAA
    mov     r1, #0
    b       MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return_wide: /* 0x10 */
/* File: arm/op_return_wide.S */
    /*
     * Return a 64-bit value.
     */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov     r2, rINST, lsr #8           @ r2<- AA
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[AA]
    ldmia   r2, {r0-r1}                 @ r0/r1 <- vAA/vAA+1
    b       MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_return_object: /* 0x11 */
/* File: arm/op_return_object.S */
/* File: arm/op_return.S */
    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov     r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r0, r2                     @ r0<- vAA
    mov     r1, #0
    b       MterpReturn


/* ------------------------------ */
    .balign 128
.L_op_const_4: /* 0x12 */
/* File: arm/op_const_4.S */
    /* const/4 vA, #+B */
    sbfx    r1, rINST, #12, #4          @ r1<- sssssssB (sign-extended)
    ubfx    r0, rINST, #8, #4           @ r0<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ ip<- opcode from rINST
    SET_VREG r1, r0                     @ fp[A]<- r1
    GOTO_OPCODE ip                      @ execute next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_16: /* 0x13 */
/* File: arm/op_const_16.S */
    /* const/16 vAA, #+BBBB */
    FETCH_S r0, 1                       @ r0<- ssssBBBB (sign-extended)
    mov     r3, rINST, lsr #8           @ r3<- AA
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r3                     @ vAA<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const: /* 0x14 */
/* File: arm/op_const.S */
    /* const vAA, #+BBBBbbbb */
    mov     r3, rINST, lsr #8           @ r3<- AA
    FETCH r0, 1                         @ r0<- bbbb (low)
    FETCH r1, 2                         @ r1<- BBBB (high)
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    orr     r0, r0, r1, lsl #16         @ r0<- BBBBbbbb
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r3                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_high16: /* 0x15 */
/* File: arm/op_const_high16.S */
    /* const/high16 vAA, #+BBBB0000 */
    FETCH r0, 1                         @ r0<- 0000BBBB (zero-extended)
    mov     r3, rINST, lsr #8           @ r3<- AA
    mov     r0, r0, lsl #16             @ r0<- BBBB0000
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r3                     @ vAA<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide_16: /* 0x16 */
/* File: arm/op_const_wide_16.S */
    /* const-wide/16 vAA, #+BBBB */
    FETCH_S r0, 1                       @ r0<- ssssBBBB (sign-extended)
    mov     r3, rINST, lsr #8           @ r3<- AA
    mov     r1, r0, asr #31             @ r1<- ssssssss
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    CLEAR_SHADOW_PAIR r3, r2, lr        @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[AA]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r3, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide_32: /* 0x17 */
/* File: arm/op_const_wide_32.S */
    /* const-wide/32 vAA, #+BBBBbbbb */
    FETCH r0, 1                         @ r0<- 0000bbbb (low)
    mov     r3, rINST, lsr #8           @ r3<- AA
    FETCH_S r2, 2                       @ r2<- ssssBBBB (high)
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    orr     r0, r0, r2, lsl #16         @ r0<- BBBBbbbb
    CLEAR_SHADOW_PAIR r3, r2, lr        @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[AA]
    mov     r1, r0, asr #31             @ r1<- ssssssss
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r3, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide: /* 0x18 */
/* File: arm/op_const_wide.S */
    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    FETCH r0, 1                         @ r0<- bbbb (low)
    FETCH r1, 2                         @ r1<- BBBB (low middle)
    FETCH r2, 3                         @ r2<- hhhh (high middle)
    orr     r0, r0, r1, lsl #16         @ r0<- BBBBbbbb (low word)
    FETCH r3, 4                         @ r3<- HHHH (high)
    mov     r9, rINST, lsr #8           @ r9<- AA
    orr     r1, r2, r3, lsl #16         @ r1<- HHHHhhhh (high word)
    CLEAR_SHADOW_PAIR r9, r2, r3        @ Zero out the shadow regs
    FETCH_ADVANCE_INST 5                @ advance rPC, load rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[AA]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_wide_high16: /* 0x19 */
/* File: arm/op_const_wide_high16.S */
    /* const-wide/high16 vAA, #+BBBB000000000000 */
    FETCH r1, 1                         @ r1<- 0000BBBB (zero-extended)
    mov     r3, rINST, lsr #8           @ r3<- AA
    mov     r0, #0                      @ r0<- 00000000
    mov     r1, r1, lsl #16             @ r1<- BBBB0000
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    CLEAR_SHADOW_PAIR r3, r0, r2        @ Zero shadow regs
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[AA]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r3, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_string: /* 0x1a */
/* File: arm/op_const_string.S */
    /* const/string vAA, String@BBBB */
    EXPORT_PC
    FETCH r0, 1                         @ r0<- BBBB
    mov     r1, rINST, lsr #8           @ r1<- AA
    add     r2, rFP, #OFF_FP_SHADOWFRAME
    mov     r3, rSELF
    bl      MterpConstString            @ (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2                     @ load rINST
    cmp     r0, #0                      @ fail?
    bne     MterpPossibleException      @ let reference interpreter deal with it.
    ADVANCE 2                           @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_string_jumbo: /* 0x1b */
/* File: arm/op_const_string_jumbo.S */
    /* const/string vAA, String@BBBBBBBB */
    EXPORT_PC
    FETCH r0, 1                         @ r0<- bbbb (low)
    FETCH r2, 2                         @ r2<- BBBB (high)
    mov     r1, rINST, lsr #8           @ r1<- AA
    orr     r0, r0, r2, lsl #16         @ r1<- BBBBbbbb
    add     r2, rFP, #OFF_FP_SHADOWFRAME
    mov     r3, rSELF
    bl      MterpConstString            @ (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 3                     @ advance rPC
    cmp     r0, #0                      @ fail?
    bne     MterpPossibleException      @ let reference interpreter deal with it.
    ADVANCE 3                           @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_const_class: /* 0x1c */
/* File: arm/op_const_class.S */
    /* const/class vAA, Class@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- BBBB
    mov     r1, rINST, lsr #8           @ r1<- AA
    add     r2, rFP, #OFF_FP_SHADOWFRAME
    mov     r3, rSELF
    bl      MterpConstClass             @ (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2
    cmp     r0, #0
    bne     MterpPossibleException
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_monitor_enter: /* 0x1d */
/* File: arm/op_monitor_enter.S */
    /*
     * Synchronize on an object.
     */
    /* monitor-enter vAA */
    EXPORT_PC
    mov      r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r0, r2                      @ r0<- vAA (object)
    mov      r1, rSELF                   @ r1<- self
    bl       artLockObjectFromCode
    cmp      r0, #0
    bne      MterpException
    FETCH_ADVANCE_INST 1
    GET_INST_OPCODE ip                   @ extract opcode from rINST
    GOTO_OPCODE ip                       @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_monitor_exit: /* 0x1e */
/* File: arm/op_monitor_exit.S */
    /*
     * Unlock an object.
     *
     * Exceptions that occur when unlocking a monitor need to appear as
     * if they happened at the following instruction.  See the Dalvik
     * instruction spec.
     */
    /* monitor-exit vAA */
    EXPORT_PC
    mov      r2, rINST, lsr #8          @ r2<- AA
    GET_VREG r0, r2                     @ r0<- vAA (object)
    mov      r1, rSELF                  @ r0<- self
    bl       artUnlockObjectFromCode    @ r0<- success for unlock(self, obj)
    cmp     r0, #0                      @ failed?
    bne     MterpException
    FETCH_ADVANCE_INST 1                @ before throw: advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_check_cast: /* 0x1f */
/* File: arm/op_check_cast.S */
    /*
     * Check to see if a cast from one class to another is allowed.
     */
    /* check-cast vAA, class@BBBB */
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- BBBB
    mov      r1, rINST, lsr #8          @ r1<- AA
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &object
    ldr      r2, [rFP, #OFF_FP_METHOD]  @ r2<- method
    mov      r3, rSELF                  @ r3<- self
    bl       MterpCheckCast             @ (index, &obj, method, self)
    PREFETCH_INST 2
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_instance_of: /* 0x20 */
/* File: arm/op_instance_of.S */
    /*
     * Check to see if an object reference is an instance of a class.
     *
     * Most common situation is a non-null object, being compared against
     * an already-resolved class.
     */
    /* instance-of vA, vB, class@CCCC */
    EXPORT_PC
    FETCH     r0, 1                     @ r0<- CCCC
    mov       r1, rINST, lsr #12        @ r1<- B
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &object
    ldr       r2, [rFP, #OFF_FP_METHOD] @ r2<- method
    mov       r3, rSELF                 @ r3<- self
    bl        MterpInstanceOf           @ (index, &obj, method, self)
    ldr       r1, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx      r9, rINST, #8, #4         @ r9<- A
    PREFETCH_INST 2
    cmp       r1, #0                    @ exception pending?
    bne       MterpException
    ADVANCE 2                           @ advance rPC
    SET_VREG r0, r9                     @ vA<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_array_length: /* 0x21 */
/* File: arm/op_array_length.S */
    /*
     * Return the length of an array.
     */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r2, rINST, #8, #4           @ r2<- A
    GET_VREG r0, r1                     @ r0<- vB (object ref)
    cmp     r0, #0                      @ is object null?
    beq     common_errNullObject        @ yup, fail
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- array length
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r3, r2                     @ vB<- length
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_new_instance: /* 0x22 */
/* File: arm/op_new_instance.S */
    /*
     * Create a new instance of a class.
     */
    /* new-instance vAA, class@BBBB */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rSELF
    mov     r2, rINST
    bl      MterpNewInstance           @ (shadow_frame, self, inst_data)
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 2               @ advance rPC, load rINST
    GET_INST_OPCODE ip                 @ extract opcode from rINST
    GOTO_OPCODE ip                     @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_new_array: /* 0x23 */
/* File: arm/op_new_array.S */
    /*
     * Allocate an array of objects, specified with the array class
     * and a count.
     *
     * The verifier guarantees that this is an array class, so we don't
     * check for it here.
     */
    /* new-array vA, vB, class@CCCC */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    mov     r3, rSELF
    bl      MterpNewArray
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array: /* 0x24 */
/* File: arm/op_filled_new_array.S */
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern MterpFilledNewArray
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rSELF
    bl      MterpFilledNewArray
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_filled_new_array_range: /* 0x25 */
/* File: arm/op_filled_new_array_range.S */
/* File: arm/op_filled_new_array.S */
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern MterpFilledNewArrayRange
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rSELF
    bl      MterpFilledNewArrayRange
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_fill_array_data: /* 0x26 */
/* File: arm/op_fill_array_data.S */
    /* fill-array-data vAA, +BBBBBBBB */
    EXPORT_PC
    FETCH r0, 1                         @ r0<- bbbb (lo)
    FETCH r1, 2                         @ r1<- BBBB (hi)
    mov     r3, rINST, lsr #8           @ r3<- AA
    orr     r1, r0, r1, lsl #16         @ r1<- BBBBbbbb
    GET_VREG r0, r3                     @ r0<- vAA (array object)
    add     r1, rPC, r1, lsl #1         @ r1<- PC + BBBBbbbb*2 (array data off.)
    bl      MterpFillArrayData          @ (obj, payload)
    cmp     r0, #0                      @ 0 means an exception is thrown
    beq     MterpPossibleException      @ exception?
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_throw: /* 0x27 */
/* File: arm/op_throw.S */
    /*
     * Throw an exception object in the current thread.
     */
    /* throw vAA */
    EXPORT_PC
    mov      r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r1, r2                      @ r1<- vAA (exception object)
    cmp      r1, #0                      @ null object?
    beq      common_errNullObject        @ yes, throw an NPE instead
    str      r1, [rSELF, #THREAD_EXCEPTION_OFFSET]  @ thread->exception<- obj
    b        MterpException

/* ------------------------------ */
    .balign 128
.L_op_goto: /* 0x28 */
/* File: arm/op_goto.S */
    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    sbfx    rINST, rINST, #8, #8           @ rINST<- ssssssAA (sign-extended)
    b       MterpCommonTakenBranchNoFlags

/* ------------------------------ */
    .balign 128
.L_op_goto_16: /* 0x29 */
/* File: arm/op_goto_16.S */
    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
    FETCH_S rINST, 1                    @ rINST<- ssssAAAA (sign-extended)
    b       MterpCommonTakenBranchNoFlags

/* ------------------------------ */
    .balign 128
.L_op_goto_32: /* 0x2a */
/* File: arm/op_goto_32.S */
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
    FETCH r0, 1                         @ r0<- aaaa (lo)
    FETCH r3, 2                         @ r1<- AAAA (hi)
    orrs    rINST, r0, r3, lsl #16      @ rINST<- AAAAaaaa
    b       MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_packed_switch: /* 0x2b */
/* File: arm/op_packed_switch.S */
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
    FETCH r0, 1                         @ r0<- bbbb (lo)
    FETCH r1, 2                         @ r1<- BBBB (hi)
    mov     r3, rINST, lsr #8           @ r3<- AA
    orr     r0, r0, r1, lsl #16         @ r0<- BBBBbbbb
    GET_VREG r1, r3                     @ r1<- vAA
    add     r0, rPC, r0, lsl #1         @ r0<- PC + BBBBbbbb*2
    bl      MterpDoPackedSwitch                       @ r0<- code-unit branch offset
    movs    rINST, r0
    b       MterpCommonTakenBranch

/* ------------------------------ */
    .balign 128
.L_op_sparse_switch: /* 0x2c */
/* File: arm/op_sparse_switch.S */
/* File: arm/op_packed_switch.S */
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
    FETCH r0, 1                         @ r0<- bbbb (lo)
    FETCH r1, 2                         @ r1<- BBBB (hi)
    mov     r3, rINST, lsr #8           @ r3<- AA
    orr     r0, r0, r1, lsl #16         @ r0<- BBBBbbbb
    GET_VREG r1, r3                     @ r1<- vAA
    add     r0, rPC, r0, lsl #1         @ r0<- PC + BBBBbbbb*2
    bl      MterpDoSparseSwitch                       @ r0<- code-unit branch offset
    movs    rINST, r0
    b       MterpCommonTakenBranch


/* ------------------------------ */
    .balign 128
.L_op_cmpl_float: /* 0x2d */
/* File: arm/op_cmpl_float.S */
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     *
     * int compare(x, y) {
     *     if (x == y) {
     *         return 0;
     *     } else if (x > y) {
     *         return 1;
     *     } else if (x < y) {
     *         return -1;
     *     } else {
     *         return -1;
     *     }
     * }
     */
    /* op vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    flds    s0, [r2]                    @ s0<- vBB
    flds    s1, [r3]                    @ s1<- vCC
    vcmpe.f32  s0, s1                   @ compare (vBB, vCC)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mvn     r0, #0                      @ r0<- -1 (default)
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fmstat                              @ export status flags
    movgt   r0, #1                      @ (greater than) r1<- 1
    moveq   r0, #0                      @ (equal) r1<- 0
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_cmpg_float: /* 0x2e */
/* File: arm/op_cmpg_float.S */
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
     *         return 1;
     *     }
     * }
     */
    /* op vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    flds    s0, [r2]                    @ s0<- vBB
    flds    s1, [r3]                    @ s1<- vCC
    vcmpe.f32 s0, s1                    @ compare (vBB, vCC)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mov     r0, #1                      @ r0<- 1 (default)
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fmstat                              @ export status flags
    mvnmi   r0, #0                      @ (less than) r1<- -1
    moveq   r0, #0                      @ (equal) r1<- 0
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_cmpl_double: /* 0x2f */
/* File: arm/op_cmpl_double.S */
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     *
     * int compare(x, y) {
     *     if (x == y) {
     *         return 0;
     *     } else if (x > y) {
     *         return 1;
     *     } else if (x < y) {
     *         return -1;
     *     } else {
     *         return -1;
     *     }
     * }
     */
    /* op vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    fldd    d0, [r2]                    @ d0<- vBB
    fldd    d1, [r3]                    @ d1<- vCC
    vcmpe.f64 d0, d1                    @ compare (vBB, vCC)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mvn     r0, #0                      @ r0<- -1 (default)
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fmstat                              @ export status flags
    movgt   r0, #1                      @ (greater than) r1<- 1
    moveq   r0, #0                      @ (equal) r1<- 0
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_cmpg_double: /* 0x30 */
/* File: arm/op_cmpg_double.S */
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
     *         return 1;
     *     }
     * }
     */
    /* op vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    fldd    d0, [r2]                    @ d0<- vBB
    fldd    d1, [r3]                    @ d1<- vCC
    vcmpe.f64 d0, d1                    @ compare (vBB, vCC)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mov     r0, #1                      @ r0<- 1 (default)
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fmstat                              @ export status flags
    mvnmi   r0, #0                      @ (less than) r1<- -1
    moveq   r0, #0                      @ (equal) r1<- 0
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_cmp_long: /* 0x31 */
/* File: arm/op_cmp_long.S */
    /*
     * Compare two 64-bit values.  Puts 0, 1, or -1 into the destination
     * register based on the results of the comparison.
     */
    /* cmp-long vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    cmp     r0, r2
    sbcs    ip, r1, r3                  @ Sets correct CCs for checking LT (but not EQ/NE)
    mov     ip, #0
    mvnlt   ip, #0                      @ -1
    cmpeq   r0, r2                      @ For correct EQ/NE, we may need to repeat the first CMP
    orrne   ip, #1
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG ip, r9                     @ vAA<- ip
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_if_eq: /* 0x32 */
/* File: arm/op_if_eq.S */
/* File: arm/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    beq MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_ne: /* 0x33 */
/* File: arm/op_if_ne.S */
/* File: arm/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    bne MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_lt: /* 0x34 */
/* File: arm/op_if_lt.S */
/* File: arm/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    blt MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_ge: /* 0x35 */
/* File: arm/op_if_ge.S */
/* File: arm/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    bge MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_gt: /* 0x36 */
/* File: arm/op_if_gt.S */
/* File: arm/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    bgt MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_le: /* 0x37 */
/* File: arm/op_if_le.S */
/* File: arm/bincmp.S */
    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    ble MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_eqz: /* 0x38 */
/* File: arm/op_if_eqz.S */
/* File: arm/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    beq MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_nez: /* 0x39 */
/* File: arm/op_if_nez.S */
/* File: arm/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    bne MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_ltz: /* 0x3a */
/* File: arm/op_if_ltz.S */
/* File: arm/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    blt MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_gez: /* 0x3b */
/* File: arm/op_if_gez.S */
/* File: arm/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    bge MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_gtz: /* 0x3c */
/* File: arm/op_if_gtz.S */
/* File: arm/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    bgt MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_if_lez: /* 0x3d */
/* File: arm/op_if_lez.S */
/* File: arm/zcmp.S */
    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    ble MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_unused_3e: /* 0x3e */
/* File: arm/op_unused_3e.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_3f: /* 0x3f */
/* File: arm/op_unused_3f.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_40: /* 0x40 */
/* File: arm/op_unused_40.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_41: /* 0x41 */
/* File: arm/op_unused_41.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_42: /* 0x42 */
/* File: arm/op_unused_42.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_43: /* 0x43 */
/* File: arm/op_unused_43.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_aget: /* 0x44 */
/* File: arm/op_aget.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #2     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldr   r2, [r0, #MIRROR_INT_ARRAY_DATA_OFFSET]     @ r2<- vBB[vCC]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r2, r9                     @ vAA<- r2
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aget_wide: /* 0x45 */
/* File: arm/op_aget_wide.S */
    /*
     * Array get, 64 bits.  vAA <- vBB[vCC].
     *
     * Arrays of long/double are 64-bit aligned, so it's okay to use LDRD.
     */
    /* aget-wide vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    CLEAR_SHADOW_PAIR r9, r2, r3        @ Zero out the shadow regs
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #3          @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldrd    r2, [r0, #MIRROR_WIDE_ARRAY_DATA_OFFSET]  @ r2/r3<- vBB[vCC]
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[AA]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r2-r3}                 @ vAA/vAA+1<- r2/r3
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aget_object: /* 0x46 */
/* File: arm/op_aget_object.S */
    /*
     * Array object get.  vAA <- vBB[vCC].
     *
     * for: aget-object
     */
    /* op vAA, vBB, vCC */
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    EXPORT_PC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    bl       artAGetObjectFromMterp     @ (array, index)
    ldr      r1, [rSELF, #THREAD_EXCEPTION_OFFSET]
    PREFETCH_INST 2
    cmp      r1, #0
    bne      MterpException
    SET_VREG_OBJECT r0, r9
    ADVANCE 2
    GET_INST_OPCODE ip
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aget_boolean: /* 0x47 */
/* File: arm/op_aget_boolean.S */
/* File: arm/op_aget.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #0     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldrb   r2, [r0, #MIRROR_BOOLEAN_ARRAY_DATA_OFFSET]     @ r2<- vBB[vCC]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r2, r9                     @ vAA<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aget_byte: /* 0x48 */
/* File: arm/op_aget_byte.S */
/* File: arm/op_aget.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #0     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldrsb   r2, [r0, #MIRROR_BYTE_ARRAY_DATA_OFFSET]     @ r2<- vBB[vCC]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r2, r9                     @ vAA<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aget_char: /* 0x49 */
/* File: arm/op_aget_char.S */
/* File: arm/op_aget.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #1     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldrh   r2, [r0, #MIRROR_CHAR_ARRAY_DATA_OFFSET]     @ r2<- vBB[vCC]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r2, r9                     @ vAA<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aget_short: /* 0x4a */
/* File: arm/op_aget_short.S */
/* File: arm/op_aget.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #1     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldrsh   r2, [r0, #MIRROR_SHORT_ARRAY_DATA_OFFSET]     @ r2<- vBB[vCC]
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r2, r9                     @ vAA<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput: /* 0x4b */
/* File: arm/op_aput.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]     @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #2     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r9                     @ r2<- vAA
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    str  r2, [r0, #MIRROR_INT_ARRAY_DATA_OFFSET]     @ vBB[vCC]<- r2
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aput_wide: /* 0x4c */
/* File: arm/op_aput_wide.S */
    /*
     * Array put, 64 bits.  vBB[vCC] <- vAA.
     *
     * Arrays of long/double are 64-bit aligned, so it's okay to use STRD.
     */
    /* aput-wide vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]    @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #3          @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[AA]
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    ldmia   r9, {r2-r3}                 @ r2/r3<- vAA/vAA+1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    strd    r2, [r0, #MIRROR_WIDE_ARRAY_DATA_OFFSET]  @ r2/r3<- vBB[vCC]
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aput_object: /* 0x4d */
/* File: arm/op_aput_object.S */
    /*
     * Store an object into an array.  vBB[vCC] <- vAA.
     */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    bl      MterpAputObject
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_aput_boolean: /* 0x4e */
/* File: arm/op_aput_boolean.S */
/* File: arm/op_aput.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]     @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #0     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r9                     @ r2<- vAA
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    strb  r2, [r0, #MIRROR_BOOLEAN_ARRAY_DATA_OFFSET]     @ vBB[vCC]<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput_byte: /* 0x4f */
/* File: arm/op_aput_byte.S */
/* File: arm/op_aput.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]     @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #0     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r9                     @ r2<- vAA
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    strb  r2, [r0, #MIRROR_BYTE_ARRAY_DATA_OFFSET]     @ vBB[vCC]<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput_char: /* 0x50 */
/* File: arm/op_aput_char.S */
/* File: arm/op_aput.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]     @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #1     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r9                     @ r2<- vAA
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    strh  r2, [r0, #MIRROR_CHAR_ARRAY_DATA_OFFSET]     @ vBB[vCC]<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_aput_short: /* 0x51 */
/* File: arm/op_aput_short.S */
/* File: arm/op_aput.S */
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
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    cmp     r0, #0                      @ null array object?
    beq     common_errNullObject        @ yes, bail
    ldr     r3, [r0, #MIRROR_ARRAY_LENGTH_OFFSET]     @ r3<- arrayObj->length
    add     r0, r0, r1, lsl #1     @ r0<- arrayObj + index*width
    cmp     r1, r3                      @ compare unsigned index, length
    bcs     common_errArrayIndex        @ index >= length, bail
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_VREG r2, r9                     @ r2<- vAA
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    strh  r2, [r0, #MIRROR_SHORT_ARRAY_DATA_OFFSET]     @ vBB[vCC]<- r2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget: /* 0x52 */
/* File: arm/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGet32InstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if 0
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_wide: /* 0x53 */
/* File: arm/op_iget_wide.S */
    /*
     * 64-bit instance field get.
     *
     * for: iget-wide
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGet64InstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpException                @ bail out
    CLEAR_SHADOW_PAIR r2, ip, lr           @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r3, r2              @ r3<- &fp[A]
    stmia    r3, {r0-r1}                   @ fp[A]<- r0/r1
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_object: /* 0x54 */
/* File: arm/op_iget_object.S */
/* File: arm/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGetObjInstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if 1
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean: /* 0x55 */
/* File: arm/op_iget_boolean.S */
/* File: arm/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGetBooleanInstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if 0
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_byte: /* 0x56 */
/* File: arm/op_iget_byte.S */
/* File: arm/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGetByteInstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if 0
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_char: /* 0x57 */
/* File: arm/op_iget_char.S */
/* File: arm/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGetCharInstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if 0
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_short: /* 0x58 */
/* File: arm/op_iget_short.S */
/* File: arm/op_iget.S */
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGetShortInstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if 0
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput: /* 0x59 */
/* File: arm/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field@CCCC */
    .extern artSet32InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    GET_VREG r2, r2                     @ r2<- fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet32InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_wide: /* 0x5a */
/* File: arm/op_iput_wide.S */
    /* iput-wide vA, vB, field@CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet64InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_object: /* 0x5b */
/* File: arm/op_iput_object.S */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    mov     r3, rSELF
    bl      MterpIputObject
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_boolean: /* 0x5c */
/* File: arm/op_iput_boolean.S */
/* File: arm/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field@CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    GET_VREG r2, r2                     @ r2<- fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet8InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_byte: /* 0x5d */
/* File: arm/op_iput_byte.S */
/* File: arm/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field@CCCC */
    .extern artSet8InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    GET_VREG r2, r2                     @ r2<- fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet8InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_char: /* 0x5e */
/* File: arm/op_iput_char.S */
/* File: arm/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field@CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    GET_VREG r2, r2                     @ r2<- fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet16InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_short: /* 0x5f */
/* File: arm/op_iput_short.S */
/* File: arm/op_iput.S */
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field@CCCC */
    .extern artSet16InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    GET_VREG r2, r2                     @ r2<- fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet16InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sget: /* 0x60 */
/* File: arm/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern artGet32StaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGet32StaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if 0
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip

/* ------------------------------ */
    .balign 128
.L_op_sget_wide: /* 0x61 */
/* File: arm/op_sget_wide.S */
    /*
     * SGET_WIDE handler wrapper.
     *
     */
    /* sget-wide vAA, field@BBBB */

    .extern artGet64StaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGet64StaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r9, rINST, lsr #8             @ r9<- AA
    VREG_INDEX_TO_ADDR lr, r9           @ r9<- &fp[AA]
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    CLEAR_SHADOW_PAIR r9, r2, ip        @ Zero out the shadow regs
    stmia lr, {r0-r1}                   @ vAA/vAA+1<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sget_object: /* 0x62 */
/* File: arm/op_sget_object.S */
/* File: arm/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern artGetObjStaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGetObjStaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if 1
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_boolean: /* 0x63 */
/* File: arm/op_sget_boolean.S */
/* File: arm/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern artGetBooleanStaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGetBooleanStaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if 0
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_byte: /* 0x64 */
/* File: arm/op_sget_byte.S */
/* File: arm/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern artGetByteStaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGetByteStaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if 0
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_char: /* 0x65 */
/* File: arm/op_sget_char.S */
/* File: arm/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern artGetCharStaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGetCharStaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if 0
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sget_short: /* 0x66 */
/* File: arm/op_sget_short.S */
/* File: arm/op_sget.S */
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern artGetShortStaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGetShortStaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if 0
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip


/* ------------------------------ */
    .balign 128
.L_op_sput: /* 0x67 */
/* File: arm/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    mov     r3, rINST, lsr #8           @ r3<- AA
    GET_VREG r1, r3                     @ r1<= fp[AA]
    ldr     r2, [rFP, #OFF_FP_METHOD]
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      artSet32StaticFromCode
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sput_wide: /* 0x68 */
/* File: arm/op_sput_wide.S */
    /*
     * SPUT_WIDE handler wrapper.
     *
     */
    /* sput-wide vAA, field@BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    ldr     r1, [rFP, #OFF_FP_METHOD]
    mov     r2, rINST, lsr #8           @ r3<- AA
    VREG_INDEX_TO_ADDR r2, r2
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      artSet64IndirectStaticFromMterp
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sput_object: /* 0x69 */
/* File: arm/op_sput_object.S */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    mov     r3, rSELF
    bl      MterpSputObject
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_sput_boolean: /* 0x6a */
/* File: arm/op_sput_boolean.S */
/* File: arm/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    mov     r3, rINST, lsr #8           @ r3<- AA
    GET_VREG r1, r3                     @ r1<= fp[AA]
    ldr     r2, [rFP, #OFF_FP_METHOD]
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      artSet8StaticFromCode
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sput_byte: /* 0x6b */
/* File: arm/op_sput_byte.S */
/* File: arm/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    mov     r3, rINST, lsr #8           @ r3<- AA
    GET_VREG r1, r3                     @ r1<= fp[AA]
    ldr     r2, [rFP, #OFF_FP_METHOD]
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      artSet8StaticFromCode
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sput_char: /* 0x6c */
/* File: arm/op_sput_char.S */
/* File: arm/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    mov     r3, rINST, lsr #8           @ r3<- AA
    GET_VREG r1, r3                     @ r1<= fp[AA]
    ldr     r2, [rFP, #OFF_FP_METHOD]
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      artSet16StaticFromCode
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sput_short: /* 0x6d */
/* File: arm/op_sput_short.S */
/* File: arm/op_sput.S */
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    mov     r3, rINST, lsr #8           @ r3<- AA
    GET_VREG r1, r3                     @ r1<= fp[AA]
    ldr     r2, [rFP, #OFF_FP_METHOD]
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      artSet16StaticFromCode
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual: /* 0x6e */
/* File: arm/op_invoke_virtual.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtual
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeVirtual
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
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
/* File: arm/op_invoke_super.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuper
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeSuper
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
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
/* File: arm/op_invoke_direct.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirect
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeDirect
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_static: /* 0x71 */
/* File: arm/op_invoke_static.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStatic
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeStatic
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip




/* ------------------------------ */
    .balign 128
.L_op_invoke_interface: /* 0x72 */
/* File: arm/op_invoke_interface.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterface
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeInterface
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
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
/* File: arm/op_return_void_no_barrier.S */
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov    r0, #0
    mov    r1, #0
    b      MterpReturn

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range: /* 0x74 */
/* File: arm/op_invoke_virtual_range.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualRange
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeVirtualRange
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_super_range: /* 0x75 */
/* File: arm/op_invoke_super_range.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeSuperRange
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeSuperRange
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_direct_range: /* 0x76 */
/* File: arm/op_invoke_direct_range.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeDirectRange
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeDirectRange
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_static_range: /* 0x77 */
/* File: arm/op_invoke_static_range.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeStaticRange
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeStaticRange
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_interface_range: /* 0x78 */
/* File: arm/op_invoke_interface_range.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeInterfaceRange
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeInterfaceRange
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_unused_79: /* 0x79 */
/* File: arm/op_unused_79.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_7a: /* 0x7a */
/* File: arm/op_unused_7a.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_neg_int: /* 0x7b */
/* File: arm/op_neg_int.S */
/* File: arm/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r3                     @ r0<- vB
                               @ optional op; may set condition codes
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    rsb     r0, r0, #0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_not_int: /* 0x7c */
/* File: arm/op_not_int.S */
/* File: arm/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r3                     @ r0<- vB
                               @ optional op; may set condition codes
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    mvn     r0, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_neg_long: /* 0x7d */
/* File: arm/op_neg_long.S */
/* File: arm/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0/r1".
     * This could be an ARM instruction or a function call.
     *
     * For: neg-long, not-long, neg-double, long-to-double, double-to-long
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r3, {r0-r1}                 @ r0/r1<- vAA
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    rsbs    r0, r0, #0                           @ optional op; may set condition codes
    rsc     r1, r1, #0                              @ r0/r1<- op, r2-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-11 instructions */


/* ------------------------------ */
    .balign 128
.L_op_not_long: /* 0x7e */
/* File: arm/op_not_long.S */
/* File: arm/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0/r1".
     * This could be an ARM instruction or a function call.
     *
     * For: neg-long, not-long, neg-double, long-to-double, double-to-long
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r3, {r0-r1}                 @ r0/r1<- vAA
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    mvn     r0, r0                           @ optional op; may set condition codes
    mvn     r1, r1                              @ r0/r1<- op, r2-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-11 instructions */


/* ------------------------------ */
    .balign 128
.L_op_neg_float: /* 0x7f */
/* File: arm/op_neg_float.S */
/* File: arm/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r3                     @ r0<- vB
                               @ optional op; may set condition codes
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    add     r0, r0, #0x80000000                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_neg_double: /* 0x80 */
/* File: arm/op_neg_double.S */
/* File: arm/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0/r1".
     * This could be an ARM instruction or a function call.
     *
     * For: neg-long, not-long, neg-double, long-to-double, double-to-long
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r3, {r0-r1}                 @ r0/r1<- vAA
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    add     r1, r1, #0x80000000                              @ r0/r1<- op, r2-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-11 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_long: /* 0x81 */
/* File: arm/op_int_to_long.S */
/* File: arm/unopWider.S */
    /*
     * Generic 32bit-to-64bit unary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = op r0", where
     * "result" is a 64-bit quantity in r0/r1.
     *
     * For: int-to-long, int-to-double, float-to-long, float-to-double
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    GET_VREG r0, r3                     @ r0<- vB
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
                               @ optional op; may set condition codes
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    mov     r1, r0, asr #31                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vA/vA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 9-10 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_float: /* 0x82 */
/* File: arm/op_int_to_float.S */
/* File: arm/funop.S */
    /*
     * Generic 32-bit unary floating-point operation.  Provide an "instr"
     * line that specifies an instruction that performs "s1 = op s0".
     *
     * for: int-to-float, float-to-int
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    flds    s0, [r3]                    @ s0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fsitos  s1, s0                              @ s1<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fsts    s1, [r9]                    @ vA<- s1
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_int_to_double: /* 0x83 */
/* File: arm/op_int_to_double.S */
/* File: arm/funopWider.S */
    /*
     * Generic 32bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "d0 = op s0".
     *
     * For: int-to-double, float-to-double
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    flds    s0, [r3]                    @ s0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fsitod  d0, s0                              @ d0<- op
    CLEAR_SHADOW_PAIR r9, ip, lr        @ Zero shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fstd    d0, [r9]                    @ vA<- d0
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_long_to_int: /* 0x84 */
/* File: arm/op_long_to_int.S */
/* we ignore the high word, making this equivalent to a 32-bit reg move */
/* File: arm/op_move.S */
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B from 15:12
    ubfx    r0, rINST, #8, #4           @ r0<- A from 11:8
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_VREG r2, r1                     @ r2<- fp[B]
    GET_INST_OPCODE ip                  @ ip<- opcode from rINST
    .if 0
    SET_VREG_OBJECT r2, r0              @ fp[A]<- r2
    .else
    SET_VREG r2, r0                     @ fp[A]<- r2
    .endif
    GOTO_OPCODE ip                      @ execute next instruction


/* ------------------------------ */
    .balign 128
.L_op_long_to_float: /* 0x85 */
/* File: arm/op_long_to_float.S */
/* File: arm/unopNarrower.S */
    /*
     * Generic 64bit-to-32bit unary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = op r0/r1", where
     * "result" is a 32-bit quantity in r0.
     *
     * For: long-to-float, double-to-int, double-to-float
     *
     * (This would work for long-to-int, but that instruction is actually
     * an exact match for op_move.)
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    ldmia   r3, {r0-r1}                 @ r0/r1<- vB/vB+1
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      __aeabi_l2f                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 9-10 instructions */


/* ------------------------------ */
    .balign 128
.L_op_long_to_double: /* 0x86 */
/* File: arm/op_long_to_double.S */
    /*
     * Specialised 64-bit floating point operation.
     *
     * Note: The result will be returned in d2.
     *
     * For: long-to-double
     */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[A]
    vldr    d0, [r3]                    @ d0<- vAA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

    vcvt.f64.s32    d1, s1              @ d1<- (double)(vAAh)
    vcvt.f64.u32    d2, s0              @ d2<- (double)(vAAl)
    vldr            d3, constvalop_long_to_double
    vmla.f64        d2, d1, d3          @ d2<- vAAh*2^32 + vAAl

    GET_INST_OPCODE ip                  @ extract opcode from rINST
    vstr.64 d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction

    /* literal pool helper */
constvalop_long_to_double:
    .8byte          0x41f0000000000000

/* ------------------------------ */
    .balign 128
.L_op_float_to_int: /* 0x87 */
/* File: arm/op_float_to_int.S */
/* File: arm/funop.S */
    /*
     * Generic 32-bit unary floating-point operation.  Provide an "instr"
     * line that specifies an instruction that performs "s1 = op s0".
     *
     * for: int-to-float, float-to-int
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    flds    s0, [r3]                    @ s0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    ftosizs s1, s0                              @ s1<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fsts    s1, [r9]                    @ vA<- s1
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_float_to_long: /* 0x88 */
/* File: arm/op_float_to_long.S */
@include "arm/unopWider.S" {"instr":"bl      __aeabi_f2lz"}
/* File: arm/unopWider.S */
    /*
     * Generic 32bit-to-64bit unary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = op r0", where
     * "result" is a 64-bit quantity in r0/r1.
     *
     * For: int-to-long, int-to-double, float-to-long, float-to-double
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    GET_VREG r0, r3                     @ r0<- vB
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
                               @ optional op; may set condition codes
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    bl      f2l_doconv                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vA/vA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 9-10 instructions */



/* ------------------------------ */
    .balign 128
.L_op_float_to_double: /* 0x89 */
/* File: arm/op_float_to_double.S */
/* File: arm/funopWider.S */
    /*
     * Generic 32bit-to-64bit floating point unary operation.  Provide an
     * "instr" line that specifies an instruction that performs "d0 = op s0".
     *
     * For: int-to-double, float-to-double
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    flds    s0, [r3]                    @ s0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    vcvt.f64.f32  d0, s0                              @ d0<- op
    CLEAR_SHADOW_PAIR r9, ip, lr        @ Zero shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fstd    d0, [r9]                    @ vA<- d0
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_double_to_int: /* 0x8a */
/* File: arm/op_double_to_int.S */
/* File: arm/funopNarrower.S */
    /*
     * Generic 64bit-to-32bit unary floating point operation.  Provide an
     * "instr" line that specifies an instruction that performs "s0 = op d0".
     *
     * For: double-to-int, double-to-float
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    fldd    d0, [r3]                    @ d0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    ftosizd  s0, d0                              @ s0<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fsts    s0, [r9]                    @ vA<- s0
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_double_to_long: /* 0x8b */
/* File: arm/op_double_to_long.S */
@include "arm/unopWide.S" {"instr":"bl      __aeabi_d2lz"}
/* File: arm/unopWide.S */
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0/r1".
     * This could be an ARM instruction or a function call.
     *
     * For: neg-long, not-long, neg-double, long-to-double, double-to-long
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r3, {r0-r1}                 @ r0/r1<- vAA
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      d2l_doconv                              @ r0/r1<- op, r2-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-11 instructions */



/* ------------------------------ */
    .balign 128
.L_op_double_to_float: /* 0x8c */
/* File: arm/op_double_to_float.S */
/* File: arm/funopNarrower.S */
    /*
     * Generic 64bit-to-32bit unary floating point operation.  Provide an
     * "instr" line that specifies an instruction that performs "s0 = op d0".
     *
     * For: double-to-int, double-to-float
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    fldd    d0, [r3]                    @ d0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    vcvt.f32.f64  s0, d0                              @ s0<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fsts    s0, [r9]                    @ vA<- s0
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_int_to_byte: /* 0x8d */
/* File: arm/op_int_to_byte.S */
/* File: arm/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r3                     @ r0<- vB
                               @ optional op; may set condition codes
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    sxtb    r0, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_char: /* 0x8e */
/* File: arm/op_int_to_char.S */
/* File: arm/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r3                     @ r0<- vB
                               @ optional op; may set condition codes
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    uxth    r0, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_int_to_short: /* 0x8f */
/* File: arm/op_int_to_short.S */
/* File: arm/unop.S */
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op r0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r3                     @ r0<- vB
                               @ optional op; may set condition codes
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    sxth    r0, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 8-9 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_int: /* 0x90 */
/* File: arm/op_add_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    add     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_int: /* 0x91 */
/* File: arm/op_sub_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    sub     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int: /* 0x92 */
/* File: arm/op_mul_int.S */
/* must be "mul r0, r1, r0" -- "r0, r0, r1" is illegal */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    mul     r0, r1, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int: /* 0x93 */
/* File: arm/op_div_int.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r0 = r0 div r1". The selection between sdiv or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * div-int
     *
     */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r0, r0, r1                  @ r0<- op
#else
    bl    __aeabi_idiv                  @ r0<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */

/* ------------------------------ */
    .balign 128
.L_op_rem_int: /* 0x94 */
/* File: arm/op_rem_int.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r1 = r0 rem r1". The selection between sdiv block or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * NOTE: idivmod returns quotient in r0 and remainder in r1
     *
     * rem-int
     *
     */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r2, r0, r1
    mls  r1, r1, r2, r0                 @ r1<- op, r0-r2 changed
#else
    bl   __aeabi_idivmod                @ r1<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r1, r9                     @ vAA<- r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */

/* ------------------------------ */
    .balign 128
.L_op_and_int: /* 0x95 */
/* File: arm/op_and_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    and     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int: /* 0x96 */
/* File: arm/op_or_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    orr     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int: /* 0x97 */
/* File: arm/op_xor_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    eor     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_int: /* 0x98 */
/* File: arm/op_shl_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, asl r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_int: /* 0x99 */
/* File: arm/op_shr_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, asr r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_int: /* 0x9a */
/* File: arm/op_ushr_int.S */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, lsr r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_long: /* 0x9b */
/* File: arm/op_add_long.S */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    adds    r0, r0, r2                           @ optional op; may set condition codes
    adc     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_long: /* 0x9c */
/* File: arm/op_sub_long.S */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    subs    r0, r0, r2                           @ optional op; may set condition codes
    sbc     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_long: /* 0x9d */
/* File: arm/op_mul_long.S */
    /*
     * Signed 64-bit integer multiply.
     *
     * Consider WXxYZ (r1r0 x r3r2) with a long multiply:
     *        WX
     *      x YZ
     *  --------
     *     ZW ZX
     *  YW YX
     *
     * The low word of the result holds ZX, the high word holds
     * (ZW+YX) + (the high overflow from ZX).  YW doesn't matter because
     * it doesn't fit in the low 64 bits.
     *
     * Unlike most ARM math operations, multiply instructions have
     * restrictions on using the same register more than once (Rd and Rm
     * cannot be the same).
     */
    /* mul-long vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    mul     ip, r2, r1                  @ ip<- ZxW
    umull   r1, lr, r2, r0              @ r1/lr <- ZxX
    mla     r2, r0, r3, ip              @ r2<- YxX + (ZxW)
    mov     r0, rINST, lsr #8           @ r0<- AA
    add     r2, r2, lr                  @ r2<- lr + low(ZxW + (YxX))
    VREG_INDEX_TO_ADDR r0, r0           @ r0<- &fp[AA]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r0, {r1-r2 }                @ vAA/vAA+1<- r1/r2
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_div_long: /* 0x9e */
/* File: arm/op_div_long.S */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 1
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      __aeabi_ldivmod                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_long: /* 0x9f */
/* File: arm/op_rem_long.S */
/* ldivmod returns quotient in r0/r1 and remainder in r2/r3 */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 1
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      __aeabi_ldivmod                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r2,r3}     @ vAA/vAA+1<- r2/r3
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_long: /* 0xa0 */
/* File: arm/op_and_long.S */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    and     r0, r0, r2                           @ optional op; may set condition codes
    and     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_long: /* 0xa1 */
/* File: arm/op_or_long.S */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    orr     r0, r0, r2                           @ optional op; may set condition codes
    orr     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_long: /* 0xa2 */
/* File: arm/op_xor_long.S */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    eor     r0, r0, r2                           @ optional op; may set condition codes
    eor     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_long: /* 0xa3 */
/* File: arm/op_shl_long.S */
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
     * 6 bits of the shift distance.
     */
    /* shl-long vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r3, r0, #255                @ r3<- BB
    mov     r0, r0, lsr #8              @ r0<- CC
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BB]
    GET_VREG r2, r0                     @ r2<- vCC
    ldmia   r3, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    CLEAR_SHADOW_PAIR r9, lr, ip        @ Zero out the shadow regs
    and     r2, r2, #63                 @ r2<- r2 & 0x3f
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[AA]
    mov     r1, r1, asl r2              @ r1<- r1 << r2
    rsb     r3, r2, #32                 @ r3<- 32 - r2
    orr     r1, r1, r0, lsr r3          @ r1<- r1 | (r0 << (32-r2))
    subs    ip, r2, #32                 @ ip<- r2 - 32
    movpl   r1, r0, asl ip              @ if r2 >= 32, r1<- r0 << (r2-32)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mov     r0, r0, asl r2              @ r0<- r0 << r2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_shr_long: /* 0xa4 */
/* File: arm/op_shr_long.S */
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
     * 6 bits of the shift distance.
     */
    /* shr-long vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r3, r0, #255                @ r3<- BB
    mov     r0, r0, lsr #8              @ r0<- CC
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BB]
    GET_VREG r2, r0                     @ r2<- vCC
    ldmia   r3, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    CLEAR_SHADOW_PAIR r9, lr, ip        @ Zero out the shadow regs
    and     r2, r2, #63                 @ r0<- r0 & 0x3f
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[AA]
    mov     r0, r0, lsr r2              @ r0<- r2 >> r2
    rsb     r3, r2, #32                 @ r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @ r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @ ip<- r2 - 32
    movpl   r0, r1, asr ip              @ if r2 >= 32, r0<-r1 >> (r2-32)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mov     r1, r1, asr r2              @ r1<- r1 >> r2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_ushr_long: /* 0xa5 */
/* File: arm/op_ushr_long.S */
    /*
     * Long integer shift.  This is different from the generic 32/64-bit
     * binary operations because vAA/vBB are 64-bit but vCC (the shift
     * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
     * 6 bits of the shift distance.
     */
    /* ushr-long vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r3, r0, #255                @ r3<- BB
    mov     r0, r0, lsr #8              @ r0<- CC
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BB]
    GET_VREG r2, r0                     @ r2<- vCC
    ldmia   r3, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    CLEAR_SHADOW_PAIR r9, lr, ip        @ Zero out the shadow regs
    and     r2, r2, #63                 @ r0<- r0 & 0x3f
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[AA]
    mov     r0, r0, lsr r2              @ r0<- r2 >> r2
    rsb     r3, r2, #32                 @ r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @ r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @ ip<- r2 - 32
    movpl   r0, r1, lsr ip              @ if r2 >= 32, r0<-r1 >>> (r2-32)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    mov     r1, r1, lsr r2              @ r1<- r1 >>> r2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_add_float: /* 0xa6 */
/* File: arm/op_add_float.S */
/* File: arm/fbinop.S */
    /*
     * Generic 32-bit floating-point operation.  Provide an "instr" line that
     * specifies an instruction that performs "s2 = s0 op s1".  Because we
     * use the "softfp" ABI, this must be an instruction, not a function call.
     *
     * For: add-float, sub-float, mul-float, div-float
     */
    /* floatop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    flds    s1, [r3]                    @ s1<- vCC
    flds    s0, [r2]                    @ s0<- vBB

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fadds   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sub_float: /* 0xa7 */
/* File: arm/op_sub_float.S */
/* File: arm/fbinop.S */
    /*
     * Generic 32-bit floating-point operation.  Provide an "instr" line that
     * specifies an instruction that performs "s2 = s0 op s1".  Because we
     * use the "softfp" ABI, this must be an instruction, not a function call.
     *
     * For: add-float, sub-float, mul-float, div-float
     */
    /* floatop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    flds    s1, [r3]                    @ s1<- vCC
    flds    s0, [r2]                    @ s0<- vBB

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fsubs   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_mul_float: /* 0xa8 */
/* File: arm/op_mul_float.S */
/* File: arm/fbinop.S */
    /*
     * Generic 32-bit floating-point operation.  Provide an "instr" line that
     * specifies an instruction that performs "s2 = s0 op s1".  Because we
     * use the "softfp" ABI, this must be an instruction, not a function call.
     *
     * For: add-float, sub-float, mul-float, div-float
     */
    /* floatop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    flds    s1, [r3]                    @ s1<- vCC
    flds    s0, [r2]                    @ s0<- vBB

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fmuls   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_div_float: /* 0xa9 */
/* File: arm/op_div_float.S */
/* File: arm/fbinop.S */
    /*
     * Generic 32-bit floating-point operation.  Provide an "instr" line that
     * specifies an instruction that performs "s2 = s0 op s1".  Because we
     * use the "softfp" ABI, this must be an instruction, not a function call.
     *
     * For: add-float, sub-float, mul-float, div-float
     */
    /* floatop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    flds    s1, [r3]                    @ s1<- vCC
    flds    s0, [r2]                    @ s0<- vBB

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fdivs   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_rem_float: /* 0xaa */
/* File: arm/op_rem_float.S */
/* EABI doesn't define a float remainder function, but libm does */
/* File: arm/binop.S */
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      fmodf                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_double: /* 0xab */
/* File: arm/op_add_double.S */
/* File: arm/fbinopWide.S */
    /*
     * Generic 64-bit double-precision floating point binary operation.
     * Provide an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * for: add-double, sub-double, mul-double, div-double
     */
    /* doubleop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    fldd    d1, [r3]                    @ d1<- vCC
    fldd    d0, [r2]                    @ d0<- vBB
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    faddd   d2, d0, d1                              @ s2<- op
    CLEAR_SHADOW_PAIR r9, ip, lr        @ Zero shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sub_double: /* 0xac */
/* File: arm/op_sub_double.S */
/* File: arm/fbinopWide.S */
    /*
     * Generic 64-bit double-precision floating point binary operation.
     * Provide an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * for: add-double, sub-double, mul-double, div-double
     */
    /* doubleop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    fldd    d1, [r3]                    @ d1<- vCC
    fldd    d0, [r2]                    @ d0<- vBB
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fsubd   d2, d0, d1                              @ s2<- op
    CLEAR_SHADOW_PAIR r9, ip, lr        @ Zero shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_mul_double: /* 0xad */
/* File: arm/op_mul_double.S */
/* File: arm/fbinopWide.S */
    /*
     * Generic 64-bit double-precision floating point binary operation.
     * Provide an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * for: add-double, sub-double, mul-double, div-double
     */
    /* doubleop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    fldd    d1, [r3]                    @ d1<- vCC
    fldd    d0, [r2]                    @ d0<- vBB
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fmuld   d2, d0, d1                              @ s2<- op
    CLEAR_SHADOW_PAIR r9, ip, lr        @ Zero shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_div_double: /* 0xae */
/* File: arm/op_div_double.S */
/* File: arm/fbinopWide.S */
    /*
     * Generic 64-bit double-precision floating point binary operation.
     * Provide an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * for: add-double, sub-double, mul-double, div-double
     */
    /* doubleop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vCC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &vBB
    fldd    d1, [r3]                    @ d1<- vCC
    fldd    d0, [r2]                    @ d0<- vBB
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    fdivd   d2, d0, d1                              @ s2<- op
    CLEAR_SHADOW_PAIR r9, ip, lr        @ Zero shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vAA
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_rem_double: /* 0xaf */
/* File: arm/op_rem_double.S */
/* EABI doesn't define a double remainder function, but libm does */
/* File: arm/binopWide.S */
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      fmod                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_int_2addr: /* 0xb0 */
/* File: arm/op_add_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    add     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_int_2addr: /* 0xb1 */
/* File: arm/op_sub_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    sub     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int_2addr: /* 0xb2 */
/* File: arm/op_mul_int_2addr.S */
/* must be "mul r0, r1, r0" -- "r0, r0, r1" is illegal */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    mul     r0, r1, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int_2addr: /* 0xb3 */
/* File: arm/op_div_int_2addr.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r0 = r0 div r1". The selection between sdiv or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * div-int/2addr
     *
     */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r0, r0, r1                  @ r0<- op
#else
    bl       __aeabi_idiv               @ r0<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_int_2addr: /* 0xb4 */
/* File: arm/op_rem_int_2addr.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r1 = r0 rem r1". The selection between sdiv block or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * NOTE: idivmod returns quotient in r0 and remainder in r1
     *
     * rem-int/2addr
     *
     */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r2, r0, r1
    mls     r1, r1, r2, r0              @ r1<- op
#else
    bl      __aeabi_idivmod             @ r1<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r1, r9                     @ vAA<- r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_int_2addr: /* 0xb5 */
/* File: arm/op_and_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    and     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int_2addr: /* 0xb6 */
/* File: arm/op_or_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    orr     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int_2addr: /* 0xb7 */
/* File: arm/op_xor_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    eor     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_int_2addr: /* 0xb8 */
/* File: arm/op_shl_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, asl r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_int_2addr: /* 0xb9 */
/* File: arm/op_shr_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, asr r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_2addr: /* 0xba */
/* File: arm/op_ushr_int_2addr.S */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, lsr r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_long_2addr: /* 0xbb */
/* File: arm/op_add_long_2addr.S */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    adds    r0, r0, r2                           @ optional op; may set condition codes
    adc     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_sub_long_2addr: /* 0xbc */
/* File: arm/op_sub_long_2addr.S */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    subs    r0, r0, r2                           @ optional op; may set condition codes
    sbc     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_long_2addr: /* 0xbd */
/* File: arm/op_mul_long_2addr.S */
    /*
     * Signed 64-bit integer multiply, "/2addr" version.
     *
     * See op_mul_long for an explanation.
     *
     * We get a little tight on registers, so to avoid looking up &fp[A]
     * again we stuff it into rINST.
     */
    /* mul-long/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR rINST, r9        @ rINST<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   rINST, {r0-r1}              @ r0/r1<- vAA/vAA+1
    mul     ip, r2, r1                  @ ip<- ZxW
    umull   r1, lr, r2, r0              @ r1/lr <- ZxX
    mla     r2, r0, r3, ip              @ r2<- YxX + (ZxW)
    mov     r0, rINST                   @ r0<- &fp[A] (free up rINST)
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    add     r2, r2, lr                  @ r2<- r2 + low(ZxW + (YxX))
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r0, {r1-r2}                 @ vAA/vAA+1<- r1/r2
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_div_long_2addr: /* 0xbe */
/* File: arm/op_div_long_2addr.S */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 1
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      __aeabi_ldivmod                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rem_long_2addr: /* 0xbf */
/* File: arm/op_rem_long_2addr.S */
/* ldivmod returns quotient in r0/r1 and remainder in r2/r3 */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 1
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      __aeabi_ldivmod                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r2,r3}     @ vAA/vAA+1<- r2/r3
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_and_long_2addr: /* 0xc0 */
/* File: arm/op_and_long_2addr.S */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    and     r0, r0, r2                           @ optional op; may set condition codes
    and     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_long_2addr: /* 0xc1 */
/* File: arm/op_or_long_2addr.S */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    orr     r0, r0, r2                           @ optional op; may set condition codes
    orr     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_long_2addr: /* 0xc2 */
/* File: arm/op_xor_long_2addr.S */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    eor     r0, r0, r2                           @ optional op; may set condition codes
    eor     r1, r1, r3                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_long_2addr: /* 0xc3 */
/* File: arm/op_shl_long_2addr.S */
    /*
     * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
     * 32-bit shift distance.
     */
    /* shl-long/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r2, r3                     @ r2<- vB
    CLEAR_SHADOW_PAIR r9, lr, ip        @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[A]
    and     r2, r2, #63                 @ r2<- r2 & 0x3f
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    mov     r1, r1, asl r2              @ r1<- r1 << r2
    rsb     r3, r2, #32                 @ r3<- 32 - r2
    orr     r1, r1, r0, lsr r3          @ r1<- r1 | (r0 << (32-r2))
    subs    ip, r2, #32                 @ ip<- r2 - 32
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    movpl   r1, r0, asl ip              @ if r2 >= 32, r1<- r0 << (r2-32)
    mov     r0, r0, asl r2              @ r0<- r0 << r2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_shr_long_2addr: /* 0xc4 */
/* File: arm/op_shr_long_2addr.S */
    /*
     * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
     * 32-bit shift distance.
     */
    /* shr-long/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r2, r3                     @ r2<- vB
    CLEAR_SHADOW_PAIR r9, lr, ip        @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[A]
    and     r2, r2, #63                 @ r2<- r2 & 0x3f
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    mov     r0, r0, lsr r2              @ r0<- r2 >> r2
    rsb     r3, r2, #32                 @ r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @ r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @ ip<- r2 - 32
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    movpl   r0, r1, asr ip              @ if r2 >= 32, r0<-r1 >> (r2-32)
    mov     r1, r1, asr r2              @ r1<- r1 >> r2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_ushr_long_2addr: /* 0xc5 */
/* File: arm/op_ushr_long_2addr.S */
    /*
     * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
     * 32-bit shift distance.
     */
    /* ushr-long/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r2, r3                     @ r2<- vB
    CLEAR_SHADOW_PAIR r9, lr, ip        @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[A]
    and     r2, r2, #63                 @ r2<- r2 & 0x3f
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    mov     r0, r0, lsr r2              @ r0<- r2 >> r2
    rsb     r3, r2, #32                 @ r3<- 32 - r2
    orr     r0, r0, r1, asl r3          @ r0<- r0 | (r1 << (32-r2))
    subs    ip, r2, #32                 @ ip<- r2 - 32
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    movpl   r0, r1, lsr ip              @ if r2 >= 32, r0<-r1 >>> (r2-32)
    mov     r1, r1, lsr r2              @ r1<- r1 >>> r2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0-r1}                 @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_add_float_2addr: /* 0xc6 */
/* File: arm/op_add_float_2addr.S */
/* File: arm/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    flds    s1, [r3]                    @ s1<- vB
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    flds    s0, [r9]                    @ s0<- vA
    fadds   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sub_float_2addr: /* 0xc7 */
/* File: arm/op_sub_float_2addr.S */
/* File: arm/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    flds    s1, [r3]                    @ s1<- vB
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    flds    s0, [r9]                    @ s0<- vA
    fsubs   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_mul_float_2addr: /* 0xc8 */
/* File: arm/op_mul_float_2addr.S */
/* File: arm/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    flds    s1, [r3]                    @ s1<- vB
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    flds    s0, [r9]                    @ s0<- vA
    fmuls   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_div_float_2addr: /* 0xc9 */
/* File: arm/op_div_float_2addr.S */
/* File: arm/fbinop2addr.S */
    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    flds    s1, [r3]                    @ s1<- vB
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    flds    s0, [r9]                    @ s0<- vA
    fdivs   s2, s0, s1                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_rem_float_2addr: /* 0xca */
/* File: arm/op_rem_float_2addr.S */
/* EABI doesn't define a float remainder function, but libm does */
/* File: arm/binop2addr.S */
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r1, r3                     @ r1<- vB
    GET_VREG r0, r9                     @ r0<- vA
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    bl      fmodf                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_double_2addr: /* 0xcb */
/* File: arm/op_add_double_2addr.S */
/* File: arm/fbinopWide2addr.S */
    /*
     * Generic 64-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr,
     *      div-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    CLEAR_SHADOW_PAIR r9, ip, r0        @ Zero out shadow regs
    fldd    d1, [r3]                    @ d1<- vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fldd    d0, [r9]                    @ d0<- vA
    faddd   d2, d0, d1                              @ d2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_sub_double_2addr: /* 0xcc */
/* File: arm/op_sub_double_2addr.S */
/* File: arm/fbinopWide2addr.S */
    /*
     * Generic 64-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr,
     *      div-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    CLEAR_SHADOW_PAIR r9, ip, r0        @ Zero out shadow regs
    fldd    d1, [r3]                    @ d1<- vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fldd    d0, [r9]                    @ d0<- vA
    fsubd   d2, d0, d1                              @ d2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_mul_double_2addr: /* 0xcd */
/* File: arm/op_mul_double_2addr.S */
/* File: arm/fbinopWide2addr.S */
    /*
     * Generic 64-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr,
     *      div-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    CLEAR_SHADOW_PAIR r9, ip, r0        @ Zero out shadow regs
    fldd    d1, [r3]                    @ d1<- vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fldd    d0, [r9]                    @ d0<- vA
    fmuld   d2, d0, d1                              @ d2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_div_double_2addr: /* 0xce */
/* File: arm/op_div_double_2addr.S */
/* File: arm/fbinopWide2addr.S */
    /*
     * Generic 64-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr,
     *      div-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    CLEAR_SHADOW_PAIR r9, ip, r0        @ Zero out shadow regs
    fldd    d1, [r3]                    @ d1<- vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fldd    d0, [r9]                    @ d0<- vA
    fdivd   d2, d0, d1                              @ d2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_rem_double_2addr: /* 0xcf */
/* File: arm/op_rem_double_2addr.S */
/* EABI doesn't define a double remainder function, but libm does */
/* File: arm/binopWide2addr.S */
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, div-long/2addr, rem-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr,
     *      rem-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   r9, {r0-r1}                 @ r0/r1<- vAA/vAA+1
    .if 0
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
                               @ optional op; may set condition codes
    bl      fmod                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {r0,r1}     @ vAA/vAA+1<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 12-15 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_int_lit16: /* 0xd0 */
/* File: arm/op_add_int_lit16.S */
/* File: arm/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    add     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rsub_int: /* 0xd1 */
/* File: arm/op_rsub_int.S */
/* this op is "rsub-int", but can be thought of as "rsub-int/lit16" */
/* File: arm/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    rsb     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit16: /* 0xd2 */
/* File: arm/op_mul_int_lit16.S */
/* must be "mul r0, r1, r0" -- "r0, r0, r1" is illegal */
/* File: arm/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    mul     r0, r1, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int_lit16: /* 0xd3 */
/* File: arm/op_div_int_lit16.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r0 = r0 div r1". The selection between sdiv or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * div-int/lit16
     *
     */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r0, r0, r1                  @ r0<- op
#else
    bl       __aeabi_idiv               @ r0<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */

/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit16: /* 0xd4 */
/* File: arm/op_rem_int_lit16.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r1 = r0 rem r1". The selection between sdiv block or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * NOTE: idivmod returns quotient in r0 and remainder in r1
     *
     * rem-int/lit16
     *
     */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r2, r0, r1
    mls     r1, r1, r2, r0              @ r1<- op
#else
    bl     __aeabi_idivmod              @ r1<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r1, r9                     @ vAA<- r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */

/* ------------------------------ */
    .balign 128
.L_op_and_int_lit16: /* 0xd5 */
/* File: arm/op_and_int_lit16.S */
/* File: arm/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    and     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit16: /* 0xd6 */
/* File: arm/op_or_int_lit16.S */
/* File: arm/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    orr     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit16: /* 0xd7 */
/* File: arm/op_xor_int_lit16.S */
/* File: arm/binopLit16.S */
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    .if 0
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    eor     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */


/* ------------------------------ */
    .balign 128
.L_op_add_int_lit8: /* 0xd8 */
/* File: arm/op_add_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    add     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_rsub_int_lit8: /* 0xd9 */
/* File: arm/op_rsub_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    rsb     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_mul_int_lit8: /* 0xda */
/* File: arm/op_mul_int_lit8.S */
/* must be "mul r0, r1, r0" -- "r0, r0, r1" is illegal */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    mul     r0, r1, r0                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_div_int_lit8: /* 0xdb */
/* File: arm/op_div_int_lit8.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r0 = r0 div r1". The selection between sdiv or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * div-int/lit8
     *
     */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r0, r0, r1                  @ r0<- op
#else
    bl   __aeabi_idiv                   @ r0<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */

/* ------------------------------ */
    .balign 128
.L_op_rem_int_lit8: /* 0xdc */
/* File: arm/op_rem_int_lit8.S */
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r1 = r0 rem r1". The selection between sdiv block or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * NOTE: idivmod returns quotient in r0 and remainder in r1
     *
     * rem-int/lit8
     *
     */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r2, r0, r1
    mls     r1, r1, r2, r0              @ r1<- op
#else
    bl       __aeabi_idivmod            @ r1<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r1, r9                     @ vAA<- r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */

/* ------------------------------ */
    .balign 128
.L_op_and_int_lit8: /* 0xdd */
/* File: arm/op_and_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    and     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_or_int_lit8: /* 0xde */
/* File: arm/op_or_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    orr     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_xor_int_lit8: /* 0xdf */
/* File: arm/op_xor_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

                               @ optional op; may set condition codes
    eor     r0, r0, r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shl_int_lit8: /* 0xe0 */
/* File: arm/op_shl_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, asl r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_shr_int_lit8: /* 0xe1 */
/* File: arm/op_shr_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, asr r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_ushr_int_lit8: /* 0xe2 */
/* File: arm/op_ushr_int_lit8.S */
/* File: arm/binopLit8.S */
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = r0 op r1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    .if 0
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

    and     r1, r1, #31                           @ optional op; may set condition codes
    mov     r0, r0, lsr r1                              @ r0<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */


/* ------------------------------ */
    .balign 128
.L_op_iget_quick: /* 0xe3 */
/* File: arm/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- object we're operating on
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    ldr   r0, [r3, r1]                @ r0<- obj.field
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r2                     @ fp[A]<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_wide_quick: /* 0xe4 */
/* File: arm/op_iget_wide_quick.S */
    /* iget-wide-quick vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH ip, 1                         @ ip<- field byte offset
    GET_VREG r3, r2                     @ r3<- object we're operating on
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    ldrd    r0, [r3, ip]                @ r0<- obj.field (64 bits, aligned)
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    VREG_INDEX_TO_ADDR r3, r2           @ r3<- &fp[A]
    CLEAR_SHADOW_PAIR r2, ip, lr        @ Zero out the shadow regs
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r3, {r0-r1}                 @ fp[A]<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iget_object_quick: /* 0xe5 */
/* File: arm/op_iget_object_quick.S */
    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    EXPORT_PC
    GET_VREG r0, r2                     @ r0<- object we're operating on
    bl      artIGetObjectFromMterp      @ (obj, offset)
    ldr     r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx    r2, rINST, #8, #4           @ r2<- A
    PREFETCH_INST 2
    cmp     r3, #0
    bne     MterpPossibleException      @ bail out
    SET_VREG_OBJECT r0, r2              @ fp[A]<- r0
    ADVANCE 2                           @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_quick: /* 0xe6 */
/* File: arm/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- fp[B], the object pointer
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    GET_VREG r0, r2                     @ r0<- fp[A]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    str     r0, [r3, r1]             @ obj.field<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_wide_quick: /* 0xe7 */
/* File: arm/op_iput_wide_quick.S */
    /* iput-wide-quick vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r3, 1                         @ r3<- field byte offset
    GET_VREG r2, r2                     @ r2<- fp[B], the object pointer
    ubfx    r0, rINST, #8, #4           @ r0<- A
    cmp     r2, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    VREG_INDEX_TO_ADDR r0, r0           @ r0<- &fp[A]
    ldmia   r0, {r0-r1}                 @ r0/r1<- fp[A]/fp[A+1]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    strd    r0, [r2, r3]                @ obj.field<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_iput_object_quick: /* 0xe8 */
/* File: arm/op_iput_object_quick.S */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    bl      MterpIputObjectQuick
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_quick: /* 0xe9 */
/* File: arm/op_invoke_virtual_quick.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuick
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeVirtualQuick
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_invoke_virtual_range_quick: /* 0xea */
/* File: arm/op_invoke_virtual_range_quick.S */
/* File: arm/invoke.S */
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern MterpInvokeVirtualQuickRange
    EXPORT_PC
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rPC
    mov     r3, rINST
    bl      MterpInvokeVirtualQuickRange
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 3
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    GET_INST_OPCODE ip
    GOTO_OPCODE ip



/* ------------------------------ */
    .balign 128
.L_op_iput_boolean_quick: /* 0xeb */
/* File: arm/op_iput_boolean_quick.S */
/* File: arm/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- fp[B], the object pointer
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    GET_VREG r0, r2                     @ r0<- fp[A]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    strb     r0, [r3, r1]             @ obj.field<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_byte_quick: /* 0xec */
/* File: arm/op_iput_byte_quick.S */
/* File: arm/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- fp[B], the object pointer
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    GET_VREG r0, r2                     @ r0<- fp[A]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    strb     r0, [r3, r1]             @ obj.field<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_char_quick: /* 0xed */
/* File: arm/op_iput_char_quick.S */
/* File: arm/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- fp[B], the object pointer
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    GET_VREG r0, r2                     @ r0<- fp[A]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    strh     r0, [r3, r1]             @ obj.field<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iput_short_quick: /* 0xee */
/* File: arm/op_iput_short_quick.S */
/* File: arm/op_iput_quick.S */
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- fp[B], the object pointer
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    GET_VREG r0, r2                     @ r0<- fp[A]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    strh     r0, [r3, r1]             @ obj.field<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_boolean_quick: /* 0xef */
/* File: arm/op_iget_boolean_quick.S */
/* File: arm/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- object we're operating on
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    ldrb   r0, [r3, r1]                @ r0<- obj.field
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r2                     @ fp[A]<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_byte_quick: /* 0xf0 */
/* File: arm/op_iget_byte_quick.S */
/* File: arm/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- object we're operating on
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    ldrsb   r0, [r3, r1]                @ r0<- obj.field
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r2                     @ fp[A]<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_char_quick: /* 0xf1 */
/* File: arm/op_iget_char_quick.S */
/* File: arm/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- object we're operating on
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    ldrh   r0, [r3, r1]                @ r0<- obj.field
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r2                     @ fp[A]<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_iget_short_quick: /* 0xf2 */
/* File: arm/op_iget_short_quick.S */
/* File: arm/op_iget_quick.S */
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r1, 1                         @ r1<- field byte offset
    GET_VREG r3, r2                     @ r3<- object we're operating on
    ubfx    r2, rINST, #8, #4           @ r2<- A
    cmp     r3, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    ldrsh   r0, [r3, r1]                @ r0<- obj.field
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG r0, r2                     @ fp[A]<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction


/* ------------------------------ */
    .balign 128
.L_op_invoke_lambda: /* 0xf3 */
/* Transfer stub to alternate interpreter */
    b    MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_f4: /* 0xf4 */
/* File: arm/op_unused_f4.S */
/* File: arm/unused.S */
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
/* File: arm/op_unused_fa.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fb: /* 0xfb */
/* File: arm/op_unused_fb.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fc: /* 0xfc */
/* File: arm/op_unused_fc.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fd: /* 0xfd */
/* File: arm/op_unused_fd.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_fe: /* 0xfe */
/* File: arm/op_unused_fe.S */
/* File: arm/unused.S */
/*
 * Bail to reference interpreter to throw.
 */
  b MterpFallback


/* ------------------------------ */
    .balign 128
.L_op_unused_ff: /* 0xff */
/* File: arm/op_unused_ff.S */
/* File: arm/unused.S */
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

/* continuation for op_float_to_long */
/*
 * Convert the float in r0 to a long in r0/r1.
 *
 * We have to clip values to long min/max per the specification.  The
 * expected common case is a "reasonable" value that converts directly
 * to modest integer.  The EABI convert function isn't doing this for us.
 */
f2l_doconv:
    stmfd   sp!, {r4, lr}
    mov     r1, #0x5f000000             @ (float)maxlong
    mov     r4, r0
    bl      __aeabi_fcmpge              @ is arg >= maxlong?
    cmp     r0, #0                      @ nonzero == yes
    mvnne   r0, #0                      @ return maxlong (7fffffff)
    mvnne   r1, #0x80000000
    popne   {r4, pc}

    mov     r0, r4                      @ recover arg
    mov     r1, #0xdf000000             @ (float)minlong
    bl      __aeabi_fcmple              @ is arg <= minlong?
    cmp     r0, #0                      @ nonzero == yes
    movne   r0, #0                      @ return minlong (80000000)
    movne   r1, #0x80000000
    popne   {r4, pc}

    mov     r0, r4                      @ recover arg
    mov     r1, r4
    bl      __aeabi_fcmpeq              @ is arg == self?
    cmp     r0, #0                      @ zero == no
    moveq   r1, #0                      @ return zero for NaN
    popeq   {r4, pc}

    mov     r0, r4                      @ recover arg
    bl      __aeabi_f2lz                @ convert float to long
    ldmfd   sp!, {r4, pc}

/* continuation for op_double_to_long */
/*
 * Convert the double in r0/r1 to a long in r0/r1.
 *
 * We have to clip values to long min/max per the specification.  The
 * expected common case is a "reasonable" value that converts directly
 * to modest integer.  The EABI convert function isn't doing this for us.
 */
d2l_doconv:
    stmfd   sp!, {r4, r5, lr}           @ save regs
    mov     r3, #0x43000000             @ maxlong, as a double (high word)
    add     r3, #0x00e00000             @  0x43e00000
    mov     r2, #0                      @ maxlong, as a double (low word)
    sub     sp, sp, #4                  @ align for EABI
    mov     r4, r0                      @ save a copy of r0
    mov     r5, r1                      @  and r1
    bl      __aeabi_dcmpge              @ is arg >= maxlong?
    cmp     r0, #0                      @ nonzero == yes
    mvnne   r0, #0                      @ return maxlong (7fffffffffffffff)
    mvnne   r1, #0x80000000
    bne     1f

    mov     r0, r4                      @ recover arg
    mov     r1, r5
    mov     r3, #0xc3000000             @ minlong, as a double (high word)
    add     r3, #0x00e00000             @  0xc3e00000
    mov     r2, #0                      @ minlong, as a double (low word)
    bl      __aeabi_dcmple              @ is arg <= minlong?
    cmp     r0, #0                      @ nonzero == yes
    movne   r0, #0                      @ return minlong (8000000000000000)
    movne   r1, #0x80000000
    bne     1f

    mov     r0, r4                      @ recover arg
    mov     r1, r5
    mov     r2, r4                      @ compare against self
    mov     r3, r5
    bl      __aeabi_dcmpeq              @ is arg == self?
    cmp     r0, #0                      @ zero == no
    moveq   r1, #0                      @ return zero for NaN
    beq     1f

    mov     r0, r4                      @ recover arg
    mov     r1, r5
    bl      __aeabi_d2lz                @ convert double to long

1:
    add     sp, sp, #4
    ldmfd   sp!, {r4, r5, pc}

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
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (0 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move: /* 0x01 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (1 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_from16: /* 0x02 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (2 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_16: /* 0x03 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (3 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide: /* 0x04 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (4 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_from16: /* 0x05 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (5 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_wide_16: /* 0x06 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (6 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object: /* 0x07 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (7 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_from16: /* 0x08 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (8 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_object_16: /* 0x09 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (9 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result: /* 0x0a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (10 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_wide: /* 0x0b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (11 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_result_object: /* 0x0c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (12 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_move_exception: /* 0x0d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (13 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void: /* 0x0e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (14 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return: /* 0x0f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (15 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_wide: /* 0x10 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (16 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_object: /* 0x11 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (17 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_4: /* 0x12 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (18 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_16: /* 0x13 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (19 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const: /* 0x14 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (20 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_high16: /* 0x15 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (21 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_16: /* 0x16 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (22 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_32: /* 0x17 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (23 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide: /* 0x18 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (24 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_wide_high16: /* 0x19 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (25 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string: /* 0x1a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (26 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_string_jumbo: /* 0x1b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (27 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_const_class: /* 0x1c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (28 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_enter: /* 0x1d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (29 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_monitor_exit: /* 0x1e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (30 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_check_cast: /* 0x1f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (31 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_instance_of: /* 0x20 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (32 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_array_length: /* 0x21 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (33 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_instance: /* 0x22 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (34 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_new_array: /* 0x23 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (35 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array: /* 0x24 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (36 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_filled_new_array_range: /* 0x25 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (37 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_fill_array_data: /* 0x26 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (38 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_throw: /* 0x27 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (39 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto: /* 0x28 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (40 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_16: /* 0x29 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (41 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_goto_32: /* 0x2a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (42 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_packed_switch: /* 0x2b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (43 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sparse_switch: /* 0x2c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (44 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_float: /* 0x2d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (45 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_float: /* 0x2e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (46 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpl_double: /* 0x2f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (47 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmpg_double: /* 0x30 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (48 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_cmp_long: /* 0x31 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (49 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eq: /* 0x32 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (50 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ne: /* 0x33 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (51 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lt: /* 0x34 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (52 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ge: /* 0x35 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (53 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gt: /* 0x36 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (54 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_le: /* 0x37 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (55 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_eqz: /* 0x38 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (56 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_nez: /* 0x39 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (57 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_ltz: /* 0x3a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (58 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gez: /* 0x3b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (59 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_gtz: /* 0x3c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (60 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_if_lez: /* 0x3d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (61 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3e: /* 0x3e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (62 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_3f: /* 0x3f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (63 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_40: /* 0x40 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (64 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_41: /* 0x41 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (65 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_42: /* 0x42 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (66 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_43: /* 0x43 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (67 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget: /* 0x44 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (68 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_wide: /* 0x45 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (69 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_object: /* 0x46 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (70 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_boolean: /* 0x47 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (71 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_byte: /* 0x48 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (72 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_char: /* 0x49 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (73 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aget_short: /* 0x4a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (74 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput: /* 0x4b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (75 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_wide: /* 0x4c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (76 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_object: /* 0x4d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (77 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_boolean: /* 0x4e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (78 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_byte: /* 0x4f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (79 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_char: /* 0x50 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (80 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_aput_short: /* 0x51 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (81 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget: /* 0x52 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (82 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide: /* 0x53 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (83 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object: /* 0x54 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (84 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean: /* 0x55 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (85 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte: /* 0x56 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (86 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char: /* 0x57 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (87 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short: /* 0x58 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (88 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput: /* 0x59 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (89 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide: /* 0x5a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (90 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object: /* 0x5b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (91 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean: /* 0x5c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (92 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte: /* 0x5d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (93 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char: /* 0x5e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (94 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short: /* 0x5f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (95 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget: /* 0x60 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (96 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_wide: /* 0x61 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (97 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_object: /* 0x62 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (98 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_boolean: /* 0x63 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (99 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_byte: /* 0x64 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (100 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_char: /* 0x65 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (101 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sget_short: /* 0x66 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (102 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput: /* 0x67 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (103 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_wide: /* 0x68 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (104 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_object: /* 0x69 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (105 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_boolean: /* 0x6a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (106 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_byte: /* 0x6b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (107 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_char: /* 0x6c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (108 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sput_short: /* 0x6d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (109 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual: /* 0x6e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (110 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super: /* 0x6f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (111 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct: /* 0x70 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (112 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static: /* 0x71 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (113 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface: /* 0x72 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (114 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_return_void_no_barrier: /* 0x73 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (115 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range: /* 0x74 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (116 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_super_range: /* 0x75 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (117 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_direct_range: /* 0x76 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (118 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_static_range: /* 0x77 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (119 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_interface_range: /* 0x78 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (120 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_79: /* 0x79 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (121 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_7a: /* 0x7a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (122 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_int: /* 0x7b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (123 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_int: /* 0x7c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (124 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_long: /* 0x7d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (125 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_not_long: /* 0x7e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (126 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_float: /* 0x7f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (127 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_neg_double: /* 0x80 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (128 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_long: /* 0x81 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (129 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_float: /* 0x82 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (130 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_double: /* 0x83 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (131 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_int: /* 0x84 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (132 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_float: /* 0x85 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (133 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_long_to_double: /* 0x86 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (134 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_int: /* 0x87 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (135 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_long: /* 0x88 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (136 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_float_to_double: /* 0x89 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (137 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_int: /* 0x8a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (138 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_long: /* 0x8b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (139 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_double_to_float: /* 0x8c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (140 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_byte: /* 0x8d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (141 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_char: /* 0x8e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (142 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_int_to_short: /* 0x8f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (143 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int: /* 0x90 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (144 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int: /* 0x91 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (145 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int: /* 0x92 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (146 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int: /* 0x93 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (147 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int: /* 0x94 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (148 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int: /* 0x95 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (149 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int: /* 0x96 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (150 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int: /* 0x97 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (151 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int: /* 0x98 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (152 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int: /* 0x99 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (153 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int: /* 0x9a */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (154 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long: /* 0x9b */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (155 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long: /* 0x9c */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (156 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long: /* 0x9d */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (157 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long: /* 0x9e */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (158 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long: /* 0x9f */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (159 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long: /* 0xa0 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (160 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long: /* 0xa1 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (161 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long: /* 0xa2 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (162 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long: /* 0xa3 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (163 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long: /* 0xa4 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (164 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long: /* 0xa5 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (165 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float: /* 0xa6 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (166 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float: /* 0xa7 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (167 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float: /* 0xa8 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (168 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float: /* 0xa9 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (169 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float: /* 0xaa */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (170 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double: /* 0xab */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (171 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double: /* 0xac */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (172 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double: /* 0xad */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (173 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double: /* 0xae */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (174 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double: /* 0xaf */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (175 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_2addr: /* 0xb0 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (176 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_int_2addr: /* 0xb1 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (177 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_2addr: /* 0xb2 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (178 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_2addr: /* 0xb3 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (179 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_2addr: /* 0xb4 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (180 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_2addr: /* 0xb5 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (181 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_2addr: /* 0xb6 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (182 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_2addr: /* 0xb7 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (183 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_2addr: /* 0xb8 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (184 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_2addr: /* 0xb9 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (185 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_2addr: /* 0xba */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (186 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_long_2addr: /* 0xbb */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (187 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_long_2addr: /* 0xbc */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (188 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_long_2addr: /* 0xbd */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (189 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_long_2addr: /* 0xbe */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (190 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_long_2addr: /* 0xbf */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (191 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_long_2addr: /* 0xc0 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (192 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_long_2addr: /* 0xc1 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (193 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_long_2addr: /* 0xc2 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (194 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_long_2addr: /* 0xc3 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (195 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_long_2addr: /* 0xc4 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (196 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_long_2addr: /* 0xc5 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (197 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_float_2addr: /* 0xc6 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (198 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_float_2addr: /* 0xc7 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (199 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_float_2addr: /* 0xc8 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (200 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_float_2addr: /* 0xc9 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (201 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_float_2addr: /* 0xca */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (202 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_double_2addr: /* 0xcb */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (203 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_sub_double_2addr: /* 0xcc */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (204 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_double_2addr: /* 0xcd */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (205 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_double_2addr: /* 0xce */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (206 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_double_2addr: /* 0xcf */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (207 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit16: /* 0xd0 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (208 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int: /* 0xd1 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (209 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit16: /* 0xd2 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (210 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit16: /* 0xd3 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (211 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit16: /* 0xd4 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (212 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit16: /* 0xd5 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (213 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit16: /* 0xd6 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (214 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit16: /* 0xd7 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (215 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_add_int_lit8: /* 0xd8 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (216 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rsub_int_lit8: /* 0xd9 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (217 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_mul_int_lit8: /* 0xda */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (218 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_div_int_lit8: /* 0xdb */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (219 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_rem_int_lit8: /* 0xdc */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (220 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_and_int_lit8: /* 0xdd */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (221 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_or_int_lit8: /* 0xde */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (222 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_xor_int_lit8: /* 0xdf */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (223 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shl_int_lit8: /* 0xe0 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (224 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_shr_int_lit8: /* 0xe1 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (225 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_ushr_int_lit8: /* 0xe2 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (226 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_quick: /* 0xe3 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (227 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_wide_quick: /* 0xe4 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (228 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_object_quick: /* 0xe5 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (229 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_quick: /* 0xe6 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (230 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_wide_quick: /* 0xe7 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (231 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_object_quick: /* 0xe8 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (232 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_quick: /* 0xe9 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (233 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_virtual_range_quick: /* 0xea */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (234 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_boolean_quick: /* 0xeb */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (235 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_byte_quick: /* 0xec */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (236 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_char_quick: /* 0xed */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (237 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iput_short_quick: /* 0xee */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (238 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_boolean_quick: /* 0xef */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (239 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_byte_quick: /* 0xf0 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (240 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_char_quick: /* 0xf1 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (241 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_iget_short_quick: /* 0xf2 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (242 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_invoke_lambda: /* 0xf3 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (243 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_f4: /* 0xf4 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (244 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_capture_variable: /* 0xf5 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (245 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_create_lambda: /* 0xf6 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (246 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_liberate_variable: /* 0xf7 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (247 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_box_lambda: /* 0xf8 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (248 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unbox_lambda: /* 0xf9 */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (249 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fa: /* 0xfa */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (250 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fb: /* 0xfb */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (251 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fc: /* 0xfc */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (252 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fd: /* 0xfd */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (253 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_fe: /* 0xfe */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (254 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

/* ------------------------------ */
    .balign 128
.L_ALT_op_unused_ff: /* 0xff */
/* File: arm/alt_stub.S */
/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    ldr    rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]            @ refresh IBASE.
    adrl   lr, artMterpAsmInstructionStart + (255 * 128)       @ Addr of primary handler.
    mov    r0, rSELF
    add    r1, rFP, #OFF_FP_SHADOWFRAME
    b      MterpCheckBefore     @ (self, shadow_frame)              @ Tail call.

    .balign 128
    .size   artMterpAsmAltInstructionStart, .-artMterpAsmAltInstructionStart
    .global artMterpAsmAltInstructionEnd
artMterpAsmAltInstructionEnd:
/* File: arm/footer.S */
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
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogDivideByZeroException
#endif
    b MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogArrayIndexException
#endif
    b MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNegativeArraySizeException
#endif
    b MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNoSuchMethodException
#endif
    b MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNullObjectException
#endif
    b MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogExceptionThrownException
#endif
    b MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    ldr  r2, [rSELF, #THREAD_FLAGS_OFFSET]
    bl MterpLogSuspendFallback
#endif
    b MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    ldr     r0, [rSELF, #THREAD_EXCEPTION_OFFSET]
    cmp     r0, #0                                  @ Exception pending?
    beq     MterpFallback                           @ If not, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    bl      MterpHandleException                    @ (self, shadow_frame)
    cmp     r0, #0
    beq     MterpExceptionReturn                    @ no local catch, back to caller.
    ldr     r0, [rFP, #OFF_FP_CODE_ITEM]
    ldr     r1, [rFP, #OFF_FP_DEX_PC]
    ldr     rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]
    add     rPC, r0, #CODEITEM_INSNS_OFFSET
    add     rPC, rPC, r1, lsl #1                    @ generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    /* resume execution at catch block */
    EXPORT_PC
    FETCH_INST
    GET_INST_OPCODE ip
    GOTO_OPCODE ip
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
MterpCommonTakenBranchNoFlags:
    cmp     rINST, #0
MterpCommonTakenBranch:
    bgt     .L_forward_branch           @ don't add forward branches to hotness
/*
 * We need to subtract 1 from positive values and we should not see 0 here,
 * so we may use the result of the comparison with -1.
 */
#if JIT_CHECK_OSR != -1
#  error "JIT_CHECK_OSR must be -1."
#endif
    cmp     rPROFILE, #JIT_CHECK_OSR
    beq     .L_osr_check
    subgts  rPROFILE, #1
    beq     .L_add_batch                @ counted down to zero - report
.L_resume_backward_branch:
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    REFRESH_IBASE
    add     r2, rINST, rINST            @ r2<- byte offset
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    bne     .L_suspend_request_pending
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

.L_suspend_request_pending:
    EXPORT_PC
    mov     r0, rSELF
    bl      MterpSuspendCheck           @ (self)
    cmp     r0, #0
    bne     MterpFallback
    REFRESH_IBASE                       @ might have changed during suspend
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

.L_no_count_backwards:
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    bne     .L_resume_backward_branch
.L_osr_check:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement
    b       .L_resume_backward_branch

.L_forward_branch:
    cmp     rPROFILE, #JIT_CHECK_OSR @ possible OSR re-entry?
    beq     .L_check_osr_forward
.L_resume_forward_branch:
    add     r2, rINST, rINST            @ r2<- byte offset
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

.L_check_osr_forward:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement
    b       .L_resume_forward_branch

.L_add_batch:
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    strh    rPROFILE, [r1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    ldr     r0, [rFP, #OFF_FP_METHOD]
    mov     r2, rSELF
    bl      MterpAddHotnessBatch        @ (method, shadow_frame, self)
    mov     rPROFILE, r0                @ restore new hotness countdown to rPROFILE
    b       .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, #2
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    mov r0, rSELF
    add r1, rFP, #OFF_FP_SHADOWFRAME
    mov r2, rINST
    bl MterpLogOSR
#endif
    mov r0, #1                          @ Signal normal return
    b MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogFallback
#endif
MterpCommonFallback:
    mov     r0, #0                                  @ signal retry with reference interpreter.
    b       MterpDone

/*
 * We pushed some registers on the stack in ExecuteMterpImpl, then saved
 * SP and LR.  Here we restore SP, restore the registers, and then restore
 * LR to PC.
 *
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    mov     r0, #1                                  @ signal return to caller.
    b MterpDone
MterpReturn:
    ldr     r2, [rFP, #OFF_FP_RESULT_REGISTER]
    str     r0, [r2]
    str     r1, [r2, #4]
    mov     r0, #1                                  @ signal return to caller.
MterpDone:
/*
 * At this point, we expect rPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending rPROFILE and the cached hotness counter).  rPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    cmp     rPROFILE, #0
    bgt     MterpProfileActive                      @ if > 0, we may have some counts to report.
    ldmfd   sp!, {r3-r10,fp,pc}                     @ restore 10 regs and return

MterpProfileActive:
    mov     rINST, r0                               @ stash return value
    /* Report cached hotness counts */
    ldr     r0, [rFP, #OFF_FP_METHOD]
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rSELF
    strh    rPROFILE, [r1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    bl      MterpAddHotnessBatch                    @ (method, shadow_frame, self)
    mov     r0, rINST                               @ restore return value
    ldmfd   sp!, {r3-r10,fp,pc}                     @ restore 10 regs and return

    .fnend
    .size   ExecuteMterpImpl, .-ExecuteMterpImpl


