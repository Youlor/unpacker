    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    lhu     a3, 2(rPC)                  # a3 <- BBBB
    srl     a2, rINST, 8                # a2 <- AA
    GET_VREG_WIDE a0, a3                # a0 <- vBBBB
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vAA <- vBBBB
    GOTO_OPCODE v0                      # jump to next instruction
