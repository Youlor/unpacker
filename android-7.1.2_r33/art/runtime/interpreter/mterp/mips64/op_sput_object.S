    .extern MterpSputObject
    EXPORT_PC
    daddu   a0, rFP, OFF_FP_SHADOWFRAME
    move    a1, rPC
    move    a2, rINST
    move    a3, rSELF
    jal     MterpSputObject
    beqzc   v0, MterpException
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
