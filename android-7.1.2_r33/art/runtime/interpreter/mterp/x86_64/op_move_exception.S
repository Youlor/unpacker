    /* move-exception vAA */
    movq    rSELF, %rcx
    movl    THREAD_EXCEPTION_OFFSET(%rcx), %eax
    SET_VREG_OBJECT %eax, rINSTq            # fp[AA] <- exception object
    movl    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
