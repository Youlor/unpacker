    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH w3, 2                         // w3<- BBBB
    FETCH w2, 1                         // w2<- AAAA
    GET_VREG_WIDE x3, w3
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    SET_VREG_WIDE x3, w2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
