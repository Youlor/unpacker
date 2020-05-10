    /* move-exception vAA */
    movl    rSELF, %ecx
    movl    THREAD_EXCEPTION_OFFSET(%ecx), %eax
    SET_VREG_OBJECT %eax, rINST             # fp[AA] <- exception object
    movl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
