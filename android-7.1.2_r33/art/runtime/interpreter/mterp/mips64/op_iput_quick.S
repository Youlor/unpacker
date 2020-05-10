%default { "store":"sw" }
    /* For: iput-quick, iput-boolean-quick, iput-byte-quick, iput-char-quick, iput-short-quick */
    /* op vA, vB, offset//CCCC */
    srl     a2, rINST, 12               # a2 <- B
    lhu     a1, 2(rPC)                  # a1 <- field byte offset
    GET_VREG_U a3, a2                   # a3 <- fp[B], the object pointer
    ext     a2, rINST, 8, 4             # a2 <- A
    beqz    a3, common_errNullObject    # object was null
    GET_VREG a0, a2                     # a0 <- fp[A]
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    daddu   a1, a1, a3
    $store  a0, 0(a1)                   # obj.field <- a0
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
