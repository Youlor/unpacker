/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_ARCH_MIPS_ASM_SUPPORT_MIPS_S_
#define ART_RUNTIME_ARCH_MIPS_ASM_SUPPORT_MIPS_S_

#include "asm_support_mips.h"

// Define special registers.

// Register holding suspend check count down.
#define rSUSPEND $s0
// Register holding Thread::Current().
#define rSELF $s1

     // Declare a function called name, sets up $gp.
.macro ENTRY name
    .type \name, %function
    .global \name
    // Cache alignment for function entry.
    .balign 16
\name:
    .cfi_startproc
     // Ensure we get a sane starting CFA.
    .cfi_def_cfa $sp,0
    // Load $gp. We expect that ".set noreorder" is in effect.
    .cpload $t9
    // Declare a local convenience label to be branched to when $gp is already set up.
.L\name\()_gp_set:
.endm

     // Declare a function called name, doesn't set up $gp.
.macro ENTRY_NO_GP name
    .type \name, %function
    .global \name
    // Cache alignment for function entry.
    .balign 16
\name:
    .cfi_startproc
     // Ensure we get a sane starting CFA.
    .cfi_def_cfa $sp,0
.endm

.macro END name
    .cfi_endproc
    .size \name, .-\name
.endm

.macro UNIMPLEMENTED name
    ENTRY \name
    break
    break
    END \name
.endm

#if defined(__mips_isa_rev) && __mips_isa_rev > 2
  /* mips32r5 & mips32r6 have mthc1 op, and have 64-bit fp regs,
     and in FPXX abi we avoid referring to odd-numbered fp regs */

/* LDu: Load 64-bit floating-point value to float reg feven,
   from unaligned (mod-4-aligned) mem location disp(base) */
.macro LDu feven,fodd,disp,base,temp
  l.s   \feven, \disp(\base)
  lw    \temp, \disp+4(\base)
  mthc1 \temp, \feven
.endm

/* SDu: Store 64-bit floating-point value from float reg feven,
   to unaligned (mod-4-aligned) mem location disp(base) */
.macro SDu feven,fodd,disp,base,temp
  mfhc1 \temp, \feven
  s.s   \feven, \disp(\base)
  sw    \temp, \disp+4(\base)
.endm

/* MTD: Move double, from general regpair (reven,rodd)
        to float regpair (feven,fodd) */
.macro MTD reven,rodd,feven,fodd
  mtc1  \reven, \feven
  mthc1 \rodd, \feven
.endm

#else
  /* mips32r1 has no mthc1 op;
     mips32r1 and mips32r2 use 32-bit floating point register mode (FR=0),
     and always hold doubles as (feven, fodd) fp reg pair */

.macro LDu feven,fodd,disp,base,temp
  l.s   \feven, \disp(\base)
  l.s   \fodd,  \disp+4(\base)
.endm

.macro SDu feven,fodd,disp,base,temp
  s.s   \feven, \disp(\base)
  s.s   \fodd,  \disp+4(\base)
.endm

.macro MTD reven,rodd,feven,fodd
  mtc1  \reven, \feven
  mtc1  \rodd, \fodd
.endm

#endif  /* mips_isa_rev */

// Macros to poison (negate) the reference for heap poisoning.
.macro POISON_HEAP_REF rRef
#ifdef USE_HEAP_POISONING
    subu \rRef, $zero, \rRef
#endif  // USE_HEAP_POISONING
.endm

// Macros to unpoison (negate) the reference for heap poisoning.
.macro UNPOISON_HEAP_REF rRef
#ifdef USE_HEAP_POISONING
    subu \rRef, $zero, \rRef
#endif  // USE_HEAP_POISONING
.endm

// Based on contents of creg select the minimum integer
// At the end of the macro the original value of creg is lost
.macro MINint dreg,rreg,sreg,creg
  .set push
  .set noat
#if defined(_MIPS_ARCH_MIPS32R6) || defined(_MIPS_ARCH_MIPS64R6)
  .ifc \dreg, \rreg
  selnez \dreg, \rreg, \creg
  seleqz \creg, \sreg, \creg
  .else
  seleqz \dreg, \sreg, \creg
  selnez \creg, \rreg, \creg
  .endif
  or     \dreg, \dreg, \creg
#else
  movn   \dreg, \rreg, \creg
  movz   \dreg, \sreg, \creg
#endif
  .set pop
.endm

// Find minimum of two signed registers
.macro MINs dreg,rreg,sreg
  .set push
  .set noat
  slt    $at, \rreg, \sreg
  MINint \dreg, \rreg, \sreg, $at
  .set pop
.endm

// Find minimum of two unsigned registers
.macro MINu dreg,rreg,sreg
  .set push
  .set noat
  sltu   $at, \rreg, \sreg
  MINint \dreg, \rreg, \sreg, $at
  .set pop
.endm

#endif  // ART_RUNTIME_ARCH_MIPS_ASM_SUPPORT_MIPS_S_
