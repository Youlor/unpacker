/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # eax <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rax, %rax                # rax <- v[BB]
    $instr                                  # ex: addq   (rFP,%rcx,4),%rax
    SET_WIDE_VREG %rax, rINSTq              # v[AA] <- rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
