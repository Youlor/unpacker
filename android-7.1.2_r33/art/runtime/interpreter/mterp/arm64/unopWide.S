%default {"instr":"sub x0, xzr, x0", "preinstr":""}
    /*
     * Generic 64-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op x0".
     *
     * For: neg-long, not-long
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w4, wINST, #8, #4           // w4<- A
    GET_VREG_WIDE x0, w3
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    $preinstr
    $instr
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, w4
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-11 instructions */
