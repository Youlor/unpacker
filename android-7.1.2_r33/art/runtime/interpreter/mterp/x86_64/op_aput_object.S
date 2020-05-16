/*
 * Store an object into an array.  vBB[vCC] <- vAA.
 */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST ${opnum}
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpAputObject)         # (array, index)
    testb   %al, %al
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
