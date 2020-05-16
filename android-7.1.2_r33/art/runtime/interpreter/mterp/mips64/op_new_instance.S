    /*
     * Create a new instance of a class.
     */
    /* new-instance vAA, class//BBBB */
    .extern MterpNewInstance
    EXPORT_PC
    daddu   a0, rFP, OFF_FP_SHADOWFRAME
    move    a1, rSELF
    move    a2, rINST
    jal     MterpNewInstance            # (shadow_frame, self, inst_data)
    beqzc   v0, MterpPossibleException
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
