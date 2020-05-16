    /* fill-array-data vAA, +BBBBBBBB */
    EXPORT_PC
    FETCH w0, 1                         // w0<- bbbb (lo)
    FETCH w1, 2                         // w1<- BBBB (hi)
    lsr     w3, wINST, #8               // w3<- AA
    orr     w1, w0, w1, lsl #16         // w1<- BBBBbbbb
    GET_VREG w0, w3                     // w0<- vAA (array object)
    add     x1, xPC, w1, lsl #1         // w1<- PC + BBBBbbbb*2 (array data off.)
    bl      MterpFillArrayData          // (obj, payload)
    cbz     w0, MterpPossibleException      // exception?
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
