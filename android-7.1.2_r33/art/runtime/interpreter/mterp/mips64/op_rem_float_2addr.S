    /* rem-float/2addr vA, vB */
    .extern fmodf
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG_FLOAT f12, a2              # f12 <- vA
    GET_VREG_FLOAT f13, a3              # f13 <- vB
    jal     fmodf                       # f0 <- f12 op f13
    ext     a2, rINST, 8, 4             # a2 <- A
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_FLOAT f0, a2               # vA <- f0
    GOTO_OPCODE v0                      # jump to next instruction
