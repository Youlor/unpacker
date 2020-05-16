/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $$4, %ecx                       # ecx<- B
    GET_VREG %eax, %ecx                     # eax<- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # eax<- v[B+1]
    andb    $$0xF, rINSTbl                  # rINST<- A
    $instr1                                 # ex: addl   %eax,(rFP,rINST,4)
    $instr2                                 # ex: adcl   %ecx,4(rFP,rINST,4)
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
