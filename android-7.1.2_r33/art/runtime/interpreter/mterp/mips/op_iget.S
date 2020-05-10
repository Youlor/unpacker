%default { "is_object":"0", "helper":"artGet32InstanceFromCode"}
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC()
    FETCH(a0, 1)                           # a0 <- field ref CCCC
    GET_OPB(a1)                            # a1 <- B
    GET_VREG(a1, a1)                       # a1 <- fp[B], the object pointer
    lw    a2, OFF_FP_METHOD(rFP)           # a2 <- referrer
    move  a3, rSELF                        # a3 <- self
    JAL($helper)
    lw   a3, THREAD_EXCEPTION_OFFSET(rSELF)
    GET_OPA4(a2)                           # a2<- A+
    PREFETCH_INST(2)                       # load rINST
    bnez  a3, MterpPossibleException        # bail out
    .if $is_object
    SET_VREG_OBJECT(v0, a2)                # fp[A] <- v0
    .else
    SET_VREG(v0, a2)                       # fp[A] <- v0
    .endif
    ADVANCE(2)                             #  advance rPC
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
