    /* const/16 vAA, #+BBBB */
    FETCH_S w0, 1                       // w0<- ssssBBBB (sign-extended
    lsr     w3, wINST, #8               // w3<- AA
    FETCH_ADVANCE_INST 2                // advance xPC, load wINST
    SET_VREG w0, w3                     // vAA<- w0
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
