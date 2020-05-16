%default {"preinstr":"", "result":"w0", "chkzero":"0"}
    /*
     * Generic 32-bit "lit16" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit16, rsub-int, mul-int/lit16, div-int/lit16,
     *      rem-int/lit16, and-int/lit16, or-int/lit16, xor-int/lit16
     */
    /* binop/lit16 vA, vB, #+CCCC */
    FETCH_S w1, 1                       // w1<- ssssCCCC (sign-extended)
    lsr     w2, wINST, #12              // w2<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w0, w2                     // w0<- vB
    .if $chkzero
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $preinstr
    $instr                              // $result<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG $result, w9                // vAA<- $result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */
