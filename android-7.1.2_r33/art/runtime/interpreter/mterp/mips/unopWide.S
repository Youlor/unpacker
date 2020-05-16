%default {"preinstr":"", "result0":"a0", "result1":"a1"}
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op a0/a1".
     * This could be MIPS instruction or a function call.
     *
     * For: neg-long, not-long, neg-double,
     */
    /* unop vA, vB */
    GET_OPA4(rOBJ)                         #  rOBJ <- A+
    GET_OPB(a3)                            #  a3 <- B
    EAS2(a3, rFP, a3)                      #  a3 <- &fp[B]
    LOAD64(a0, a1, a3)                     #  a0/a1 <- vAA
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    $preinstr                              #  optional op
    $instr                                 #  a0/a1 <- op, a2-a3 changed
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    SET_VREG64($result0, $result1, rOBJ)   #  vAA <- a0/a1
    GOTO_OPCODE(t0)                        #  jump to next instruction
    /* 12-13 instructions */
