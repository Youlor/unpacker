%default {"routine":"art_quick_ldiv"}
/* art_quick_* methods has quick abi,
 *   so use eax, ecx, edx, ebx for args
 */
    /* div/2addr vA, vB */
    .extern   $routine
    mov     rIBASE, LOCAL0(%esp)            # save rIBASE/%edx
    movzbl  rINSTbl, %eax
    shrl    $$4, %eax                       # eax <- B
    andb    $$0xf, rINSTbl                  # rINST <- A
    mov     rINST, LOCAL1(%esp)             # save rINST/%ebx
    movl    %ebx, %ecx
    GET_VREG %edx, %eax
    GET_VREG_HIGH %ebx, %eax
    movl    %edx, %eax
    orl     %ebx, %eax
    jz      common_errDivideByZero
    GET_VREG %eax, %ecx
    GET_VREG_HIGH %ecx, %ecx
    call    SYMBOL($routine)
    mov     LOCAL1(%esp), rINST             # restore rINST/%ebx
    SET_VREG_HIGH rIBASE, rINST
    SET_VREG %eax, rINST
    mov     LOCAL0(%esp), rIBASE            # restore rIBASE/%edx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
