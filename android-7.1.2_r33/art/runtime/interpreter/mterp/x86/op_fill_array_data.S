    /* fill-array-data vAA, +BBBBBBBB */
    EXPORT_PC
    movl    2(rPC), %ecx                    # ecx <- BBBBbbbb
    leal    (rPC,%ecx,2), %ecx              # ecx <- PC + BBBBbbbb*2
    GET_VREG %eax, rINST                    # eax <- vAA (array object)
    movl    %eax, OUT_ARG0(%esp)
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpFillArrayData)      # (obj, payload)
    REFRESH_IBASE
    testb   %al, %al                        # 0 means an exception is thrown
    jz      MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
