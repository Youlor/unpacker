/*
 * Synchronize on an object.
 */
    /* monitor-enter vAA */
    EXPORT_PC
    GET_VREG OUT_32_ARG0, rINSTq
    movq    rSELF, OUT_ARG1
    call    SYMBOL(artLockObjectFromCode)   # (object, self)
    testq   %rax, %rax
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
