/*
 * Generic 64-bit binary operation.
 */
    /* binop/2addr vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    $instr                                  # for ex: addq   %rax,(rFP,%rcx,4)
    CLEAR_WIDE_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
