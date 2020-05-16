    /* iput-wide vA, vB, field//CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    lhu      a0, 2(rPC)                 # a0 <- field ref CCCC
    srl      a1, rINST, 12              # a1 <- B
    GET_VREG_U a1, a1                   # a1 <- fp[B], the object pointer
    ext      a2, rINST, 8, 4            # a2 <- A
    dlsa     a2, a2, rFP, 2             # a2 <- &fp[A]
    ld       a3, OFF_FP_METHOD(rFP)     # a3 <- referrer
    PREFETCH_INST 2
    jal      artSet64InstanceFromMterp
    bnez     v0, MterpPossibleException # bail out
    ADVANCE 2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
