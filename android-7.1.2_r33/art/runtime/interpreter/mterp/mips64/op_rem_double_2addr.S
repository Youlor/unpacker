    /* rem-double/2addr vA, vB */
    .extern fmod
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG_DOUBLE f12, a2             # f12 <- vA
    GET_VREG_DOUBLE f13, a3             # f13 <- vB
    jal     fmod                        # f0 <- f12 op f13
    ext     a2, rINST, 8, 4             # a2 <- A
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_DOUBLE f0, a2              # vA <- f0
    GOTO_OPCODE v0                      # jump to next instruction
