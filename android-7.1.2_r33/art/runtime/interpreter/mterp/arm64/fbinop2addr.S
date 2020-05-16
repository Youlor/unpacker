    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    lsr     w9, wINST, #8               // w9<- A+
    and     w9, w9, #15                 // w9<- A
    GET_VREG s1, w3
    GET_VREG s0, w9
    $instr                              // s2<- op
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG s2, w9
    GOTO_OPCODE ip                      // jump to next instruction
