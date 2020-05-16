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
    .align 2
    .global ExecuteMterpImpl
    .ent    ExecuteMterpImpl
    .frame sp, STACK_SIZE, ra
/*
 * On entry:
 *  a0  Thread* self
 *  a1  code_item
 *  a2  ShadowFrame
 *  a3  JValue* result_register
 *
 */

ExecuteMterpImpl:
    .set noreorder
    .cpload t9
    .set reorder
/* Save to the stack. Frame size = STACK_SIZE */
    STACK_STORE_FULL()
/* This directive will make sure all subsequent jal restore gp at a known offset */
    .cprestore STACK_OFFSET_GP

    /* Remember the return register */
    sw      a3, SHADOWFRAME_RESULT_REGISTER_OFFSET(a2)

    /* Remember the code_item */
    sw      a1, SHADOWFRAME_CODE_ITEM_OFFSET(a2)

    /* set up "named" registers */
    move    rSELF, a0
    lw      a0, SHADOWFRAME_NUMBER_OF_VREGS_OFFSET(a2)
    addu    rFP, a2, SHADOWFRAME_VREGS_OFFSET     # point to vregs.
    EAS2(rREFS, rFP, a0)                          # point to reference array in shadow frame
    lw      a0, SHADOWFRAME_DEX_PC_OFFSET(a2)     # Get starting dex_pc
    addu    rPC, a1, CODEITEM_INSNS_OFFSET        # Point to base of insns[]
    EAS1(rPC, rPC, a0)                            # Create direct pointer to 1st dex opcode

    EXPORT_PC()

    /* Starting ibase */
    lw      rIBASE, THREAD_CURRENT_IBASE_OFFSET(rSELF)

    /* start executing the instruction at rPC */
    FETCH_INST()                           # load rINST from rPC
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    GOTO_OPCODE(t0)                        # jump to next instruction
    /* NOTE: no fallthrough */
