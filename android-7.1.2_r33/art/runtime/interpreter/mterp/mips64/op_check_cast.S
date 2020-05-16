    /*
     * Check to see if a cast from one class to another is allowed.
     */
    /* check-cast vAA, class//BBBB */
    .extern MterpCheckCast
    EXPORT_PC
    lhu     a0, 2(rPC)                  # a0 <- BBBB
    srl     a1, rINST, 8                # a1 <- AA
    dlsa    a1, a1, rFP, 2              # a1 <- &object
    ld      a2, OFF_FP_METHOD(rFP)      # a2 <- method
    move    a3, rSELF                   # a3 <- self
    jal     MterpCheckCast              # (index, &obj, method, self)
    PREFETCH_INST 2
    bnez    v0, MterpPossibleException
    ADVANCE 2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
