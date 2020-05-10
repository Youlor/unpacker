    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6, v7" or "move v7, v6" */
    FETCH(a3, 1)                           #  a3 <- BBBB
    GET_OPA(a2)                            #  a2 <- AA
    EAS2(a3, rFP, a3)                      #  a3 <- &fp[BBBB]
    LOAD64(a0, a1, a3)                     #  a0/a1 <- fp[BBBB]
    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    SET_VREG64(a0, a1, a2)                 #  fp[AA] <- a0/a1
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
