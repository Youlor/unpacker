    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    JAL(MterpThreadFenceForConstructor)
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    move      a0, rSELF
    and       ra, (THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    beqz      ra, 1f
    JAL(MterpSuspendCheck)                 # (self)
1:
    GET_OPA(a2)                            #  a2 <- AA
    GET_VREG(v0, a2)                       #  v0 <- vAA
    move      v1, zero
    b         MterpReturn
