    /*
     * Array get, 64 bits.  vAA <- vBB[vCC].
     *
     */
    /* aget-wide vAA, vBB, vCC */
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    srl     a4, rINST, 8                # a4 <- AA
    GET_VREG_U a0, a2                   # a0 <- vBB (array object)
    GET_VREG a1, a3                     # a1 <- vCC (requested index)
    beqz    a0, common_errNullObject    # bail if null array object
    lw      a3, MIRROR_ARRAY_LENGTH_OFFSET(a0)  # a3 <- arrayObj->length
    dlsa    a0, a1, a0, 3               # a0 <- arrayObj + index*width
    bgeu    a1, a3, common_errArrayIndex  # unsigned compare: index >= length, bail
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    lw      a2, MIRROR_WIDE_ARRAY_DATA_OFFSET(a0)
    lw      a3, (MIRROR_WIDE_ARRAY_DATA_OFFSET+4)(a0)
    dinsu   a2, a3, 32, 32              # a2 <- vBB[vCC]
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a2, a4                # vAA <- a2
    GOTO_OPCODE v0                      # jump to next instruction
