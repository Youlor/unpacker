    /* const-wide/16 vAA, #+BBBB */
    movswq  2(rPC), %rax                    # rax <- ssssBBBB
    SET_WIDE_VREG %rax, rINSTq              # store
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
