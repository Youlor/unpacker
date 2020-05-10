    # iput-wide vA, vB, field              /* CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC()
    FETCH(a0, 1)                           # a0 <- field ref CCCC
    GET_OPB(a1)                            # a1 <- B
    GET_VREG(a1, a1)                       # a1 <- fp[B], the object pointer
    GET_OPA4(a2)                           # a2 <- A+
    EAS2(a2, rFP, a2)                      # a2 <- &fp[A]
    lw    a3, OFF_FP_METHOD(rFP)           # a3 <- referrer
    PREFETCH_INST(2)                       # load rINST
    JAL(artSet64InstanceFromMterp)
    bnez  v0, MterpPossibleException       # bail out
    ADVANCE(2)                             # advance rPC
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    GOTO_OPCODE(t0)                        # jump to next instruction
