    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwq  4(rPC), %rcx                    # ecx<- BBBB
    movzwq  2(rPC), %rax                    # eax<- AAAA
    GET_WIDE_VREG %rdx, %rcx                # rdx <- v[B]
    SET_WIDE_VREG %rdx, %rax                # v[A] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
