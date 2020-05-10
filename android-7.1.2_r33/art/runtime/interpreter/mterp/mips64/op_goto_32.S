    /*
     * Unconditional branch, 32-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     *
     * Unlike most opcodes, this one is allowed to branch to itself, so
     * our "backward branch" test must be "<=0" instead of "<0".
     */
    /* goto/32 +AAAAAAAA */
    .extern MterpProfileBranch
    lh      rINST, 2(rPC)               # rINST <- aaaa (low)
    lh      a1, 4(rPC)                  # a1 <- AAAA (high)
    ins     rINST, a1, 16, 16           # rINST <- offset (sign-extended AAAAaaaa)
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
    blez    a0, MterpCheckSuspendAndContinue  # suspend check if backwards branch
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
