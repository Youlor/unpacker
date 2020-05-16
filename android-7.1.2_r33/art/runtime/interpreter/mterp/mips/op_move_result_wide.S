    /* move-result-wide vAA */
    GET_OPA(a2)                            #  a2 <- AA
    lw    a3, OFF_FP_RESULT_REGISTER(rFP)  #  get pointer to result JType
    LOAD64(a0, a1, a3)                     #  a0/a1 <- retval.j
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    SET_VREG64(a0, a1, a2)                 #  fp[AA] <- a0/a1
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
