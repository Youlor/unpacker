/*
 * Return a 64-bit value.
 */
    /* return-wide vAA */
    .extern MterpThreadFenceForConstructor
    call    SYMBOL(MterpThreadFenceForConstructor)
    movl    rSELF, %eax
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    jz      1f
    movl    %eax, OUT_ARG0(%esp)
    call    SYMBOL(MterpSuspendCheck)
1:
    GET_VREG %eax, rINST                    # eax <- v[AA+0]
    GET_VREG_HIGH %ecx, rINST               # ecx <- v[AA+1]
    jmp     MterpReturn
