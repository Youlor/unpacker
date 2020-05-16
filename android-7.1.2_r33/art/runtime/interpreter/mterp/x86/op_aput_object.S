/*
 * Store an object into an array.  vBB[vCC] <- vAA.
 */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST ${opnum}
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpAputObject)         # (array, index)
    RESTORE_IBASE
    testb   %al, %al
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
