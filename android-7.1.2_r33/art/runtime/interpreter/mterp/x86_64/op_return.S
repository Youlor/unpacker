/*
 * Return a 32-bit value.
 *
 * for: return, return-object
 */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movq    rSELF, OUT_ARG0
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(OUT_ARG0)
    jz      1f
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINSTq                   # eax <- vAA
    jmp     MterpReturn
