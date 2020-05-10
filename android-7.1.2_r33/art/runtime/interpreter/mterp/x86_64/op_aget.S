%default { "load":"movl", "shift":"4", "data_offset":"MIRROR_INT_ARRAY_DATA_OFFSET", "wide":"0" }
/*
 * Array get, 32 bits or less.  vAA <- vBB[vCC].
 *
 * for: aget, aget-boolean, aget-byte, aget-char, aget-short, aget-wide
 *
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB (array object)
    GET_VREG %ecx, %rcx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    .if $wide
    movq    $data_offset(%rax,%rcx,8), %rax
    SET_WIDE_VREG %rax, rINSTq
    .else
    $load   $data_offset(%rax,%rcx,$shift), %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
