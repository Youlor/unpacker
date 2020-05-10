    /*
     * Loads a specified register from vB. Used primarily for conversions
     * from or to a floating-point type.
     *
     * Sets up a1 = A and a2 = B. a2 is later used by fcvtFooter.S to
     * store the result in vA and jump to the next instruction.
     *
     * For: int-to-float, int-to-double, long-to-float, long-to-double,
     *      float-to-int, float-to-long, float-to-double, double-to-int,
     *      double-to-long, double-to-float, neg-float, neg-double.
     */
    ext     a1, rINST, 8, 4             # a1 <- A
    srl     a2, rINST, 12               # a2 <- B
    GET_VREG$suffix $valreg, a2
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
