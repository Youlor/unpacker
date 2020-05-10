    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform, e.g. for
     * "if-lez" you would use "le".
     *
     * For: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    .extern MterpProfileBranch
    srl     a2, rINST, 8                # a2 <- AA
    lh      rINST, 2(rPC)               # rINST <- offset (sign-extended BBBB)
    GET_VREG a0, a2                     # a0 <- vAA
    b${condition}zc a0, 1f
    li      rINST, 2                    # offset if branch not taken
1:
#if MTERP_PROFILE_BRANCHES
    EXPORT_PC
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    move    a2, rINST
    jal     MterpProfileBranch          # (self, shadow_frame, offset)
    bnezc   v0, MterpOnStackReplacement # Note: offset must be in rINST
#endif
    dlsa    rPC, rINST, rPC, 1          # rPC <- rPC + offset * 2
    lw      ra, THREAD_FLAGS_OFFSET(rSELF)  # Preload flags for MterpCheckSuspendAndContinue
    move    a0, rINST                   # a0 <- offset
    FETCH_INST                          # load rINST
    bltz    a0, MterpCheckSuspendAndContinue  # suspend check if backwards branch
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
