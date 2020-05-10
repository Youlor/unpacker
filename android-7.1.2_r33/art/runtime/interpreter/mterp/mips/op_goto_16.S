    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
#if MTERP_PROFILE_BRANCHES
    FETCH_S(rINST, 1)                      #  rINST <- ssssAAAA (sign-extended)
    EXPORT_PC()
    move      a0, rSELF
    addu      a1, rFP, OFF_FP_SHADOWFRAME
    move      a2, rINST
    JAL(MterpProfileBranch)                #  (self, shadow_frame, offset)
    bnez      v0, MterpOnStackReplacement  #  Note: offset must be in rINST
    addu      a1, rINST, rINST             #  a1 <- byte offset, flags set
    FETCH_ADVANCE_INST_RB(a1)              #  update rPC, load rINST
    bgez      a1, 1f
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
1:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
#else
    FETCH_S(rINST, 1)                      #  rINST <- ssssAAAA (sign-extended)
    addu      a1, rINST, rINST             #  a1 <- byte offset, flags set
    FETCH_ADVANCE_INST_RB(a1)              #  update rPC, load rINST
    bgez      a1, 1f
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
1:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
#endif
