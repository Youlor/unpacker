    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST ${opnum}
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpIputObjectQuick)
    testb   %al, %al
    jz      MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
