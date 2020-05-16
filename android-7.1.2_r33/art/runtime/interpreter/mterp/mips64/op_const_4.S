    /* const/4 vA, #+B */
    ext     a2, rINST, 8, 4             # a2 <- A
    seh     a0, rINST                   # sign extend B in rINST
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    sra     a0, a0, 12                  # shift B into its final position
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG a0, a2                     # vA <- +B
    GOTO_OPCODE v0                      # jump to next instruction
