%default {"result":"%eax"}
/*
 * Generic 32-bit binary operation.  Provide an "instr" line that
 * specifies an instruction that performs "result = eax op (rFP,%ecx,4)".
 * This could be an x86 instruction or a function call.  (If the result
 * comes back in a register other than eax, you can override "result".)
 *
 * For: add-int, sub-int, and-int, or-int,
 *      xor-int, shl-int, shr-int, ushr-int
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG %eax, %rax                     # eax <- vBB
    $instr                                  # ex: addl    (rFP,%rcx,4),%eax
    SET_VREG $result, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
