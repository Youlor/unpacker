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
