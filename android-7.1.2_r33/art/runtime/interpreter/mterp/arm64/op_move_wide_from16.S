    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH w3, 1                         // w3<- BBBB
    lsr     w2, wINST, #8               // w2<- AA
    GET_VREG_WIDE x3, w3
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x3, w2
    GOTO_OPCODE ip                      // jump to next instruction
