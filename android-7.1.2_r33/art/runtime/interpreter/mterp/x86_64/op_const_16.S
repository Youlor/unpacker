    /* const/16 vAA, #+BBBB */
    movswl  2(rPC), %ecx                    # ecx <- ssssBBBB
    SET_VREG %ecx, rINSTq                   # vAA <- ssssBBBB
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
