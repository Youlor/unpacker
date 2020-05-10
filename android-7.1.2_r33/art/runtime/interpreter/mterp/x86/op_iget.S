%default { "is_object":"0", "helper":"artGet32InstanceFromCode"}
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
 */
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax <- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    mov     rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL($helper)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $$0xf, rINSTbl                  # rINST <- A
    .if $is_object
    SET_VREG_OBJECT %eax, rINST             # fp[A] <-value
    .else
    SET_VREG %eax, rINST                    # fp[A] <-value
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
