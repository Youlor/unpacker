%default { "helper":"UndefinedInvokeHandler" }
    /*
     * Generic invoke handler wrapper.
     */
    # op vB, {vD, vE, vF, vG, vA}, class   /* CCCC */
    # op {vCCCC..v(CCCC+AA-1)}, meth       /* BBBB */
    .extern $helper
    EXPORT_PC()
    move    a0, rSELF
    addu    a1, rFP, OFF_FP_SHADOWFRAME
    move    a2, rPC
    move    a3, rINST
    JAL($helper)
    beqz    v0, MterpException
    FETCH_ADVANCE_INST(3)
    JAL(MterpShouldSwitchInterpreters)
    bnez    v0, MterpFallback
    GET_INST_OPCODE(t0)
    GOTO_OPCODE(t0)
