    /* iget-wide-quick vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    movq    (%ecx,%eax,1), %xmm0
    andb    $$0xf, rINSTbl                  # rINST <- A
    SET_WIDE_FP_VREG %xmm0, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
