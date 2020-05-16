%default {"opcode":"shl"}
    /*
     * 64-bit shift operation.
     *
     * For: shl-long, shr-long, ushr-long
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr      w3, wINST, #8               // w3<- AA
    lsr      w2, w0, #8                  // w2<- CC
    GET_VREG w2, w2                     // w2<- vCC (shift count)
    and      w1, w0, #255                // w1<- BB
    GET_VREG_WIDE x1, w1                // x1<- vBB
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    and      x2, x2, #63                 // Mask low 6
    $opcode  x0, x1, x2                 // Do the shift.
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w3                // vAA<- x0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */
