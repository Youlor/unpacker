    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG %ecx, %ecx                     # vB (object we're operating on)
    movzwl  2(rPC), %eax                    # eax <- field byte offset
    movl    %ecx, OUT_ARG0(%esp)
    movl    %eax, OUT_ARG1(%esp)
    EXPORT_PC
    call    SYMBOL(artIGetObjectFromMterp)  # (obj, offset)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException                  # bail out
    andb    $$0xf,rINSTbl                   # rINST <- A
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
