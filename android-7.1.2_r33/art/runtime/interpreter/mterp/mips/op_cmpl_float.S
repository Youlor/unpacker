%default { "naninst":"li rTEMP, -1" }
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register rTEMP based on the results of the comparison.
     *
     * Provide a "naninst" instruction that puts 1 or -1 into rTEMP depending
     * on what value we'd like to return when one of the operands is NaN.
     *
     * The operation we're implementing is:
     *   if (x == y)
     *     return 0;
     *   else if (x < y)
     *     return -1;
     *   else if (x > y)
     *     return 1;
     *   else
     *     return {-1 or 1};  // one or both operands was NaN
     *
     * for: cmpl-float, cmpg-float
     */
    /* op vAA, vBB, vCC */

    /* "clasic" form */
    FETCH(a0, 1)                           #  a0 <- CCBB
    and       a2, a0, 255                  #  a2 <- BB
    srl       a3, a0, 8
    GET_VREG_F(ft0, a2)
    GET_VREG_F(ft1, a3)
#ifdef MIPS32REVGE6
    cmp.ult.s ft2, ft0, ft1               # Is ft0 < ft1
    li        rTEMP, -1
    bc1nez    ft2, .L${opcode}_finish
    cmp.ult.s ft2, ft1, ft0
    li        rTEMP, 1
    bc1nez    ft2, .L${opcode}_finish
    cmp.eq.s  ft2, ft0, ft1
    li        rTEMP, 0
    bc1nez    ft2, .L${opcode}_finish
    b         .L${opcode}_nan
#else
    c.olt.s   fcc0, ft0, ft1               # Is ft0 < ft1
    li        rTEMP, -1
    bc1t      fcc0, .L${opcode}_finish
    c.olt.s   fcc0, ft1, ft0
    li        rTEMP, 1
    bc1t      fcc0, .L${opcode}_finish
    c.eq.s    fcc0, ft0, ft1
    li        rTEMP, 0
    bc1t      fcc0, .L${opcode}_finish
    b         .L${opcode}_nan
#endif
%break

.L${opcode}_nan:
    $naninst

.L${opcode}_finish:
    GET_OPA(rOBJ)
    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    SET_VREG_GOTO(rTEMP, rOBJ, t0)         #  vAA <- rTEMP
