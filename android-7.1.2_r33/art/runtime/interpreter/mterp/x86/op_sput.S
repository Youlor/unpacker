%default { "helper":"artSet32StaticFromCode"}
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern $helper
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref BBBB
    GET_VREG rINST, rINST
    movl    rINST, OUT_ARG1(%esp)           # fp[AA]
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG2(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG3(%esp)            # self
    call    SYMBOL($helper)
    testb   %al, %al
    jnz     MterpException
    RESTORE_IBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
