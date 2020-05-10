    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST ${opnum}
    movq    rINSTq, OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpSputObject)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
