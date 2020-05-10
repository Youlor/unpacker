    /* iget-wide-quick vA, vB, offset//CCCC */
    srl     a2, rINST, 12               # a2 <- B
    lhu     a4, 2(rPC)                  # a4 <- field byte offset
    GET_VREG_U a3, a2                   # a3 <- object we're operating on
    ext     a2, rINST, 8, 4             # a2 <- A
    beqz    a3, common_errNullObject    # object was null
    daddu   a4, a3, a4                  # create direct pointer
    lw      a0, 0(a4)
    lw      a1, 4(a4)
    dinsu   a0, a1, 32, 32
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    SET_VREG_WIDE a0, a2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
