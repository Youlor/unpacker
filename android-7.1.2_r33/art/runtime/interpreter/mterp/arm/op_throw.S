    /*
     * Throw an exception object in the current thread.
     */
    /* throw vAA */
    EXPORT_PC
    mov      r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r1, r2                      @ r1<- vAA (exception object)
    cmp      r1, #0                      @ null object?
    beq      common_errNullObject        @ yes, throw an NPE instead
    str      r1, [rSELF, #THREAD_EXCEPTION_OFFSET]  @ thread->exception<- obj
    b        MterpException
