%default { "reg":"rINST", "store":"movl" }
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $$0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINST                   # rINST <- v[A]
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    ${store}    ${reg}, (%ecx,%eax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
