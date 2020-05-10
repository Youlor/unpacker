%default {"result":"%eax"}
/*
 * Generic 32-bit "shift/2addr" operation.
 */
    /* shift/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # eax <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # eax <- vBB
    andb    $$0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- vAA
    $instr                                  # ex: sarl %cl, %eax
    SET_VREG $result, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
