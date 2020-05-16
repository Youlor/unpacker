%default { "is_object":"0", "helper":"artGet32StaticFromCode" }
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
 */
    /* op vAA, field@BBBB */
    .extern $helper
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL($helper)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    .if $is_object
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    .else
    SET_VREG %eax, rINST                    # fp[A] <- value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
