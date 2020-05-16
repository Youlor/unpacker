%default {"result":"","second":"","wide":"","suffix":"","rem":"0","ext":"cdq"}
/*
 * 32-bit binary div/rem operation.  Handles special case of op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    .if $wide
    GET_WIDE_VREG %rax, %rax                # eax <- vBB
    GET_WIDE_VREG $second, %rcx             # ecx <- vCC
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    GET_VREG $second, %rcx                  # ecx <- vCC
    .endif
    test${suffix}   $second, $second
    jz      common_errDivideByZero
    cmp${suffix}  $$-1, $second
    je      2f
    $ext                                    # rdx:rax <- sign-extended of rax
    idiv${suffix}   $second
1:
    .if $wide
    SET_WIDE_VREG $result, rINSTq           # eax <- vBB
    .else
    SET_VREG $result, rINSTq                # eax <- vBB
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
2:
    .if $rem
    xor${suffix} $result, $result
    .else
    neg${suffix} $result
    .endif
    jmp     1b
