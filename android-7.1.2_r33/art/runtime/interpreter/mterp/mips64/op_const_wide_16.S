    /* const-wide/16 vAA, #+BBBB */
    srl     a2, rINST, 8                # a2 <- AA
    lh      a0, 2(rPC)                  # a0 <- sign-extended BBBB
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vAA <- +BBBB
    GOTO_OPCODE v0                      # jump to next instruction
