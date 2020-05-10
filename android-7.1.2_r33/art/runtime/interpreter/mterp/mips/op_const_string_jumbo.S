    # const/string vAA, String          /* BBBBBBBB */
    EXPORT_PC()
    FETCH(a0, 1)                        # a0 <- bbbb (low)
    FETCH(a2, 2)                        # a2 <- BBBB (high)
    GET_OPA(a1)                         # a1 <- AA
    sll    a2, a2, 16
    or     a0, a0, a2                   # a0 <- BBBBbbbb
    addu   a2, rFP, OFF_FP_SHADOWFRAME  # a2 <- shadow frame
    move   a3, rSELF
    JAL(MterpConstString)               # v0 <- Mterp(index, tgt_reg, shadow_frame, self)
    PREFETCH_INST(3)                    # load rINST
    bnez   v0, MterpPossibleException
    ADVANCE(3)                          # advance rPC
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction
