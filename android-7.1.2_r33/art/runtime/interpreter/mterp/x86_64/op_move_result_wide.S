    /* move-result-wide vAA */
    movq    OFF_FP_RESULT_REGISTER(rFP), %rax    # get pointer to result JType.
    movq    (%rax), %rdx                         # Get wide
    SET_WIDE_VREG %rdx, rINSTq                   # v[AA] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
