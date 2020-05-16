    /* const/class vAA, Class//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // w0<- BBBB
    lsr     w1, wINST, #8               // w1<- AA
    add     x2, xFP, #OFF_FP_SHADOWFRAME
    mov     x3, xSELF
    bl      MterpConstClass             // (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 2
    cbnz    w0, MterpPossibleException
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
