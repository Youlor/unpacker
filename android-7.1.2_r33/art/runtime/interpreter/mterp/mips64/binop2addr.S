%default {"preinstr":"", "result":"a0", "chkzero":"0"}
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = a0 op a1".
     * This could be a MIPS instruction or a function call.  (If the result
     * comes back in a register other than a0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vB (a1).  Useful for integer division and modulus.  Note that we
     * *don't* check for (INT_MIN / -1) here, because the CPU handles it
     * correctly.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr
     */
    /* binop/2addr vA, vB */
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG a0, a2                     # a0 <- vA
    GET_VREG a1, a3                     # a1 <- vB
    .if $chkzero
    beqz    a1, common_errDivideByZero  # is second operand zero?
    .endif
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    $preinstr                           # optional op
    $instr                              # $result <- op, a0-a3 changed
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG $result, a2                # vA <- $result
    GOTO_OPCODE v0                      # jump to next instruction
