%default { "is_object":"0" }
    /* for: move-result, move-result-object */
    /* op vAA */
    GET_OPA(a2)                            #  a2 <- AA
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    lw    a0, OFF_FP_RESULT_REGISTER(rFP)  #  get pointer to result JType
    lw    a0, 0(a0)                        #  a0 <- result.i
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT(a0, a2)                #  fp[AA] <- a0
    .else
    SET_VREG(a0, a2)                       #  fp[AA] <- a0
    .endif
    GOTO_OPCODE(t0)                        #  jump to next instruction
