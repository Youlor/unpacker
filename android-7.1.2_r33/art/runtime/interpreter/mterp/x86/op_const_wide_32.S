    /* const-wide/32 vAA, #+BBBBbbbb */
    movl    2(rPC), %eax                    # eax <- BBBBbbbb
    movl    rIBASE, %ecx                    # preserve rIBASE (cltd trashes it)
    cltd                                    # rIBASE:eax <- ssssssssssssBBBB
    SET_VREG_HIGH rIBASE, rINST             # store msw
    SET_VREG %eax, rINST                    # store lsw
    movl    %ecx, rIBASE                    # restore rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
