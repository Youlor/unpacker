%default { "helper":"MterpFilledNewArray" }
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    # op vB, {vD, vE, vF, vG, vA}, class   /* CCCC */
    # op {vCCCC..v(CCCC+AA-1)}, type       /* BBBB */
    .extern $helper
    EXPORT_PC()
    addu   a0, rFP, OFF_FP_SHADOWFRAME     # a0 <- shadow frame
    move   a1, rPC
    move   a2, rSELF
    JAL($helper)                           #  v0 <- helper(shadow_frame, pc, self)
    beqz      v0,  MterpPossibleException  #  has exception
    FETCH_ADVANCE_INST(3)                  #  advance rPC, load rINST
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
