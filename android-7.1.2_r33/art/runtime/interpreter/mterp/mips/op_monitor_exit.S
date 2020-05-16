    /*
     * Unlock an object.
     *
     * Exceptions that occur when unlocking a monitor need to appear as
     * if they happened at the following instruction.  See the Dalvik
     * instruction spec.
     */
    /* monitor-exit vAA */
    EXPORT_PC()
    GET_OPA(a2)                            # a2 <- AA
    GET_VREG(a0, a2)                       # a0 <- vAA (object)
    move   a1, rSELF                       # a1 <- self
    JAL(artUnlockObjectFromCode)           # v0 <- artUnlockObject(obj, self)
    bnez v0, MterpException
    FETCH_ADVANCE_INST(1)                  # advance rPC, load rINST
    GET_INST_OPCODE(t0)                    # extract opcode from rINST
    GOTO_OPCODE(t0)                        # jump to next instruction
