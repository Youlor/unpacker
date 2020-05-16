    /*
     * General 32-bit SPUT handler.
     *
     * for: sput-object,
     */
    /* op vAA, field@BBBB */
    EXPORT_PC()
    addu   a0, rFP, OFF_FP_SHADOWFRAME
    move   a1, rPC
    move   a2, rINST
    move   a3, rSELF
    JAL(MterpSputObject)
    beqz   v0, MterpException
    FETCH_ADVANCE_INST(2)               # advance rPC, load rINST
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction
