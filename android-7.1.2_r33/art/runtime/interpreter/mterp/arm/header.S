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
    add     pc, rIBASE, \reg, lsl #${handler_size_bits}
.endm
.macro GOTO_OPCODE_BASE base,reg
    add     pc, \base, \reg, lsl #${handler_size_bits}
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
