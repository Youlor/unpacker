%default { "helper":"artSet32StaticFromCode"}
/*
 * General SPUT handler wrapper.
 *
 * for: sput, sput-boolean, sput-byte, sput-char, sput-short
 */
    /* op vAA, field@BBBB */
    .extern $helper
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    GET_VREG OUT_32_ARG1, rINSTq            # fp[AA]
    movq    OFF_FP_METHOD(rFP), OUT_ARG2    # referrer
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL($helper)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
