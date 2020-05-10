    /*
     * Generic 32-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "s2 = s0 op s1".
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    flds    s1, [r3]                    @ s1<- vB
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    flds    s0, [r9]                    @ s0<- vA
    $instr                              @ s2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fsts    s2, [r9]                    @ vAA<- s2
    GOTO_OPCODE ip                      @ jump to next instruction
