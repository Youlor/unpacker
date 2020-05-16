/*
 * Check to see if a cast from one class to another is allowed.
 */
    /* check-cast vAA, class@BBBB */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- BBBB
    movl    %eax, OUT_ARG0(%esp)
    leal    VREG_ADDRESS(rINST), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    OFF_FP_METHOD(rFP),%eax
    movl    %eax, OUT_ARG2(%esp)
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)
    call    SYMBOL(MterpCheckCast)          # (index, &obj, method, self)
    RESTORE_IBASE
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
