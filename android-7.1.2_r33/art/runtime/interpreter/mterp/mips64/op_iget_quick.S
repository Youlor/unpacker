%default { "load":"lw" }
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    srl     a2, rINST, 12               # a2 <- B
    lhu     a1, 2(rPC)                  # a1 <- field byte offset
    GET_VREG_U a3, a2                   # a3 <- object we're operating on
    ext     a4, rINST, 8, 4             # a4 <- A
    daddu   a1, a1, a3
    beqz    a3, common_errNullObject    # object was null
    $load   a0, 0(a1)                   # a0 <- obj.field
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    SET_VREG a0, a4                     # fp[A] <- a0
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
