    /* mul vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    GET_VREG %eax, %rcx                     # eax <- vA
    imull   (rFP,rINSTq,4), %eax
    SET_VREG %eax, %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
