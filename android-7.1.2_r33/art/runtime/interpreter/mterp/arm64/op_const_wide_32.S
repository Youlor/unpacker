    /* const-wide/32 vAA, #+BBBBbbbb */
    FETCH w0, 1                         // w0<- 0000bbbb (low)
    lsr     w3, wINST, #8               // w3<- AA
    FETCH_S w2, 2                       // w2<- ssssBBBB (high)
    FETCH_ADVANCE_INST 3                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    orr     w0, w0, w2, lsl #16         // w0<- BBBBbbbb
    sbfm    x0, x0, 0, 31
    SET_VREG_WIDE x0, w3
    GOTO_OPCODE ip                      // jump to next instruction
