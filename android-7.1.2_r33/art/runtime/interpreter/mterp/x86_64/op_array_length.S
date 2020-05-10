/*
 * Return the length of an array.
 */
    movl    rINST, %eax                     # eax <- BA
    sarl    $$4, rINST                      # rINST <- B
    GET_VREG %ecx, rINSTq                   # ecx <- vB (object ref)
    testl   %ecx, %ecx                      # is null?
    je      common_errNullObject
    andb    $$0xf, %al                      # eax <- A
    movl    MIRROR_ARRAY_LENGTH_OFFSET(%rcx), rINST
    SET_VREG rINST, %rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
