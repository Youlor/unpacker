%default { "load":"ldr", "shift":"2", "data_offset":"MIRROR_INT_ARRAY_DATA_OFFSET" }
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
    FETCH_B w2, 1, 0                    // w2<- BB
    lsr     w9, wINST, #8               // w9<- AA
    FETCH_B w3, 1, 1                    // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     x0, common_errNullObject    // bail if null array object.
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, uxtw #$shift    // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $load   w2, [x0, #$data_offset]     // w2<- vBB[vCC]
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w2, w9                     // vAA<- w2
    GOTO_OPCODE ip                      // jump to next instruction
