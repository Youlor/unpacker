/*
 * 64-bit instance field get.
 *
 * for: iget-wide
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
    call    SYMBOL(artGet64InstanceFromCode)
    mov     rSELF, %ecx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $$0xf, rINSTbl                  # rINST <- A
    SET_VREG %eax, rINST
    SET_VREG_HIGH %edx, rINST
    RESTORE_IBASE_FROM_SELF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
