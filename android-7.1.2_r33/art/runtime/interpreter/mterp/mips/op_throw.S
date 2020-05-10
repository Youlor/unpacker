    /*
     * Throw an exception object in the current thread.
     */
    /* throw vAA */
    EXPORT_PC()                              #  exception handler can throw
    GET_OPA(a2)                              #  a2 <- AA
    GET_VREG(a1, a2)                         #  a1 <- vAA (exception object)
    # null object?
    beqz  a1, common_errNullObject           #  yes, throw an NPE instead
    sw    a1, THREAD_EXCEPTION_OFFSET(rSELF) #  thread->exception <- obj
    b         MterpException
