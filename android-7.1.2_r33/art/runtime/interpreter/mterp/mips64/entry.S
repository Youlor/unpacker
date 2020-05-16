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

    .set    reorder

    .text
    .global ExecuteMterpImpl
    .type   ExecuteMterpImpl, %function
    .balign 16
/*
 * On entry:
 *  a0  Thread* self
 *  a1  code_item
 *  a2  ShadowFrame
 *  a3  JValue* result_register
 *
 */
ExecuteMterpImpl:
    .cfi_startproc
    .cpsetup t9, t8, ExecuteMterpImpl

    .cfi_def_cfa sp, 0
    daddu   sp, sp, -STACK_SIZE
    .cfi_adjust_cfa_offset STACK_SIZE

    sd      t8, STACK_OFFSET_GP(sp)
    .cfi_rel_offset 28, STACK_OFFSET_GP
    sd      ra, STACK_OFFSET_RA(sp)
    .cfi_rel_offset 31, STACK_OFFSET_RA

    sd      s0, STACK_OFFSET_S0(sp)
    .cfi_rel_offset 16, STACK_OFFSET_S0
    sd      s1, STACK_OFFSET_S1(sp)
    .cfi_rel_offset 17, STACK_OFFSET_S1
    sd      s2, STACK_OFFSET_S2(sp)
    .cfi_rel_offset 18, STACK_OFFSET_S2
    sd      s3, STACK_OFFSET_S3(sp)
    .cfi_rel_offset 19, STACK_OFFSET_S3
    sd      s4, STACK_OFFSET_S4(sp)
    .cfi_rel_offset 20, STACK_OFFSET_S4
    sd      s5, STACK_OFFSET_S5(sp)
    .cfi_rel_offset 21, STACK_OFFSET_S5

    /* Remember the return register */
    sd      a3, SHADOWFRAME_RESULT_REGISTER_OFFSET(a2)

    /* Remember the code_item */
    sd      a1, SHADOWFRAME_CODE_ITEM_OFFSET(a2)

    /* set up "named" registers */
    move    rSELF, a0
    daddu   rFP, a2, SHADOWFRAME_VREGS_OFFSET
    lw      v0, SHADOWFRAME_NUMBER_OF_VREGS_OFFSET(a2)
    dlsa    rREFS, v0, rFP, 2
    daddu   rPC, a1, CODEITEM_INSNS_OFFSET
    lw      v0, SHADOWFRAME_DEX_PC_OFFSET(a2)
    dlsa    rPC, v0, rPC, 1
    EXPORT_PC

    /* Starting ibase */
    REFRESH_IBASE

    /* start executing the instruction at rPC */
    FETCH_INST
    GET_INST_OPCODE v0
    GOTO_OPCODE v0

    /* NOTE: no fallthrough */
