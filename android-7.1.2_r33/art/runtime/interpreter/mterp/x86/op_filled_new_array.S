%default { "helper":"MterpFilledNewArray" }
/*
 * Create a new array with elements filled from registers.
 *
 * for: filled-new-array, filled-new-array/range
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type@BBBB */
    .extern $helper
    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)
    call    SYMBOL($helper)
    REFRESH_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
