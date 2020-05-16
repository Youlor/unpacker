%default { "is_object":"0", "helper":"artGet32StaticFromCode", "extend":"" }
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */
    .extern $helper
    EXPORT_PC
    lhu     a0, 2(rPC)                  # a0 <- field ref BBBB
    ld      a1, OFF_FP_METHOD(rFP)
    move    a2, rSELF
    jal     $helper
    ld      a3, THREAD_EXCEPTION_OFFSET(rSELF)
    srl     a2, rINST, 8                # a2 <- AA
    $extend
    PREFETCH_INST 2
    bnez    a3, MterpException          # bail out
    .if $is_object
    SET_VREG_OBJECT v0, a2              # fp[AA] <- v0
    .else
    SET_VREG v0, a2                     # fp[AA] <- v0
    .endif
    ADVANCE 2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0
