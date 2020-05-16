    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
#if MTERP_PROFILE_BRANCHES
    sll       a0, rINST, 16                #  a0 <- AAxx0000
    sra       rINST, a0, 24                #  rINST <- ssssssAA (sign-extended)
    EXPORT_PC()
    move      a0, rSELF
    addu      a1, rFP, OFF_FP_SHADOWFRAME
    move      a2, rINST
    JAL(MterpProfileBranch)                #  (self, shadow_frame, offset)
    bnez      v0, MterpOnStackReplacement  #  Note: offset must be in rINST
    addu      a2, rINST, rINST             #  a2 <- byte offset
    FETCH_ADVANCE_INST_RB(a2)              #  update rPC, load rINST
    /* If backwards branch refresh rIBASE */
    bgez      a2, 1f
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
1:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
#else
    sll       a0, rINST, 16                #  a0 <- AAxx0000
    sra       rINST, a0, 24                #  rINST <- ssssssAA (sign-extended)
    addu      a2, rINST, rINST             #  a2 <- byte offset
    FETCH_ADVANCE_INST_RB(a2)              #  update rPC, load rINST
    /* If backwards branch refresh rIBASE */
    bgez      a1, 1f
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
1:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
#endif
