%default { "helper":"artSet32StaticFromCode"}
    /*
     * General SPUT handler.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    # op vAA, field                        /* BBBB */
    EXPORT_PC()
    FETCH(a0, 1)                           # a0 <- field ref BBBB
    GET_OPA(a3)                            # a3 <- AA
    GET_VREG(a1, a3)                       # a1 <- fp[AA], the object pointer
    lw    a2, OFF_FP_METHOD(rFP)           # a2 <- method
    move  a3, rSELF                        # a3 <- self
    PREFETCH_INST(2)                       # load rINST
    JAL($helper)
    bnez  v0, MterpException               # bail out
    ADVANCE(2)                             # advance rPC
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    GOTO_OPCODE(t0)                        # jump to next instruction
