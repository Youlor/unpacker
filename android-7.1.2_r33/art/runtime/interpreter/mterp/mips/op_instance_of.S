    /*
     * Check to see if an object reference is an instance of a class.
     *
     * Most common situation is a non-null object, being compared against
     * an already-resolved class.
     */
    # instance-of vA, vB, class            /* CCCC */
    EXPORT_PC()
    FETCH(a0, 1)                           # a0 <- CCCC
    GET_OPB(a1)                            # a1 <- B
    EAS2(a1, rFP, a1)                      # a1 <- &object
    lw    a2, OFF_FP_METHOD(rFP)           # a2 <- method
    move  a3, rSELF                        # a3 <- self
    GET_OPA4(rOBJ)                         # rOBJ <- A+
    JAL(MterpInstanceOf)                   # v0 <- Mterp(index, &obj, method, self)
    lw   a1, THREAD_EXCEPTION_OFFSET(rSELF)
    PREFETCH_INST(2)                       # load rINST
    bnez a1, MterpException
    ADVANCE(2)                             # advance rPC
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    SET_VREG_GOTO(v0, rOBJ, t0)            # vA <- v0
