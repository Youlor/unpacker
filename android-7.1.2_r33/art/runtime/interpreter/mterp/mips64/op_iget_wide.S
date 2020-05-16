    /*
     * 64-bit instance field get.
     *
     * for: iget-wide
     */
    .extern artGet64InstanceFromCode
    EXPORT_PC
    lhu      a0, 2(rPC)                 # a0 <- field ref CCCC
    srl      a1, rINST, 12              # a1 <- B
    GET_VREG_U a1, a1                   # a1 <- fp[B], the object pointer
    ld       a2, OFF_FP_METHOD(rFP)     # a2 <- referrer
    move     a3, rSELF                  # a3 <- self
    jal      artGet64InstanceFromCode
    ld       a3, THREAD_EXCEPTION_OFFSET(rSELF)
    ext      a2, rINST, 8, 4            # a2 <- A
    PREFETCH_INST 2
    bnez     a3, MterpPossibleException # bail out
    SET_VREG_WIDE v0, a2                # fp[A] <- v0
    ADVANCE 2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
