%default {"result":"%eax"}
/*
 * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
 * that specifies an instruction that performs "result = eax op ecx".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int/lit16, rsub-int,
 *      and-int/lit16, or-int/lit16, xor-int/lit16
 */
    /* binop/lit16 vA, vB, #+CCCC */
    movl    rINST, %eax                     # rax <- 000000BA
    sarl    $$4, %eax                       # eax <- B
    GET_VREG %eax, %rax                     # eax <- vB
    andb    $$0xf, rINSTbl                  # rINST <- A
    movswl  2(rPC), %ecx                    # ecx <- ssssCCCC
    $instr                                  # for example: addl %ecx, %eax
    SET_VREG $result, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
