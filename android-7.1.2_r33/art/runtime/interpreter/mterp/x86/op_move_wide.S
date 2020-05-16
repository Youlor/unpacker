    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    GET_WIDE_FP_VREG %xmm0, rINST           # xmm0 <- v[B]
    SET_WIDE_FP_VREG %xmm0, %ecx            # v[A] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
