    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    mov     r0, rSELF
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    blne    MterpSuspendCheck                       @ (self)
    mov    r0, #0
    mov    r1, #0
    b      MterpReturn
