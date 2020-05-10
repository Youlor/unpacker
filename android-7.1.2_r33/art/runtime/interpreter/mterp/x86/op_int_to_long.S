    /* int to long vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- +A
    sarl    $$4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    andb    $$0xf, rINSTbl                  # rINST <- A
    movl    rIBASE, %ecx                    # cltd trashes rIBASE/edx
    cltd                                    # rINST:eax<- sssssssBBBBBBBB
    SET_VREG_HIGH rIBASE, rINST             # v[A+1] <- rIBASE
    SET_VREG %eax, rINST                    # v[A+0] <- %eax
    movl    %ecx, rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

