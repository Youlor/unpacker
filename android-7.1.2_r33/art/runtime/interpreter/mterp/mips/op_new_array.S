    /*
     * Allocate an array of objects, specified with the array class
     * and a count.
     *
     * The verifier guarantees that this is an array class, so we don't
     * check for it here.
     */
    /* new-array vA, vB, class@CCCC */
    EXPORT_PC()
    addu   a0, rFP, OFF_FP_SHADOWFRAME
    move   a1, rPC
    move   a2, rINST
    move   a3, rSELF
    JAL(MterpNewArray)
    beqz   v0, MterpPossibleException
    FETCH_ADVANCE_INST(2)               # advance rPC, load rINST
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction
