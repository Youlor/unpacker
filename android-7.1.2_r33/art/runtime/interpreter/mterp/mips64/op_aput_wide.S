    /*
     * Array put, 64 bits.  vBB[vCC] <- vAA.
     *
     */
    /* aput-wide vAA, vBB, vCC */
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    srl     a4, rINST, 8                # a4 <- AA
    GET_VREG_U a0, a2                   # a0 <- vBB (array object)
    GET_VREG a1, a3                     # a1 <- vCC (requested index)
    beqz    a0, common_errNullObject    # bail if null array object
    lw      a3, MIRROR_ARRAY_LENGTH_OFFSET(a0)  # a3 <- arrayObj->length
    dlsa    a0, a1, a0, 3               # a0 <- arrayObj + index*width
    bgeu    a1, a3, common_errArrayIndex  # unsigned compare: index >= length, bail
    GET_VREG_WIDE a2, a4                # a2 <- vAA
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    sw      a2, MIRROR_WIDE_ARRAY_DATA_OFFSET(a0)
    dsrl32  a2, a2, 0
    sw      a2, (MIRROR_WIDE_ARRAY_DATA_OFFSET+4)(a0)  # vBB[vCC] <- a2
    GOTO_OPCODE v0                      # jump to next instruction
