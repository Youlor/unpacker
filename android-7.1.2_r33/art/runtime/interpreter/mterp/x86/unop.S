%default {"instr":""}
/*
 * Generic 32-bit unary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = op eax".
 */
    /* unop vA, vB */
    movzbl  rINSTbl,%ecx                    # ecx <- A+
    sarl    $$4,rINST                       # rINST <- B
    GET_VREG %eax, rINST                    # eax <- vB
    andb    $$0xf,%cl                       # ecx <- A
    $instr
    SET_VREG %eax, %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
