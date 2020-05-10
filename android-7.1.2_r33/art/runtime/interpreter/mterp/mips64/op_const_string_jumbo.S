    /* const/string vAA, String//BBBBBBBB */
    .extern MterpConstString
    EXPORT_PC
    lh      a0, 2(rPC)                  # a0 <- bbbb (low)
    lh      a4, 4(rPC)                  # a4 <- BBBB (high)
    srl     a1, rINST, 8                # a1 <- AA
    ins     a0, a4, 16, 16              # a0 <- BBBBbbbb
    daddu   a2, rFP, OFF_FP_SHADOWFRAME
    move    a3, rSELF
    jal     MterpConstString            # (index, tgt_reg, shadow_frame, self)
    PREFETCH_INST 3                     # load rINST
    bnez    v0, MterpPossibleException  # let reference interpreter deal with it.
    ADVANCE 3                           # advance rPC
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
