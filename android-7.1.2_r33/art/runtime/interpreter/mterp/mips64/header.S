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

#include <machine/regdef.h>

/* TODO: add the missing file and use its FP register definitions. */
/* #include <machine/fpregdef.h> */
/* FP register definitions */
#define f0  $$f0
#define f1  $$f1
#define f2  $$f2
#define f3  $$f3
#define f12 $$f12
#define f13 $$f13

/*
 * It looks like the GNU assembler currently does not support the blec and bgtc
 * idioms, which should translate into bgec and bltc respectively with swapped
 * left and right register operands.
 * TODO: remove these macros when the assembler is fixed.
 */
.macro blec lreg, rreg, target
    bgec    \rreg, \lreg, \target
.endm
.macro bgtc lreg, rreg, target
    bltc    \rreg, \lreg, \target
.endm

/*
Mterp and MIPS64 notes:

The following registers have fixed assignments:

  reg nick      purpose
  s0  rPC       interpreted program counter, used for fetching instructions
  s1  rFP       interpreted frame pointer, used for accessing locals and args
  s2  rSELF     self (Thread) pointer
  s3  rINST     first 16-bit code unit of current instruction
  s4  rIBASE    interpreted instruction base pointer, used for computed goto
  s5  rREFS     base of object references in shadow frame  (ideally, we'll get rid of this later).
*/

/* During bringup, we'll use the shadow frame model instead of rFP */
/* single-purpose registers, given names for clarity */
#define rPC     s0
#define rFP     s1
#define rSELF   s2
#define rINST   s3
#define rIBASE  s4
#define rREFS   s5

/*
 * This is a #include, not a %include, because we want the C pre-processor
 * to expand the macros into assembler assignment statements.
 */
#include "asm_support.h"

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
#define OFF_FP_SHADOWFRAME (-SHADOWFRAME_VREGS_OFFSET)

#define MTERP_PROFILE_BRANCHES 1
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
    sd      rPC, OFF_FP_DEX_PC_PTR(rFP)
.endm

/*
 * Refresh handler table.
 */
.macro REFRESH_IBASE
    ld      rIBASE, THREAD_CURRENT_IBASE_OFFSET(rSELF)
.endm

/*
 * Fetch the next instruction from rPC into rINST.  Does not advance rPC.
 */
.macro FETCH_INST
    lhu     rINST, 0(rPC)
.endm

/* Advance rPC by some number of code units. */
.macro ADVANCE count
    daddu   rPC, rPC, (\count) * 2
.endm

/*
 * Fetch the next instruction from the specified offset.  Advances rPC
 * to point to the next instruction.
 *
 * This must come AFTER anything that can throw an exception, or the
 * exception catch may miss.  (This also implies that it must come after
 * EXPORT_PC.)
 */
.macro FETCH_ADVANCE_INST count
    ADVANCE \count
    FETCH_INST
.endm

/*
 * Similar to FETCH_ADVANCE_INST, but does not update rPC.  Used to load
 * rINST ahead of possible exception point.  Be sure to manually advance rPC
 * later.
 */
.macro PREFETCH_INST count
    lhu     rINST, ((\count) * 2)(rPC)
.endm

/*
 * Put the instruction's opcode field into the specified register.
 */
.macro GET_INST_OPCODE reg
    and     \reg, rINST, 255
.endm

/*
 * Begin executing the opcode in _reg.
 */
.macro GOTO_OPCODE reg
    .set noat
    sll     AT, \reg, 7
    daddu   AT, rIBASE, AT
    jic     AT, 0
    .set at
.endm

/*
 * Get/set the 32-bit value from a Dalvik register.
 * Note, GET_VREG does sign extension to 64 bits while
 * GET_VREG_U does zero extension to 64 bits.
 * One is useful for arithmetic while the other is
 * useful for storing the result value as 64-bit.
 */
.macro GET_VREG reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    lw      \reg, 0(AT)
    .set at
.endm
.macro GET_VREG_U reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    lwu     \reg, 0(AT)
    .set at
.endm
.macro GET_VREG_FLOAT reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    lwc1    \reg, 0(AT)
    .set at
.endm
.macro SET_VREG reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    sw      \reg, 0(AT)
    dlsa    AT, \vreg, rREFS, 2
    sw      zero, 0(AT)
    .set at
.endm
.macro SET_VREG_OBJECT reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    sw      \reg, 0(AT)
    dlsa    AT, \vreg, rREFS, 2
    sw      \reg, 0(AT)
    .set at
.endm
.macro SET_VREG_FLOAT reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    swc1    \reg, 0(AT)
    dlsa    AT, \vreg, rREFS, 2
    sw      zero, 0(AT)
    .set at
.endm

/*
 * Get/set the 64-bit value from a Dalvik register.
 * Avoid unaligned memory accesses.
 * Note, SET_VREG_WIDE clobbers the register containing the value being stored.
 * Note, SET_VREG_DOUBLE clobbers the register containing the Dalvik register number.
 */
.macro GET_VREG_WIDE reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    lw      \reg, 0(AT)
    lw      AT, 4(AT)
    dinsu   \reg, AT, 32, 32
    .set at
.endm
.macro GET_VREG_DOUBLE reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    lwc1    \reg, 0(AT)
    lw      AT, 4(AT)
    mthc1   AT, \reg
    .set at
.endm
.macro SET_VREG_WIDE reg, vreg
    .set noat
    dlsa    AT, \vreg, rFP, 2
    sw      \reg, 0(AT)
    drotr32 \reg, \reg, 0
    sw      \reg, 4(AT)
    dlsa    AT, \vreg, rREFS, 2
    sw      zero, 0(AT)
    sw      zero, 4(AT)
    .set at
.endm
.macro SET_VREG_DOUBLE reg, vreg
    .set noat
    dlsa    AT, \vreg, rREFS, 2
    sw      zero, 0(AT)
    sw      zero, 4(AT)
    dlsa    AT, \vreg, rFP, 2
    swc1    \reg, 0(AT)
    mfhc1   \vreg, \reg
    sw      \vreg, 4(AT)
    .set at
.endm

/*
 * On-stack offsets for spilling/unspilling callee-saved registers
 * and the frame size.
 */
#define STACK_OFFSET_RA 0
#define STACK_OFFSET_GP 8
#define STACK_OFFSET_S0 16
#define STACK_OFFSET_S1 24
#define STACK_OFFSET_S2 32
#define STACK_OFFSET_S3 40
#define STACK_OFFSET_S4 48
#define STACK_OFFSET_S5 56
#define STACK_SIZE      64

/* Constants for float/double_to_int/long conversions */
#define INT_MIN             0x80000000
#define INT_MIN_AS_FLOAT    0xCF000000
#define INT_MIN_AS_DOUBLE   0xC1E0000000000000
#define LONG_MIN            0x8000000000000000
#define LONG_MIN_AS_FLOAT   0xDF000000
#define LONG_MIN_AS_DOUBLE  0xC3E0000000000000
