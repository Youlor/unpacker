/*
 * Check to see if a cast from one class to another is allowed.
 */
    /* check-cast vAA, class@BBBB */
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # OUT_ARG0 <- BBBB
    leaq    VREG_ADDRESS(rINSTq), OUT_ARG1
    movq    OFF_FP_METHOD(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpCheckCast)          # (index, &obj, method, self)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
