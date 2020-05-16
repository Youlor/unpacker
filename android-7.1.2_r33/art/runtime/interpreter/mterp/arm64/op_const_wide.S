    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    FETCH w0, 1                         // w0<- bbbb (low)
    FETCH w1, 2                         // w1<- BBBB (low middle)
    FETCH w2, 3                         // w2<- hhhh (high middle)
    FETCH w3, 4                         // w3<- HHHH (high)
    lsr     w4, wINST, #8               // r4<- AA
    FETCH_ADVANCE_INST 5                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    orr     w0, w0, w1, lsl #16         // w0<-         BBBBbbbb
    orr     x0, x0, x2, lsl #32         // w0<-     hhhhBBBBbbbb
    orr     x0, x0, x3, lsl #48         // w0<- HHHHhhhhBBBBbbbb
    SET_VREG_WIDE x0, w4
    GOTO_OPCODE ip                      // jump to next instruction
