    /*
     * Return a 64-bit value.
     */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    JAL(MterpThreadFenceForConstructor)
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    move      a0, rSELF
    and       ra, (THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    beqz      ra, 1f
    JAL(MterpSuspendCheck)                 # (self)
1:
    GET_OPA(a2)                            #  a2 <- AA
    EAS2(a2, rFP, a2)                      #  a2 <- &fp[AA]
    LOAD64(v0, v1, a2)                     #  v0/v1 <- vAA/vAA+1
    b         MterpReturn
