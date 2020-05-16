%default { "load":"lw", "shift":"2", "data_offset":"MIRROR_INT_ARRAY_DATA_OFFSET" }
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    srl     a4, rINST, 8                # a4 <- AA
    GET_VREG_U a0, a2                   # a0 <- vBB (array object)
    GET_VREG a1, a3                     # a1 <- vCC (requested index)
    beqz    a0, common_errNullObject    # bail if null array object
    lw      a3, MIRROR_ARRAY_LENGTH_OFFSET(a0)  # a3 <- arrayObj->length
    .if $shift
    # [d]lsa does not support shift count of 0.
    dlsa    a0, a1, a0, $shift          # a0 <- arrayObj + index*width
    .else
    daddu   a0, a1, a0                  # a0 <- arrayObj + index*width
    .endif
    bgeu    a1, a3, common_errArrayIndex  # unsigned compare: index >= length, bail
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    $load   a2, $data_offset(a0)        # a2 <- vBB[vCC]
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG a2, a4                     # vAA <- a2
    GOTO_OPCODE v0                      # jump to next instruction
