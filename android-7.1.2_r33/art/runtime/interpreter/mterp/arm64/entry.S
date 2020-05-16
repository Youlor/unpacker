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
