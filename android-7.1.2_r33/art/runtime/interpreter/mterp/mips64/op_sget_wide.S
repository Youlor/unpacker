    /*
     * SGET_WIDE handler wrapper.
     *
     */
    /* sget-wide vAA, field//BBBB */
    .extern artGet64StaticFromCode
    EXPORT_PC
    lhu     a0, 2(rPC)                  # a0 <- field ref BBBB
    ld      a1, OFF_FP_METHOD(rFP)
    move    a2, rSELF
    jal     artGet64StaticFromCode
    ld      a3, THREAD_EXCEPTION_OFFSET(rSELF)
    srl     a4, rINST, 8                # a4 <- AA
    bnez    a3, MterpException          # bail out
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    SET_VREG_WIDE v0, a4
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
