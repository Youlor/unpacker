%default {"load":"LOAD64_F(fa0, fa0f, a3)"}
    /*
     * Generic 64bit-to-32bit unary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = op a0/a1", where
     * "result" is a 32-bit quantity in a0.
     *
     * For: long-to-float, double-to-int, double-to-float
     * If hard floating point support is available, use fa0 as the parameter,
     * except for long-to-float opcode.
     * (This would work for long-to-int, but that instruction is actually
     * an exact match for OP_MOVE.)
     */
    /* unop vA, vB */
    GET_OPB(a3)                            #  a3 <- B
    GET_OPA4(rOBJ)                         #  t1 <- A+
    EAS2(a3, rFP, a3)                      #  a3 <- &fp[B]
    $load
    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    $instr

.L${opcode}_set_vreg_f:
    SET_VREG_F(fv0, rOBJ)                  #  vA <- result0
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
