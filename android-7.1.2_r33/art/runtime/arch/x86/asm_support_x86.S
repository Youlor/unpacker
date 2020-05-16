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

#ifndef ART_RUNTIME_ARCH_X86_ASM_SUPPORT_X86_S_
#define ART_RUNTIME_ARCH_X86_ASM_SUPPORT_X86_S_

#include "asm_support_x86.h"

// Regular gas(1) & current clang/llvm assembler support named macro parameters.
#define MACRO0(macro_name) .macro macro_name
#define MACRO1(macro_name, macro_arg1) .macro macro_name macro_arg1
#define MACRO2(macro_name, macro_arg1, macro_arg2) .macro macro_name macro_arg1, macro_arg2
#define MACRO3(macro_name, macro_arg1, macro_arg2, macro_arg3) .macro macro_name macro_arg1, macro_arg2, macro_arg3
#define MACRO4(macro_name, macro_arg1, macro_arg2, macro_arg3, macro_arg4) .macro macro_name macro_arg1, macro_arg2, macro_arg3, macro_arg4
#define MACRO5(macro_name, macro_arg1, macro_arg2, macro_arg3, macro_arg4, macro_arg5) .macro macro_name macro_arg1, macro_arg2, macro_arg3, macro_arg4, macro_arg5
#define END_MACRO .endm

#if defined(__clang__)
    // Clang/llvm does not support .altmacro. However, the clang/llvm preprocessor doesn't
    // separate the backslash and parameter by a space. Everything just works.
    #define RAW_VAR(name) \name
    #define VAR(name) \name
    #define CALLVAR(name) SYMBOL(\name)
    #define PLT_VAR(name) \name@PLT
    #define REG_VAR(name) %\name
    #define CALL_MACRO(name) \name
#else
    // Regular gas(1) uses \argument_name for macro arguments.
    // We need to turn on alternate macro syntax so we can use & instead or the preprocessor
    // will screw us by inserting a space between the \ and the name. Even in this mode there's
    // no special meaning to $, so literals are still just $x. The use of altmacro means % is a
    // special character meaning care needs to be taken when passing registers as macro
    // arguments.
    .altmacro
    #define RAW_VAR(name) name&
    #define VAR(name) name&
    #define CALLVAR(name) SYMBOL(name&)
    #define PLT_VAR(name) name&@PLT
    #define REG_VAR(name) %name
    #define CALL_MACRO(name) name&
#endif

#define LITERAL(value) $value
#if defined(__APPLE__)
    #define MACRO_LITERAL(value) $(value)
#else
    #define MACRO_LITERAL(value) $value
#endif

#if defined(__APPLE__)
    #define FUNCTION_TYPE(name)
    #define SIZE(name)
#else
    #define FUNCTION_TYPE(name) .type name, @function
    #define SIZE(name) .size name, .-name
#endif

    // CFI support.
#if !defined(__APPLE__)
    #define CFI_STARTPROC .cfi_startproc
    #define CFI_ENDPROC .cfi_endproc
    #define CFI_ADJUST_CFA_OFFSET(size) .cfi_adjust_cfa_offset size
    #define CFI_DEF_CFA(reg,size) .cfi_def_cfa reg,size
    #define CFI_DEF_CFA_REGISTER(reg) .cfi_def_cfa_register reg
    #define CFI_RESTORE(reg) .cfi_restore reg
    #define CFI_REL_OFFSET(reg,size) .cfi_rel_offset reg,size
    #define CFI_RESTORE_STATE .cfi_restore_state
    #define CFI_REMEMBER_STATE .cfi_remember_state
#else
    // Mac OS' doesn't like cfi_* directives.
    #define CFI_STARTPROC
    #define CFI_ENDPROC
    #define CFI_ADJUST_CFA_OFFSET(size)
    #define CFI_DEF_CFA(reg,size)
    #define CFI_DEF_CFA_REGISTER(reg)
    #define CFI_RESTORE(reg)
    #define CFI_REL_OFFSET(reg,size)
    #define CFI_RESTORE_STATE
    #define CFI_REMEMBER_STATE
#endif

    // Symbols. On a Mac, we need a leading underscore.
#if !defined(__APPLE__)
    #define SYMBOL(name) name
    #define PLT_SYMBOL(name) name ## @PLT
#else
    // Mac OS' symbols have an _ prefix.
    #define SYMBOL(name) _ ## name
    #define PLT_SYMBOL(name) _ ## name
#endif

// Directive to hide a function symbol.
#if defined(__APPLE__)
    #define ASM_HIDDEN .private_extern
#else
    #define ASM_HIDDEN .hidden
#endif

    /* Cache alignment for function entry */
MACRO0(ALIGN_FUNCTION_ENTRY)
    .balign 16
END_MACRO

MACRO1(DEFINE_FUNCTION, c_name)
    FUNCTION_TYPE(SYMBOL(\c_name))
    ASM_HIDDEN CALLVAR(c_name)
    .globl CALLVAR(c_name)
    ALIGN_FUNCTION_ENTRY
CALLVAR(c_name):
    CFI_STARTPROC
    // Ensure we get a sane starting CFA.
    CFI_DEF_CFA(esp, 4)
END_MACRO

MACRO1(END_FUNCTION, c_name)
    CFI_ENDPROC
    SIZE(SYMBOL(\c_name))
END_MACRO

MACRO1(PUSH, reg)
    pushl REG_VAR(reg)
    CFI_ADJUST_CFA_OFFSET(4)
    CFI_REL_OFFSET(REG_VAR(reg), 0)
END_MACRO

MACRO1(POP, reg)
    popl REG_VAR(reg)
    CFI_ADJUST_CFA_OFFSET(-4)
    CFI_RESTORE(REG_VAR(reg))
END_MACRO

MACRO1(CFI_RESTORE_REG, reg)
    CFI_RESTORE(REG_VAR(reg))
END_MACRO

#define UNREACHABLE int3

MACRO1(UNIMPLEMENTED,name)
    FUNCTION_TYPE(\name)
    .globl VAR(name)
    ALIGN_FUNCTION_ENTRY
VAR(name):
    CFI_STARTPROC
    UNREACHABLE
    UNREACHABLE
    CFI_ENDPROC
    SIZE(\name)
END_MACRO

MACRO1(SETUP_GOT_NOSAVE, got_reg)
#ifndef __APPLE__
    .ifc VAR(got_reg), ebx
      call __x86.get_pc_thunk.bx
      addl $_GLOBAL_OFFSET_TABLE_, %ebx
    .else
      .error "Unknown GOT register \got_reg"
    .endif
#endif
END_MACRO

// Macros to poison (negate) the reference for heap poisoning.
MACRO1(POISON_HEAP_REF, rRef)
#ifdef USE_HEAP_POISONING
    neg REG_VAR(rRef)
#endif  // USE_HEAP_POISONING
END_MACRO

// Macros to unpoison (negate) the reference for heap poisoning.
MACRO1(UNPOISON_HEAP_REF, rRef)
#ifdef USE_HEAP_POISONING
    neg REG_VAR(rRef)
#endif  // USE_HEAP_POISONING
END_MACRO


#endif  // ART_RUNTIME_ARCH_X86_ASM_SUPPORT_X86_S_
