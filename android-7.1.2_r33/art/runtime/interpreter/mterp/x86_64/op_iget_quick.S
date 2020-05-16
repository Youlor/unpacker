%default { "load":"movl", "wide":"0"}
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick, iget-wide-quick */
    /* op vA, vB, offset@CCCC */
    movl    rINST, %ecx                     # rcx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    movzwq  2(rPC), %rax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $$0xf,rINSTbl                   # rINST <- A
    .if $wide
    movq (%rcx,%rax,1), %rax
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    ${load} (%rcx,%rax,1), %eax
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
