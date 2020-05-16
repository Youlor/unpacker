    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    .extern artIGetObjectFromMterp
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG0, %rcx              # vB (object we're operating on)
    movzwl  2(rPC), OUT_32_ARG1             # eax <- field byte offset
    EXPORT_PC
    callq   SYMBOL(artIGetObjectFromMterp)  # (obj, offset)
    movq    rSELF, %rcx
    cmpq    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException                  # bail out
    andb    $$0xf, rINSTbl                  # rINST <- A
    SET_VREG_OBJECT %eax, rINSTq            # fp[A] <- value
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
