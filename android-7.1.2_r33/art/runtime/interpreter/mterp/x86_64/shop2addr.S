%default {"wide":"0"}
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %rcx                     # ecx <- vBB
    andb    $$0xf, rINSTbl                  # rINST <- A
    .if $wide
    GET_WIDE_VREG %rax, rINSTq              # rax <- vAA
    $instr                                  # ex: sarl %cl, %eax
    SET_WIDE_VREG %rax, rINSTq
    .else
    GET_VREG %eax, rINSTq                   # eax <- vAA
    $instr                                  # ex: sarl %cl, %eax
    SET_VREG %eax, rINSTq
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
