    /*
     * 32-bit binary multiplication.
     */
    /* mul vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    mov     rIBASE, LOCAL0(%esp)
    imull   (rFP,%ecx,4), %eax              # trashes rIBASE/edx
    mov     LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
