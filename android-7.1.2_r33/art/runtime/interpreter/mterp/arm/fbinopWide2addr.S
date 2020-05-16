    /*
     * Generic 64-bit floating point "/2addr" binary operation.  Provide
     * an "instr" line that specifies an instruction that performs
     * "d2 = d0 op d1".
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr,
     *      div-double/2addr
     */
    /* binop/2addr vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    CLEAR_SHADOW_PAIR r9, ip, r0        @ Zero out shadow regs
    fldd    d1, [r3]                    @ d1<- vB
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    fldd    d0, [r9]                    @ d0<- vA
    $instr                              @ d2<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    fstd    d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction
