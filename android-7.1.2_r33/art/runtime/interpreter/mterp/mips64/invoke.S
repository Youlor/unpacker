%default { "helper":"UndefinedInvokeHandler" }
    /*
     * Generic invoke handler wrapper.
     */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern $helper
    .extern MterpShouldSwitchInterpreters
    EXPORT_PC
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    move    a2, rPC
    move    a3, rINST
    jal     $helper
    beqzc   v0, MterpException
    FETCH_ADVANCE_INST 3
    jal     MterpShouldSwitchInterpreters
    bnezc   v0, MterpFallback
    GET_INST_OPCODE v0
    GOTO_OPCODE v0
