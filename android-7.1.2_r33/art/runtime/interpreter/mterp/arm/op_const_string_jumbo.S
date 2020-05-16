    /* const/string vAA, String@BBBBBBBB */
    EXPORT_PC
    FETCH r0, 1                         @ r0<- bbbb (low)
    FETCH r2, 2                         @ r2<- BBBB (high)
    mov     r1, rINST, lsr #8           @ r1<- AA
    orr     r0, r0, r2, lsl #16         @ r1<- BBBBbbbb
    add     r2, rFP, #OFF_FP_SHADOWFRAME
    mov     r3, rSELF
    bl      MterpConstString            @ (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 3                     @ advance rPC
    cmp     r0, #0                      @ fail?
    bne     MterpPossibleException      @ let reference interpreter deal with it.
    ADVANCE 3                           @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
