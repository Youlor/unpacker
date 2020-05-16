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
    subq    $$FRAME_SIZE, %rsp
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
