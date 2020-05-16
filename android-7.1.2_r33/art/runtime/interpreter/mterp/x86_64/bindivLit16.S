%default {"result":"","rem":"0"}
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/lit16 vA, vB, #+CCCC */
    /* Need A in rINST, ssssCCCC in ecx, vB in eax */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $$4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    andb    $$0xf, rINSTbl                  # rINST <- A
    testl   %ecx, %ecx
    jz      common_errDivideByZero
    cmpl    $$-1, %ecx
    je      2f
    cdq                                     # rax <- sign-extended of eax
    idivl   %ecx
1:
    SET_VREG $result, rINSTq                # vA <- result
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if $rem
    xorl    $result, $result
    .else
    negl    $result
    .endif
    jmp     1b
