    /* iput-wide vA, vB, field@CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref CCCC
    movzbq  rINSTbl, %rcx                   # rcx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    GET_VREG OUT_32_ARG1, %rcx              # the object pointer
    andb    $$0xf, rINSTbl                  # rINST <- A
    leaq    VREG_ADDRESS(rINSTq), OUT_ARG2  # &fp[A]
    movq    OFF_FP_METHOD(rFP), OUT_ARG3    # referrer
    call    SYMBOL(artSet64InstanceFromMterp)
    testb   %al, %al
    jnz     MterpPossibleException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
