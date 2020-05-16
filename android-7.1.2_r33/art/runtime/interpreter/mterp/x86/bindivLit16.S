%default {"result":"","special":""}
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movzbl  rINSTbl, %eax                   # eax <- 000000BA
    sarl    $$4, %eax                       # eax <- B
    GET_VREG %eax, %eax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $$0xf, rINSTbl                  # rINST <- A
    testl   %ecx, %ecx
    je      common_errDivideByZero
    cmpl    $$-1, %ecx
    jne     .L${opcode}_continue_div
    cmpl    $$0x80000000, %eax
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
