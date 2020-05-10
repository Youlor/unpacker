    /* rem vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE d1, w1                // d1<- vB
    GET_VREG_WIDE d0, w2                // d0<- vA
    bl fmod
    ubfx    w2, wINST, #8, #4           // w2<- A (need to reload - killed across call)
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE d0, w2                // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */
