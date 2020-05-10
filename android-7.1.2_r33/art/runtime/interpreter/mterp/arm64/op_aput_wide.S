    /*
     * Array put, 64 bits.  vBB[vCC] <- vAA.
     *
     */
    /* aput-wide vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    cbz     w0, common_errNullObject    // bail if null
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- arrayObj->length
    add     x0, x0, w1, lsl #3          // w0<- arrayObj + index*width
    cmp     w1, w3                      // compare unsigned index, length
    bcs     common_errArrayIndex        // index >= length, bail
    GET_VREG_WIDE x1, w4
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    str     x1, [x0, #MIRROR_WIDE_ARRAY_DATA_OFFSET]
    GOTO_OPCODE ip                      // jump to next instruction
