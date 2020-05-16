/*
 * Allocate an array of objects, specified with the array class
 * and a count.
 *
 * The verifier guarantees that this is an array class, so we don't
 * check for it here.
 */
    /* new-array vA, vB, class@CCCC */
    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST ${opnum}
    movq    rINSTq, OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpNewArray)
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
