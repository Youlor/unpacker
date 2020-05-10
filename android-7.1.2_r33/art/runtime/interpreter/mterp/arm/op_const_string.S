    /* const/string vAA, String@BBBB */
    EXPORT_PC
    FETCH r0, 1                         @ r0<- BBBB
    mov     r1, rINST, lsr #8           @ r1<- AA
    add     r2, rFP, #OFF_FP_SHADOWFRAME
    mov     r3, rSELF
    bl      MterpConstString            @ (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2                     @ load rINST
    cmp     r0, #0                      @ fail?
    bne     MterpPossibleException      @ let reference interpreter deal with it.
    ADVANCE 2                           @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
