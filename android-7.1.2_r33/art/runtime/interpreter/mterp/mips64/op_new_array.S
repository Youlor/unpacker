    /*
     * Allocate an array of objects, specified with the array class
     * and a count.
     *
     * The verifier guarantees that this is an array class, so we don't
     * check for it here.
     */
    /* new-array vA, vB, class//CCCC */
    .extern MterpNewArray
    EXPORT_PC
    daddu   a0, rFP, OFF_FP_SHADOWFRAME
    move    a1, rPC
    move    a2, rINST
    move    a3, rSELF
    jal     MterpNewArray
    beqzc   v0, MterpPossibleException
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
