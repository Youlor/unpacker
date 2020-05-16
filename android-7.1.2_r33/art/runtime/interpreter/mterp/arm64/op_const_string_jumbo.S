    /* const/string vAA, String//BBBBBBBB */
    EXPORT_PC
    FETCH w0, 1                         // w0<- bbbb (low
    FETCH w2, 2                         // w2<- BBBB (high
    lsr     w1, wINST, #8               // w1<- AA
    orr     w0, w0, w2, lsl #16         // w1<- BBBBbbbb
    add     x2, xFP, #OFF_FP_SHADOWFRAME
    mov     x3, xSELF
    bl      MterpConstString            // (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 3                     // advance rPC
    cbnz    w0, MterpPossibleException      // let reference interpreter deal with it.
    ADVANCE 3                           // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
