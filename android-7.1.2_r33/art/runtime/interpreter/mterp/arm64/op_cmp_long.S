    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG_WIDE x1, w2
    GET_VREG_WIDE x2, w3
    cmp     x1, x2
    csinc   w0, wzr, wzr, eq
    csneg   w0, w0, w0, ge
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    SET_VREG w0, w4
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
