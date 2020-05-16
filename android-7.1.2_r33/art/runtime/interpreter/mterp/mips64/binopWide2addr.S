%default {"preinstr":"", "result":"a0", "chkzero":"0"}
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = a0 op a1".
     * This could be a MIPS instruction or a function call.  (If the result
     * comes back in a register other than a0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vB (a1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (LONG_MIN / -1) here, because the CPU handles it
     * correctly.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      rem-long/2addr, and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr
     */
    /* binop/2addr vA, vB */
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG_WIDE a0, a2                # a0 <- vA
    GET_VREG_WIDE a1, a3                # a1 <- vB
    .if $chkzero
    beqz    a1, common_errDivideByZero  # is second operand zero?
    .endif
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    $preinstr                           # optional op
    $instr                              # $result <- op, a0-a3 changed
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE $result, a2           # vA <- $result
    GOTO_OPCODE v0                      # jump to next instruction
