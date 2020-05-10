    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    srl     a4, rINST, 8                # a4 <- AA
    lh      a0, 2(rPC)                  # a0 <- bbbb (low)
    lh      a1, 4(rPC)                  # a1 <- BBBB (low middle)
    lh      a2, 6(rPC)                  # a2 <- hhhh (high middle)
    lh      a3, 8(rPC)                  # a3 <- HHHH (high)
    FETCH_ADVANCE_INST 5                # advance rPC, load rINST
    ins     a0, a1, 16, 16              # a0 = BBBBbbbb
    ins     a2, a3, 16, 16              # a2 = HHHHhhhh
    dinsu   a0, a2, 32, 32              # a0 = HHHHhhhhBBBBbbbb
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a4                # vAA <- +HHHHhhhhBBBBbbbb
    GOTO_OPCODE v0                      # jump to next instruction
