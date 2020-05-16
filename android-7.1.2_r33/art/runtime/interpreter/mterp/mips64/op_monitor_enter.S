    /*
     * Synchronize on an object.
     */
    /* monitor-enter vAA */
    .extern artLockObjectFromCode
    EXPORT_PC
    srl     a2, rINST, 8                # a2 <- AA
    GET_VREG_U a0, a2                   # a0 <- vAA (object)
    move    a1, rSELF                   # a1 <- self
    jal     artLockObjectFromCode
    bnezc   v0, MterpException
    FETCH_ADVANCE_INST 1
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
