    /*
     * Stores a specified register containing the result of conversion
     * from or to a floating-point type and jumps to the next instruction.
     *
     * Expects a1 to contain the destination Dalvik register number.
     * a1 is set up by fcvtHeader.S.
     *
     * For: int-to-float, int-to-double, long-to-float, long-to-double,
     *      float-to-int, float-to-long, float-to-double, double-to-int,
     *      double-to-long, double-to-float, neg-float, neg-double.
     *
     * Note that this file can't be included after a break in other files
     * and in those files its contents appear as a copy.
     * See: float-to-int, float-to-long, double-to-int, double-to-long.
     */
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG$suffix $valreg, a1
    GOTO_OPCODE v0                      # jump to next instruction
