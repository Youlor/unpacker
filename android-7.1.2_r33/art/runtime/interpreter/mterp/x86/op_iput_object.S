    EXPORT_PC
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    movl    rPC, OUT_ARG1(%esp)
    REFRESH_INST ${opnum}
    movl    rINST, OUT_ARG2(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG3(%esp)
    call    SYMBOL(MterpIputObject)
    testb   %al, %al
    jz      MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
