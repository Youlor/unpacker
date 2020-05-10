    /* const vAA, #+BBBBbbbb */
    lsr     w3, wINST, #8               // w3<- AA
    FETCH w0, 1                         // w0<- bbbb (low
    FETCH w1, 2                         // w1<- BBBB (high
    FETCH_ADVANCE_INST 3                // advance rPC, load wINST
    orr     w0, w0, w1, lsl #16         // w0<- BBBBbbbb
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG w0, w3                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
