    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op a0".
     * This could be a MIPS instruction or a function call.
     *
     * for: int-to-float, float-to-int
     */
    /* unop vA, vB */
    GET_OPB(a3)                            #  a3 <- B
    GET_OPA4(rOBJ)                         #  t0 <- A+
    GET_VREG_F(fa0, a3)
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    $instr

.L${opcode}_set_vreg_f:
    SET_VREG_F(fv0, rOBJ)
    GET_INST_OPCODE(t1)                    #  extract opcode from rINST
    GOTO_OPCODE(t1)                        #  jump to next instruction
