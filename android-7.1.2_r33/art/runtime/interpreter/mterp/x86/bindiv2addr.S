%default {"result":"","special":""}
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    mov     rIBASE, LOCAL0(%esp)
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $$0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $$-1, %ecx
    jne     .L${opcode}_continue_div2addr
    cmpl    $$0x80000000, %eax
    jne     .L${opcode}_continue_div2addr
    movl    $special, $result
    SET_VREG $result, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.L${opcode}_continue_div2addr:
    cltd
    idivl   %ecx
    SET_VREG $result, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
