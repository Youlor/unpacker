/*
 * Return a 64-bit value.
 */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movq    rSELF, OUT_ARG0
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_WIDE_VREG %rax, rINSTq              # eax <- v[AA]
    jmp     MterpReturn
