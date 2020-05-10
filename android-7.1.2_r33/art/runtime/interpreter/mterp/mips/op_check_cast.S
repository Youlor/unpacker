    /*
     * Check to see if a cast from one class to another is allowed.
     */
    # check-cast vAA, class                /* BBBB */
    EXPORT_PC()
    FETCH(a0, 1)                           #  a0 <- BBBB
    GET_OPA(a1)                            #  a1 <- AA
    EAS2(a1, rFP, a1)                      #  a1 <- &object
    lw     a2, OFF_FP_METHOD(rFP)          #  a2 <- method
    move   a3, rSELF                       #  a3 <- self
    JAL(MterpCheckCast)                    #  v0 <- CheckCast(index, &obj, method, self)
    PREFETCH_INST(2)
    bnez   v0, MterpPossibleException
    ADVANCE(2)
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
