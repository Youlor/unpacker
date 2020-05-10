    /* const-wide/32 vAA, #+BBBBbbbb */
    srl     a2, rINST, 8                # a2 <- AA
    lh      a0, 2(rPC)                  # a0 <- bbbb (low)
    lh      a1, 4(rPC)                  # a1 <- BBBB (high)
    FETCH_ADVANCE_INST 3                # advance rPC, load rINST
    ins     a0, a1, 16, 16              # a0 = BBBBbbbb
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vAA <- +BBBBbbbb
    GOTO_OPCODE v0                      # jump to next instruction
