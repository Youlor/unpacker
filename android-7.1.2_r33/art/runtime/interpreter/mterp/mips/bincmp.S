    /*
     * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
     * fragment that specifies the *reverse* comparison to perform, e.g.
     * for "if-le" you would use "gt".
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    GET_OPA4(a0)                           #  a0 <- A+
    GET_OPB(a1)                            #  a1 <- B
    GET_VREG(a3, a1)                       #  a3 <- vB
    GET_VREG(a2, a0)                       #  a2 <- vA
    b${revcmp} a2, a3, 1f                  #  branch to 1 if comparison failed
    FETCH_S(rINST, 1)                      #  rINST<- branch offset, in code units
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
    addu      a2, rINST, rINST             #  convert to bytes
    FETCH_ADVANCE_INST_RB(a2)              #  update rPC, load rINST
    bgez      a2, .L_${opcode}_finish
    lw        ra, THREAD_FLAGS_OFFSET(rSELF)
    b         MterpCheckSuspendAndContinue

%break

.L_${opcode}_finish:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
