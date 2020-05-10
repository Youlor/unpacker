%default {"preinstr":"", "result":"w0", "chkzero":"0"}
    /*
     * Generic 32-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the ARM math lib
     * handles it correctly.
     *
     * For: add-int, sub-int, mul-int, div-int, rem-int, and-int, or-int,
     *      xor-int, shl-int, shr-int, ushr-int, add-float, sub-float,
     *      mul-float, div-float, rem-float
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w9, wINST, #8               // w9<- AA
    lsr     w3, w0, #8                  // w3<- CC
    and     w2, w0, #255                // w2<- BB
    GET_VREG w1, w3                     // w1<- vCC
    GET_VREG w0, w2                     // w0<- vBB
    .if $chkzero
    cbz     w1, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $preinstr                           // optional op; may set condition codes
    $instr                              // $result<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG $result, w9                // vAA<- $result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */
