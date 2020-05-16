    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = a0-a1 op a2-a3".
     * This could be an MIPS instruction or a function call.
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr,
     *  div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    GET_OPA4(rOBJ)                         #  rOBJ <- A+
    GET_OPB(a1)                            #  a1 <- B
    EAS2(a1, rFP, a1)                      #  a1 <- &fp[B]
    EAS2(t0, rFP, rOBJ)                    #  t0 <- &fp[A]
    LOAD64_F(fa0, fa0f, t0)
    LOAD64_F(fa1, fa1f, a1)

    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    $instr
    SET_VREG64_F(fv0, fv0f, rOBJ)
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
