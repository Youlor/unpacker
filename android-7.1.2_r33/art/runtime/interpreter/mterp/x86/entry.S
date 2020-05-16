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
    subl    $$FRAME_SIZE, %esp
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
