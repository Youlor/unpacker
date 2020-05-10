    /*
     * 64-bit instance field get.
     *
     * for: iget-wide
     */
    EXPORT_PC()
    FETCH(a0, 1)                           # a0 <- field byte offset
    GET_OPB(a1)                            # a1 <- B
    GET_VREG(a1, a1)                       # a1 <- fp[B], the object pointer
    lw    a2, OFF_FP_METHOD(rFP)           # a2 <- referrer
    move  a3, rSELF                        # a3 <- self
    JAL(artGet64InstanceFromCode)
    lw   a3, THREAD_EXCEPTION_OFFSET(rSELF)
    GET_OPA4(a2)                           # a2<- A+
    PREFETCH_INST(2)                       # load rINST
    bnez a3, MterpException                # bail out
    SET_VREG64(v0, v1, a2)                 # fp[A] <- v0/v1
    ADVANCE(2)                             # advance rPC
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    GOTO_OPCODE(t0)                        # jump to next instruction
