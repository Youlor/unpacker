/*
 * Array object get.  vAA <- vBB[vCC].
 *
 * for: aget-object
 */
    /* op vAA, vBB, vCC */
    movzbq  2(rPC), %rax                    # rax <- BB
    movzbq  3(rPC), %rcx                    # rcx <- CC
    GET_VREG OUT_32_ARG0, %rax              # eax <- vBB (array object)
    GET_VREG OUT_32_ARG1, %rcx              # ecx <- vCC (requested index)
    EXPORT_PC
    call    SYMBOL(artAGetObjectFromMterp)  # (array, index)
    movq    rSELF, %rcx
    cmpq    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    SET_VREG_OBJECT %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
