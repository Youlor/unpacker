    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    .extern MterpSuspendCheck
    jal     MterpThreadFenceForConstructor
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)
    move    a0, rSELF
    and     ra, ra, (THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    beqzc   ra, 1f
    jal     MterpSuspendCheck           # (self)
1:
    srl     a2, rINST, 8                # a2 <- AA
    GET_VREG_U a0, a2                   # a0 <- vAA
    b       MterpReturn
