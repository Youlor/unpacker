    /*
     * Throw an exception object in the current thread.
     */
    /* throw vAA */
    EXPORT_PC
    srl     a2, rINST, 8                # a2 <- AA
    GET_VREG_U a0, a2                   # a0 <- vAA (exception object)
    beqzc   a0, common_errNullObject
    sd      a0, THREAD_EXCEPTION_OFFSET(rSELF)  # thread->exception <- obj
    b       MterpException
