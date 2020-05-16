%default { "reg":"rINST", "store":"movl", "shift":"4", "data_offset":"MIRROR_INT_ARRAY_DATA_OFFSET", "wide":"0" }
/*
 * Array put, 32 bits or less.  vBB[vCC] <- vAA.
 *
 * for: aput, aput-boolean, aput-byte, aput-char, aput-short, aput-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if $wide
    GET_WIDE_VREG rINSTq, rINSTq
    .else
    GET_VREG rINST, rINSTq
    .endif
    $store    $reg, $data_offset(%rax,%rcx,$shift)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
