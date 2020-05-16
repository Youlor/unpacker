    /* iput-wide-quick vA, vB, offset//CCCC */
    srl     a2, rINST, 12               # a2 <- B
    lhu     a3, 2(rPC)                  # a3 <- field byte offset
    GET_VREG_U a2, a2                   # a2 <- fp[B], the object pointer
    ext     a0, rINST, 8, 4             # a0 <- A
    beqz    a2, common_errNullObject    # object was null
    GET_VREG_WIDE a0, a0                # a0 <- fp[A]
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    daddu   a1, a2, a3                  # create a direct pointer
    sw      a0, 0(a1)
    dsrl32  a0, a0, 0
    sw      a0, 4(a1)
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
