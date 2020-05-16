    /* mul/lit8 vAA, vBB, #+CC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movl    rIBASE, %ecx
    GET_VREG  %eax, %eax                    # eax <- rBB
    movsbl  3(rPC), rIBASE                  # rIBASE <- ssssssCC
    imull   rIBASE, %eax                    # trashes rIBASE/edx
    movl    %ecx, rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
