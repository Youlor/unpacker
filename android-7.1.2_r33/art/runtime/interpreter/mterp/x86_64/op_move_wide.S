    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movl    rINST, %ecx                     # ecx <- BA
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    GET_WIDE_VREG %rdx, rINSTq              # rdx <- v[B]
    SET_WIDE_VREG %rdx, %rcx                # v[A] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
