%default { "helper":"artSet32StaticFromCode" }
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    .extern $helper
    EXPORT_PC
    lhu     a0, 2(rPC)                  # a0 <- field ref BBBB
    srl     a3, rINST, 8                # a3 <- AA
    GET_VREG a1, a3                     # a1 <- fp[AA]
    ld      a2, OFF_FP_METHOD(rFP)
    move    a3, rSELF
    PREFETCH_INST 2                     # Get next inst, but don't advance rPC
    jal     $helper
    bnezc   v0, MterpException          # 0 on success
    ADVANCE 2                           # Past exception point - now advance rPC
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
