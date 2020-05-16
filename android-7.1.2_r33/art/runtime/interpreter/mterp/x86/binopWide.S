/*
 * Generic 64-bit binary operation.
 */
    /* binop vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)            # save rIBASE
    GET_VREG rIBASE, %eax                   # rIBASE <- v[BB+0]
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1]
    $instr1                                 # ex: addl   (rFP,%ecx,4),rIBASE
    $instr2                                 # ex: adcl   4(rFP,%ecx,4),%eax
    SET_VREG rIBASE, rINST                  # v[AA+0] <- rIBASE
    movl    LOCAL0(%esp), rIBASE            # restore rIBASE
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
