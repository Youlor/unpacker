    /*
     * Generic 32-bit binary float operation.
     *
     * For: add-fp, sub-fp, mul-fp, div-fp, rem-fp
     */

    /* binop vAA, vBB, vCC */
    FETCH(a0, 1)                           #  a0 <- CCBB
    GET_OPA(rOBJ)                          #  s5 <- AA
    srl       a3, a0, 8                    #  a3 <- CC
    and       a2, a0, 255                  #  a2 <- BB
    GET_VREG_F(fa1, a3)                    #  a1 <- vCC
    GET_VREG_F(fa0, a2)                    #  a0 <- vBB

    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    $instr                                 #  f0 = result
    SET_VREG_F(fv0, rOBJ)                  #  vAA <- fv0
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
