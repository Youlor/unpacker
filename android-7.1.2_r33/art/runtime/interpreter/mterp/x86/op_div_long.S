%default {"routine":"art_quick_ldiv"}
/* art_quick_* methods has quick abi,
 *   so use eax, ecx, edx, ebx for args
 */
    /* div vAA, vBB, vCC */
    .extern $routine
    mov     rIBASE, LOCAL0(%esp)            # save rIBASE/%edx
    mov     rINST, LOCAL1(%esp)             # save rINST/%ebx
    movzbl  3(rPC), %eax                    # eax <- CC
    GET_VREG %ecx, %eax
    GET_VREG_HIGH %ebx, %eax
    movl    %ecx, %edx
    orl     %ebx, %ecx
    jz      common_errDivideByZero
    movzbl  2(rPC), %eax                    # eax <- BB
    GET_VREG_HIGH %ecx, %eax
    GET_VREG %eax, %eax
    call    SYMBOL($routine)
    mov     LOCAL1(%esp), rINST             # restore rINST/%ebx
    SET_VREG_HIGH rIBASE, rINST
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE            # restore rIBASE/%edx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
