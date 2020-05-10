    # const/class vAA, Class               /* BBBB */
    EXPORT_PC()
    FETCH(a0, 1)                        # a0 <- BBBB
    GET_OPA(a1)                         # a1 <- AA
    addu   a2, rFP, OFF_FP_SHADOWFRAME  # a2 <- shadow frame
    move   a3, rSELF
    JAL(MterpConstClass)
    PREFETCH_INST(2)                    # load rINST
    bnez   v0, MterpPossibleException
    ADVANCE(2)                          # advance rPC
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction
