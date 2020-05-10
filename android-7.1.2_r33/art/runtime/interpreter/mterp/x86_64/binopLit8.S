%default {"result":"%eax"}
/*
 * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than r0, you can override "result".)
 *
 * For: add-int/lit8, rsub-int/lit8
 *      and-int/lit8, or-int/lit8, xor-int/lit8,
 *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
 */
    /* binop/lit8 vAA, vBB, #+CC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movsbl  3(rPC), %ecx                    # rcx <- ssssssCC
    GET_VREG %eax, %rax                     # eax <- rBB
    $instr                                  # ex: addl %ecx,%eax
    SET_VREG $result, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
