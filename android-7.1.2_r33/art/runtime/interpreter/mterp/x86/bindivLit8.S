%default {"result":"","special":""}
/*
 * 32-bit div/rem "lit8" binary operation.  Handles special case of
 * op0=minint & op1=-1
 */
    /* div/rem/lit8 vAA, vBB, #+CC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %eax                    # eax <- rBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $$0x80000000, %eax
    jne     .L${opcode}_continue_div
    cmpl    $$-1, %ecx
    jne     .L${opcode}_continue_div
    movl    $special, %eax
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.L${opcode}_continue_div:
    mov     rIBASE, LOCAL0(%esp)
    cltd
    idivl   %ecx
    SET_VREG $result, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
