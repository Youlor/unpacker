%default { "is_object":"0", "helper":"artGet32InstanceFromCode", "wide":"0"}
/*
 * General instance field get.
 *
 * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short, iget-wide
 */
    EXPORT_PC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    movzwl  2(rPC), OUT_32_ARG0             # eax <- field ref CCCC
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3
    call    SYMBOL($helper)
    movq    rSELF, %rcx
    cmpq    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $$0xf, rINSTbl                  # rINST <- A
    .if $is_object
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <-value
    .else
    .if $wide
    SET_WIDE_VREG %rax, rINSTq              # fp[A] <-value
    .else
    SET_VREG %eax, rINSTq                   # fp[A] <-value
    .endif
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
