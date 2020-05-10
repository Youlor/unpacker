/*
 * Create a new instance of a class.
 */
    /* new-instance vAA, class@BBBB */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG1(%esp)
    REFRESH_INST ${opnum}
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpNewInstance)
    RESTORE_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
