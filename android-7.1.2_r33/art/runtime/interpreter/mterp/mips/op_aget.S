%default { "load":"lw", "shift":"2", "data_offset":"MIRROR_INT_ARRAY_DATA_OFFSET" }
    /*
     * Array get, 32 bits or less.  vAA <- vBB[vCC].
     *
     * Note: using the usual FETCH/and/shift stuff, this fits in exactly 17
     * instructions.  We use a pair of FETCH_Bs instead.
     *
     * for: aget, aget-boolean, aget-byte, aget-char, aget-short
     *
     * NOTE: assumes data offset for arrays is the same for all non-wide types.
     * If this changes, specialize.
     */
    /* op vAA, vBB, vCC */
    FETCH_B(a2, 1, 0)                      #  a2 <- BB
    GET_OPA(rOBJ)                          #  rOBJ <- AA
    FETCH_B(a3, 1, 1)                      #  a3 <- CC
    GET_VREG(a0, a2)                       #  a0 <- vBB (array object)
    GET_VREG(a1, a3)                       #  a1 <- vCC (requested index)
    # null array object?
    beqz      a0, common_errNullObject     #  yes, bail
    LOAD_base_offMirrorArray_length(a3, a0) #  a3 <- arrayObj->length
    .if $shift
    EASN(a0, a0, a1, $shift)               #  a0 <- arrayObj + index*width
    .else
    addu      a0, a0, a1
    .endif
    # a1 >= a3; compare unsigned index
    bgeu      a1, a3, common_errArrayIndex #  index >= length, bail
    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    $load a2, $data_offset(a0)             #  a2 <- vBB[vCC]
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    SET_VREG_GOTO(a2, rOBJ, t0)            #  vAA <- a2
