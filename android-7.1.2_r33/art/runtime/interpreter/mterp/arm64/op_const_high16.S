    /* const/high16 vAA, #+BBBB0000 */
    FETCH   w0, 1                       // r0<- 0000BBBB (zero-extended
    lsr     w3, wINST, #8               // r3<- AA
    lsl     w0, w0, #16                 // r0<- BBBB0000
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    SET_VREG w0, w3                     // vAA<- r0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
