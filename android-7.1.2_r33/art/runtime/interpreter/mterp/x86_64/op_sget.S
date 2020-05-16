%default { "is_object":"0", "helper":"artGet32StaticFromCode", "wide":"0" }
/*
 * General SGET handler wrapper.
 *
 * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short, sget-wide
 */
    /* op vAA, field@BBBB */
    .extern $helper
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    movq    rSELF, OUT_ARG2                 # self
    call    SYMBOL($helper)
    movq    rSELF, %rcx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    .if $is_object
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    .else
    .if $wide
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <- value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <- value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
