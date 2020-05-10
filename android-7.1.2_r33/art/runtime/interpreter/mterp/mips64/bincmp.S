    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform, e.g. for
     * "if-le" you would use "le".
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    .extern MterpProfileBranch
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    lh      rINST, 2(rPC)               # rINST <- offset (sign-extended CCCC)
    GET_VREG a0, a2                     # a0 <- vA
    GET_VREG a1, a3                     # a1 <- vB
    b${condition}c a0, a1, 1f
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
