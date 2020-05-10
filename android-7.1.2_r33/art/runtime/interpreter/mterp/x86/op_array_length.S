/*
 * Return the length of an array.
 */
    mov     rINST, %eax                     # eax <- BA
    sarl    $$4, rINST                      # rINST <- B
    GET_VREG %ecx, rINST                    # ecx <- vB (object ref)
    testl   %ecx, %ecx                      # is null?
    je      common_errNullObject
    andb    $$0xf, %al                      # eax <- A
    movl    MIRROR_ARRAY_LENGTH_OFFSET(%ecx), rINST
    SET_VREG rINST, %eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
