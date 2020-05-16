/*
 * SPUT_WIDE handler wrapper.
 *
 */
    /* sput-wide vAA, field@BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC
    movzwq  2(rPC), OUT_ARG0                # field ref BBBB
    movq    OFF_FP_METHOD(rFP), OUT_ARG1    # referrer
    leaq    VREG_ADDRESS(rINSTq), OUT_ARG2  # &fp[AA]
    movq    rSELF, OUT_ARG3                 # self
    call    SYMBOL(artSet64IndirectStaticFromMterp)
    testb   %al, %al
    jnz     MterpException
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
