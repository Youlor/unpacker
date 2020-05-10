%default { "reg":"rINST", "store":"movl" }
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # vB (object we're operating on)
    testl   %ecx, %ecx                      # is object null?
    je      common_errNullObject
    andb    $$0xf, rINSTbl                  # rINST <- A
    GET_VREG rINST, rINSTq                  # rINST <- v[A]
    movzwq  2(rPC), %rax                    # rax <- field byte offset
    ${store}    ${reg}, (%rcx,%rax,1)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
