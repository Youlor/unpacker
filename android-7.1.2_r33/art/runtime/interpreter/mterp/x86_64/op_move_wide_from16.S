    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    movzwl  2(rPC), %ecx                    # ecx <- BBBB
    GET_WIDE_VREG %rdx, %rcx                # rdx <- v[B]
    SET_WIDE_VREG %rdx, rINSTq              # v[A] <- rdx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
