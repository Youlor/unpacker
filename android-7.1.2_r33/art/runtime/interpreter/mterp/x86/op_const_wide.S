    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    movl    2(rPC), %eax                    # eax <- lsw
    movzbl  rINSTbl, %ecx                   # ecx <- AA
    movl    6(rPC), rINST                   # rINST <- msw
    SET_VREG %eax, %ecx
    SET_VREG_HIGH  rINST, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 5
