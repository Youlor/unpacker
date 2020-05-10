    /* move-result-wide vAA */
    movl    OFF_FP_RESULT_REGISTER(rFP), %eax    # get pointer to result JType.
    movl    4(%eax), %ecx                   # Get high
    movl    (%eax), %eax                    # Get low
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    SET_VREG_HIGH %ecx, rINST               # v[AA+1] <- ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
