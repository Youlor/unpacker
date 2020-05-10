/*
 * Unlock an object.
 *
 * Exceptions that occur when unlocking a monitor need to appear as
 * if they happened at the following instruction.  See the Dalvik
 * instruction spec.
 */
    /* monitor-exit vAA */
    EXPORT_PC
    GET_VREG OUT_32_ARG0, rINSTq
    movq    rSELF, OUT_ARG1
    call    SYMBOL(artUnlockObjectFromCode) # (object, self)
    testq   %rax, %rax
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
