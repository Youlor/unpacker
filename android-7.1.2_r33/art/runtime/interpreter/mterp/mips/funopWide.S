%default {"preinstr":"", "ld_arg":"LOAD64_F(fa0, fa0f, a3)", "st_result":"SET_VREG64_F(fv0, fv0f, rOBJ)"}
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op a0/a1".
     * This could be a MIPS instruction or a function call.
     *
     * long-to-double, double-to-long
     */
    /* unop vA, vB */
    GET_OPA4(rOBJ)                         #  t1 <- A+
    GET_OPB(a3)                            #  a3 <- B
    EAS2(a3, rFP, a3)                      #  a3 <- &fp[B]
    $ld_arg
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    $preinstr                              #  optional op
    $instr                                 #  a0/a1 <- op, a2-a3 changed

.L${opcode}_set_vreg:
    $st_result                             #  vAA <- a0/a1
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
    /* 12-13 instructions */
