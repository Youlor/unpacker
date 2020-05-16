/*
 * Throw an exception object in the current thread.
 */
    /* throw vAA */
    EXPORT_PC
    GET_VREG %eax, rINSTq                   # eax<- vAA (exception object)
    testb   %al, %al
    jz      common_errNullObject
    movq    rSELF, %rcx
    movq    %rax, THREAD_EXCEPTION_OFFSET(%rcx)
    jmp     MterpException
