/*
 * Array put, 64 bits.  vBB[vCC] <- vAA.
 *
 */
    /* aput-wide vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecx <- vCC (requested index)
    testl   %eax, %eax                      # null array object?
    je      common_errNullObject            # bail if so
    cmpl    MIRROR_ARRAY_LENGTH_OFFSET(%eax), %ecx
    jae     common_errArrayIndex            # index >= length, bail.
    leal    MIRROR_WIDE_ARRAY_DATA_OFFSET(%eax,%ecx,8), %eax
    GET_WIDE_FP_VREG %xmm0, rINST           # xmm0 <- vAA
    movq    %xmm0, (%eax)                   # vBB[vCC] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
