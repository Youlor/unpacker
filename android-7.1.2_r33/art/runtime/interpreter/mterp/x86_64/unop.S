%default {"preinstr":"", "instr":"", "wide":"0"}
/*
 * Generic 32/64-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $$4,rINST                       # rINST <- B
    .if ${wide}
    GET_WIDE_VREG %rax, rINSTq              # rax <- vB
    .else
    GET_VREG %eax, rINSTq                   # eax <- vB
    .endif
    andb    $$0xf,%cl                       # ecx <- A
$preinstr
$instr
    .if ${wide}
    SET_WIDE_VREG %rax, %rcx
    .else
    SET_VREG %eax, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
