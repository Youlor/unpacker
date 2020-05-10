    /* const-wide/32 vAA, #+BBBBbbbb */
    movslq   2(rPC), %rax                   # eax <- ssssssssBBBBbbbb
    SET_WIDE_VREG %rax, rINSTq              # store
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
