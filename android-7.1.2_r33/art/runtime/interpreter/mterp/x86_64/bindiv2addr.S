%default {"result":"","second":"","wide":"","suffix":"","rem":"0","ext":"cdq"}
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $$4, %ecx                       # rcx <- B
    andb    $$0xf, rINSTbl                  # rINST <- A
    .if $wide
    GET_WIDE_VREG %rax, rINSTq              # eax <- vA
    GET_WIDE_VREG $second, %rcx             # ecx <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vA
    GET_VREG $second, %rcx                  # ecx <- vB
    .endif
    test${suffix}   $second, $second
    jz      common_errDivideByZero
    cmp${suffix}  $$-1, $second
    je      2f
    $ext                                    # rdx:rax <- sign-extended of rax
    idiv${suffix}   $second
1:
    .if $wide
    SET_WIDE_VREG $result, rINSTq           # vA <- result
    .else
    SET_VREG $result, rINSTq                # vA <- result
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
2:
    .if $rem
    xor${suffix} $result, $result
    .else
    neg${suffix} $result
    .endif
    jmp     1b
