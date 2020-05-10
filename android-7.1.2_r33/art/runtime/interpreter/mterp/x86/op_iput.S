%default { "handler":"artSet32InstanceFromMterp" }
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern $handler
    EXPORT_PC
    movzwl  2(rPC), %eax                    # eax<- 0000CCCC
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movzbl  rINSTbl, %ecx                   # ecx<- BA
    sarl    $$4, %ecx                       # ecx<- B
    GET_VREG %ecx, %ecx
    movl    %ecx, OUT_ARG1(%esp)            # the object pointer
    andb    $$0xf, rINSTbl                  # rINST<- A
    GET_VREG %eax, rINST
    movl    %eax, OUT_ARG2(%esp)            # fp[A]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG3(%esp)            # referrer
    call    SYMBOL($handler)
    testb   %al, %al
    jnz     MterpPossibleException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
