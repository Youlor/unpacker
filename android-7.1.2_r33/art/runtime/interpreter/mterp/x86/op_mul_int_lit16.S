    /* mul/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $$4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movl    rIBASE, %ecx
    movswl  2(rPC), rIBASE                  # rIBASE <- ssssCCCC
    andb    $$0xf, rINSTbl                  # rINST <- A
    imull   rIBASE, %eax                    # trashes rIBASE/edx
    movl    %ecx, rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
