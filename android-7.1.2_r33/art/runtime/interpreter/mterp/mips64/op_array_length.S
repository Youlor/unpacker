    /*
     * Return the length of an array.
     */
    srl     a1, rINST, 12               # a1 <- B
    GET_VREG_U a0, a1                   # a0 <- vB (object ref)
    ext     a2, rINST, 8, 4             # a2 <- A
    beqz    a0, common_errNullObject    # yup, fail
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    lw      a3, MIRROR_ARRAY_LENGTH_OFFSET(a0)  # a3 <- array length
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG a3, a2                     # vB <- length
    GOTO_OPCODE v0                      # jump to next instruction
