    /* const/class vAA, Class@BBBB */
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # eax <- OUT_ARG0
    movq    rINSTq, OUT_ARG1
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpConstClass)         # (index, tgt_reg, shadow_frame, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
