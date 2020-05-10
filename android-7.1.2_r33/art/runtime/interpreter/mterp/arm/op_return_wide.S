    /*
     * Return a 64-bit value.
     */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov     r2, rINST, lsr #8           @ r2<- AA
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[AA]
    ldmia   r2, {r0-r1}                 @ r0/r1 <- vAA/vAA+1
    b       MterpReturn
