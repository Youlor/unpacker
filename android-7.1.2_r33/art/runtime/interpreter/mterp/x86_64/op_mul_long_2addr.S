    /* mul vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, %rcx                # rax <- vA
    imulq   (rFP,rINSTq,4), %rax
    SET_WIDE_VREG %rax, %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
