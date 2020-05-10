    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwl  4(rPC), %ecx                    # ecx<- BBBB
    movzwl  2(rPC), %eax                    # eax<- AAAA
    GET_WIDE_FP_VREG %xmm0, %ecx            # xmm0 <- v[B]
    SET_WIDE_FP_VREG %xmm0, %eax            # v[A] <- xmm0
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
