    /*
     * SGET_WIDE handler wrapper.
     *
     */
    /* sget-wide vAA, field@BBBB */

    .extern artGet64StaticFromCode
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    artGet64StaticFromCode
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r9, rINST, lsr #8             @ r9<- AA
    VREG_INDEX_TO_ADDR lr, r9           @ r9<- &fp[AA]
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    CLEAR_SHADOW_PAIR r9, r2, ip        @ Zero out the shadow regs
    stmia lr, {r0-r1}                   @ vAA/vAA+1<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
