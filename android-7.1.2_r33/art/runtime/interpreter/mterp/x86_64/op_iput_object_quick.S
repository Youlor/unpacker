    EXPORT_PC
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG0
    movq    rPC, OUT_ARG1
    REFRESH_INST ${opnum}
    movl    rINST, OUT_32_ARG2
    call    SYMBOL(MterpIputObjectQuick)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
