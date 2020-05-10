%default { "handler":"artSet32InstanceFromMterp"}
/*
 * General 32-bit instance field put.
 *
 * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
 */
    /* op vA, vB, field@CCCC */
    .extern $handler
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # field ref <- 0000CCCC
    movzbq  rINSTbl, %rcx                   # rcx<- BA
    sarl    $$4, %ecx                       # ecx<- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $$0xf, rINSTbl                  # rINST<- A
    GET_VREG OUT_32_ARG2, rINSTq            # fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL($handler)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
