    /* const-wide vAA, #+HHHHhhhhBBBBbbbb */
    movq    2(rPC), %rax                    # rax <- HHHHhhhhBBBBbbbb
    SET_WIDE_VREG %rax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 5
