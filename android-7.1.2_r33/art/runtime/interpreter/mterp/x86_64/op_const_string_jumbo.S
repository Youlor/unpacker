    /* const/string vAA, String@BBBBBBBB */
    EXPORT_PC
    movl    2(rPC), OUT_32_ARG0             # OUT_32_ARG0 <- BBBB
    movq    rINSTq, OUT_ARG1
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpConstString)        # (index, tgt_reg, shadow_frame, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
