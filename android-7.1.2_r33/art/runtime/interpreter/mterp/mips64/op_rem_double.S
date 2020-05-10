    /* rem-double vAA, vBB, vCC */
    .extern fmod
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    GET_VREG_DOUBLE f12, a2             # f12 <- vBB
    GET_VREG_DOUBLE f13, a3             # f13 <- vCC
    jal     fmod                        # f0 <- f12 op f13
    srl     a4, rINST, 8                # a4 <- AA
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_DOUBLE f0, a4              # vAA <- f0
    GOTO_OPCODE v0                      # jump to next instruction
