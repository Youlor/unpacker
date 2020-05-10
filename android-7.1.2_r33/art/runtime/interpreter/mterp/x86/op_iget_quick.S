%default { "load":"movl"}
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    ${load} (%ecx,%eax,1), %eax
    andb    $$0xf,rINSTbl                   # rINST <- A
    SET_VREG %eax, rINST                    # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
