%default {"result":"%eax","tmp":"%ecx"}
/*
 * Generic 32-bit binary operation in which both operands loaded to
 * registers (op0 in eax, op1 in ecx).
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC),%eax                     # eax <- BB
    movzbl  3(rPC),%ecx                     # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # eax <- vBB
    $instr                                  # ex: addl    %ecx,%eax
    SET_VREG $result, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
