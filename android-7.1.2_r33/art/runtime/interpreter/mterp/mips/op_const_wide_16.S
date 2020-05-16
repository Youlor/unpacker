    # const-wide/16 vAA,                   /* +BBBB */
    FETCH_S(a0, 1)                         #  a0 <- ssssBBBB (sign-extended)
    GET_OPA(a3)                            #  a3 <- AA
    sra       a1, a0, 31                   #  a1 <- ssssssss
    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    SET_VREG64(a0, a1, a3)                 #  vAA <- a0/a1
    GOTO_OPCODE(t0)                        #  jump to next instruction
