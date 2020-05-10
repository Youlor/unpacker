    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE  x3, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE  x3, w2
    GOTO_OPCODE ip                      // jump to next instruction
