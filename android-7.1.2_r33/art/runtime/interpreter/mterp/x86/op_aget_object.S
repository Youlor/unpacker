/*
 * Array object get.  vAA <- vBB[vCC].
 *
 * for: aget-object
 */
    /* op vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB (array object)
    GET_VREG %ecx, %ecx                     # ecs <- vCC (requested index)
    EXPORT_PC
    movl    %eax, OUT_ARG0(%esp)
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(artAGetObjectFromMterp)  # (array, index)
    movl    rSELF, %ecx
    RESTORE_IBASE_FROM_SELF %ecx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    SET_VREG_OBJECT %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
