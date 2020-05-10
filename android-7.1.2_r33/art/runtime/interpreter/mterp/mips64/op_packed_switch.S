%default { "func":"MterpDoPackedSwitch" }
    /*
     * Handle a packed-switch or sparse-switch instruction.  In both cases
     * we decode it and hand it off to a helper function.
     *
     * We don't really expect backward branches in a switch statement, but
     * they're perfectly legal, so we check for them here.
     *
     * for: packed-switch, sparse-switch
     */
    /* op vAA, +BBBBBBBB */
    .extern $func
    .extern MterpProfileBranch
    lh      a0, 2(rPC)                  # a0 <- bbbb (lo)
    lh      a1, 4(rPC)                  # a1 <- BBBB (hi)
    srl     a3, rINST, 8                # a3 <- AA
    ins     a0, a1, 16, 16              # a0 <- BBBBbbbb
    GET_VREG a1, a3                     # a1 <- vAA
    dlsa    a0, a0, rPC, 1              # a0 <- PC + BBBBbbbb*2
    jal     $func                       # v0 <- code-unit branch offset
    move    rINST, v0
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
