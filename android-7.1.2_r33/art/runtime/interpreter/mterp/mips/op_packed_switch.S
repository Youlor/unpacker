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
    /* op vAA, +BBBB */
#if MTERP_PROFILE_BRANCHES
    FETCH(a0, 1)                           #  a0 <- bbbb (lo)
    FETCH(a1, 2)                           #  a1 <- BBBB (hi)
    GET_OPA(a3)                            #  a3 <- AA
    sll       t0, a1, 16
    or        a0, a0, t0                   #  a0 <- BBBBbbbb
    GET_VREG(a1, a3)                       #  a1 <- vAA
    EAS1(a0, rPC, a0)                      #  a0 <- PC + BBBBbbbb*2
    JAL($func)                             #  a0 <- code-unit branch offset
    move      rINST, v0
    EXPORT_PC()
    move      a0, rSELF
    addu      a1, rFP, OFF_FP_SHADOWFRAME
    move      a2, rINST
    JAL(MterpProfileBranch)                #  (self, shadow_frame, offset)
    bnez      v0, MterpOnStackReplacement  #  Note: offset must be in rINST
    addu      a1, rINST, rINST             #  a1 <- byte offset
    FETCH_ADVANCE_INST_RB(a1)              #  update rPC, load rINST
    bgtz      a1, .L${opcode}_finish
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
#else
    FETCH(a0, 1)                           #  a0 <- bbbb (lo)
    FETCH(a1, 2)                           #  a1 <- BBBB (hi)
    GET_OPA(a3)                            #  a3 <- AA
    sll       t0, a1, 16
    or        a0, a0, t0                   #  a0 <- BBBBbbbb
    GET_VREG(a1, a3)                       #  a1 <- vAA
    EAS1(a0, rPC, a0)                      #  a0 <- PC + BBBBbbbb*2
    JAL($func)                             #  a0 <- code-unit branch offset
    move      rINST, v0
    addu      a1, rINST, rINST             #  a1 <- byte offset
    FETCH_ADVANCE_INST_RB(a1)              #  update rPC, load rINST
    bgtz      a1, 1f
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
1:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
#endif

%break

.L${opcode}_finish:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
