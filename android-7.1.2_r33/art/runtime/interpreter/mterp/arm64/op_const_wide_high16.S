    /* const-wide/high16 vAA, #+BBBB000000000000 */
    FETCH w0, 1                         // w0<- 0000BBBB (zero-extended)
    lsr     w1, wINST, #8               // w1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    lsl     x0, x0, #48
    SET_VREG_WIDE x0, w1
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
