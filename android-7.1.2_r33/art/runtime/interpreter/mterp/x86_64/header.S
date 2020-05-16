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
    #define MACRO_LITERAL(value) $$(value)
    #define FUNCTION_TYPE(name)
    #define SIZE(start,end)
    // Mac OS' symbols have an _ prefix.
    #define SYMBOL(name) _ ## name
#else
    #define MACRO_LITERAL(value) $$value
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
    movb    $$\_opnum, rINSTbl
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
    shll    MACRO_LITERAL(${handler_size_bits}), %eax
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
