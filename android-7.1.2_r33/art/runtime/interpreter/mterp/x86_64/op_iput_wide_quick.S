    /* iput-wide-quick vA, vB, offset@CCCC */
    movzbq    rINSTbl, %rcx                 # rcx<- BA
    sarl      $$4, %ecx                     # ecx<- B
    GET_VREG  %ecx, %rcx                    # vB (object we're operating on)
    testl     %ecx, %ecx                    # is object null?
    je        common_errNullObject
    movzwq    2(rPC), %rax                  # rax<- field byte offset
    leaq      (%rcx,%rax,1), %rcx           # ecx<- Address of 64-bit target
    andb      $$0xf, rINSTbl                # rINST<- A
    GET_WIDE_VREG %rax, rINSTq              # rax<- fp[A]/fp[A+1]
    movq      %rax, (%rcx)                  # obj.field<- r0/r1
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
