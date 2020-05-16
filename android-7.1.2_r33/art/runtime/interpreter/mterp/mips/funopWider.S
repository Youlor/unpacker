%default {"st_result":"SET_VREG64_F(fv0, fv0f, rOBJ)"}
    /*
     * Generic 32bit-to-64bit unary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = op a0", where
     * "result" is a 64-bit quantity in a0/a1.
     *
     * For: int-to-double, float-to-long, float-to-double
     */
    /* unop vA, vB */
    GET_OPA4(rOBJ)                         #  rOBJ <- A+
    GET_OPB(a3)                            #  a3 <- B
    GET_VREG_F(fa0, a3)
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    $instr

.L${opcode}_set_vreg:
    $st_result                             #  vA/vA+1 <- a0/a1
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
