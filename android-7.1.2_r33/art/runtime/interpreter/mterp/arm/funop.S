    /*
     * Generic 32-bit unary floating-point operation.  Provide an "instr"
     * line that specifies an instruction that performs "s1 = op s0".
     *
     * for: int-to-float, float-to-int
     */
    /* unop vA, vB */
    mov     r3, rINST, lsr #12          @ r3<- B
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &vB
    flds    s0, [r3]                    @ s0<- vB
    ubfx    r9, rINST, #8, #4           @ r9<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    $instr                              @ s1<- op
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &vA
    fsts    s1, [r9]                    @ vA<- s1
    GOTO_OPCODE ip                      @ jump to next instruction
