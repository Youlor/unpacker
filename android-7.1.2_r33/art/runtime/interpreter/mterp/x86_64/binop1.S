%default {"wide":"0"}
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_VREG %ecx, %rcx                     # eax <- vCC
    .if $wide
    GET_WIDE_VREG %rax, %rax                # rax <- vBB
    $instr                                  # ex: addl    %ecx,%eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, %rax                     # eax <- vBB
    $instr                                  # ex: addl    %ecx,%eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
