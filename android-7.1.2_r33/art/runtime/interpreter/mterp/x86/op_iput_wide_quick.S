    /* iput-wide-quick vA, vB, offset@CCCC */
    movzbl    rINSTbl, %ecx                 # ecx<- BA
    sarl      $$4, %ecx                     # ecx<- B
    GET_VREG  %ecx, %ecx                    # vB (object we're operating on)
    testl     %ecx, %ecx                    # is object null?
    je        common_errNullObject
    movzwl    2(rPC), %eax                  # eax<- field byte offset
    leal      (%ecx,%eax,1), %ecx           # ecx<- Address of 64-bit target
    andb      $$0xf, rINSTbl                # rINST<- A
    GET_WIDE_FP_VREG %xmm0, rINST           # xmm0<- fp[A]/fp[A+1]
    movq      %xmm0, (%ecx)                 # obj.field<- r0/r1
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
