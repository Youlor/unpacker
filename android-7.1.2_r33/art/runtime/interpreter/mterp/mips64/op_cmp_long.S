    /* cmp-long vAA, vBB, vCC */
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    srl     a4, rINST, 8                # a4 <- AA
    GET_VREG_WIDE a0, a2                # a0 <- vBB
    GET_VREG_WIDE a1, a3                # a1 <- vCC
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    slt     a2, a0, a1
    slt     a0, a1, a0
    subu    a0, a0, a2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG a0, a4                     # vAA <- result
    GOTO_OPCODE v0                      # jump to next instruction
