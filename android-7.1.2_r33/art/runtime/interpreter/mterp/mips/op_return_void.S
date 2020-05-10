    .extern MterpThreadFenceForConstructor
    JAL(MterpThreadFenceForConstructor)
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    move      a0, rSELF
    and       ra, (THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    beqz      ra, 1f
    JAL(MterpSuspendCheck)                 # (self)
1:
    move      v0, zero
    move      v1, zero
    b         MterpReturn
