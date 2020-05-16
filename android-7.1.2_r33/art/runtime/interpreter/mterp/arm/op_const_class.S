    /* const/class vAA, Class@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- BBBB
    mov     r1, rINST, lsr #8           @ r1<- AA
    add     r2, rFP, #OFF_FP_SHADOWFRAME
    mov     r3, rSELF
    bl      MterpConstClass             @ (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2
    cmp     r0, #0
    bne     MterpPossibleException
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
