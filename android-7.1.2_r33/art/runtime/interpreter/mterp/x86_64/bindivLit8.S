%default {"result":"","rem":"0"}
/*
 * 32-bit div/rem "lit8" binary operation.  Handles special case of
 * op0=minint & op1=-1
 */
    /* div/rem/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movsbl  3(rPC), %ecx                    # ecx <- ssssssCC
    GET_VREG  %eax, %rax                    # eax <- rBB
    testl   %ecx, %ecx
    je      common_errDivideByZero
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
