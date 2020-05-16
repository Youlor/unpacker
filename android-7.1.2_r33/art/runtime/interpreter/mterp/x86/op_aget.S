%default { "load":"movl", "shift":"4", "data_offset":"MIRROR_INT_ARRAY_DATA_OFFSET" }
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short
 *
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    $load   $data_offset(%eax,%ecx,$shift), %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
