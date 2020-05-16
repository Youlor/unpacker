    /* rem vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE d1, w2                // d1<- vCC
    GET_VREG_WIDE d0, w1                // d0<- vBB
    bl  fmod
    lsr     w4, wINST, #8               // w4<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w4                // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */
