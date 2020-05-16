%default {"opcode":"lsl"}
    /*
     * Generic 64-bit shift operation.
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG w1, w1                     // x1<- vB
    GET_VREG_WIDE x0, w2                // x0<- vA
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    and     x1, x1, #63                 // Mask low 6 bits.
    $opcode x0, x0, x1
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */
