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
    add     \reg, xIBASE, \reg, lsl #${handler_size_bits}
    br      \reg
.endm
.macro GOTO_OPCODE_BASE base,reg
    add     \reg, \base, \reg, lsl #${handler_size_bits}
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
