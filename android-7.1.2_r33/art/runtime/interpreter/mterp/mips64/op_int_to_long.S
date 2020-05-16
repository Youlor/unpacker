    /* int-to-long vA, vB */
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG a0, a3                     # a0 <- vB (sign-extended to 64 bits)
    ext     a2, rINST, 8, 4             # a2 <- A
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vA <- vB
    GOTO_OPCODE v0                      # jump to next instruction
