    /* const-wide/high16 vAA, #+BBBB000000000000 */
    srl     a2, rINST, 8                # a2 <- AA
    lh      a0, 2(rPC)                  # a0 <- BBBB
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    dsll32  a0, a0, 16                  # a0 <- BBBB000000000000
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vAA <- +BBBB000000000000
    GOTO_OPCODE v0                      # jump to next instruction
