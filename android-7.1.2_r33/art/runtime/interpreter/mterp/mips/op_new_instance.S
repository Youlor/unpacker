    /*
     * Create a new instance of a class.
     */
    # new-instance vAA, class              /* BBBB */
    EXPORT_PC()
    addu   a0, rFP, OFF_FP_SHADOWFRAME
    move   a1, rSELF
    move   a2, rINST
    JAL(MterpNewInstance)
    beqz   v0, MterpPossibleException
    FETCH_ADVANCE_INST(2)               # advance rPC, load rINST
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction
