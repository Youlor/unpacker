%default {"preinstr":""}
    /*
     * Generic 32-bit unary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = op w0".
     * This could be an ARM instruction or a function call.
     *
     * for: neg-int, not-int, neg-float, int-to-float, float-to-int,
     *      int-to-byte, int-to-char, int-to-short
     */
    /* unop vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    GET_VREG w0, w3                     // w0<- vB
    ubfx    w9, wINST, #8, #4           // w9<- A
    $preinstr                           // optional op; may set condition codes
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    $instr                              // w0<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w9                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
    /* 8-9 instructions */
