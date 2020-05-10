    /*
     * Generic one-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    GET_OPA(a0)                            #  a0 <- AA
    GET_VREG(a2, a0)                       #  a2 <- vAA
    FETCH_S(rINST, 1)                      #  rINST <- branch offset, in code units
    b${revcmp} a2, zero, 1f                #  branch to 1 if comparison failed
    b 2f
1:
    li        rINST, 2                     #  rINST- BYTE branch dist for not-taken
2:
#if MTERP_PROFILE_BRANCHES
    EXPORT_PC()
    move      a0, rSELF
    addu      a1, rFP, OFF_FP_SHADOWFRAME
    move      a2, rINST
    JAL(MterpProfileBranch)                #  (self, shadow_frame, offset)
    bnez      v0, MterpOnStackReplacement  #  Note: offset must be in rINST
#endif
    addu      a1, rINST, rINST             #  convert to bytes
    FETCH_ADVANCE_INST_RB(a1)              #  update rPC, load rINST
    bgez      a1, 3f
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue
3:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
