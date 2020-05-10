/*
 * Throw an exception object in the current thread.
 */
    /* throw vAA */
    EXPORT_PC
    GET_VREG %eax, rINST                    # eax<- vAA (exception object)
    testl   %eax, %eax
    jz      common_errNullObject
    movl    rSELF,%ecx
    movl    %eax, THREAD_EXCEPTION_OFFSET(%ecx)
    jmp     MterpException
