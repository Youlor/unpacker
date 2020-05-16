    /* const-wide/16 vAA, #+BBBB */
    FETCH_S w0, 1                       // w0<- ssssBBBB (sign-extended
    lsr     w3, wINST, #8               // w3<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    sbfm    x0, x0, 0, 31
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE x0, w3
    GOTO_OPCODE ip                      // jump to next instruction
