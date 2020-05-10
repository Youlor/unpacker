%default { "helper":"MterpFilledNewArray" }
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    /* op vB, {vD, vE, vF, vG, vA}, class//CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type//BBBB */
    .extern $helper
    EXPORT_PC
    daddu   a0, rFP, OFF_FP_SHADOWFRAME
    move    a1, rPC
    move    a2, rSELF
    jal     $helper
    beqzc   v0, MterpPossibleException
    FETCH_ADVANCE_INST 3                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
