    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    lhu     a3, 4(rPC)                  # a3 <- BBBB
    lhu     a2, 2(rPC)                  # a2 <- AAAA
    GET_VREG_WIDE a0, a3                # a0 <- vBBBB
    FETCH_ADVANCE_INST 3                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vAAAA <- vBBBB
    GOTO_OPCODE v0                      # jump to next instruction
