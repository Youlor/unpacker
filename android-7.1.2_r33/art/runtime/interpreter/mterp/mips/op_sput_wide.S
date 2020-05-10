    /*
     * 64-bit SPUT handler.
     */
    # sput-wide vAA, field                 /* BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC()
    FETCH(a0, 1)                           # a0 <- field ref CCCC
    lw    a1, OFF_FP_METHOD(rFP)           # a1 <- method
    GET_OPA(a2)                            # a2 <- AA
    EAS2(a2, rFP, a2)                      # a2 <- &fp[AA]
    move  a3, rSELF                        # a3 <- self
    PREFETCH_INST(2)                       # load rINST
    JAL(artSet64IndirectStaticFromMterp)
    bnez  v0, MterpException               # bail out
    ADVANCE(2)                             # advance rPC
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    GOTO_OPCODE(t0)                        # jump to next instruction
