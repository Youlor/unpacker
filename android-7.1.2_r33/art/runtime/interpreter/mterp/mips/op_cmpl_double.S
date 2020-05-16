%default { "naninst":"li rTEMP, -1" }
    /*
     * Compare two floating-point values. Puts 0(==), 1(>), or -1(<)
     * into the destination register (rTEMP) based on the comparison results.
     *
     * Provide a "naninst" instruction that puts 1 or -1 into rTEMP depending
     * on what value we'd like to return when one of the operands is NaN.
     *
     * See op_cmpl_float for more details.
     *
     * For: cmpl-double, cmpg-double
     */
    /* op vAA, vBB, vCC */

    FETCH(a0, 1)                           #  a0 <- CCBB
    and       rOBJ, a0, 255                #  s5 <- BB
    srl       t0, a0, 8                    #  t0 <- CC
    EAS2(rOBJ, rFP, rOBJ)                  #  s5 <- &fp[BB]
    EAS2(t0, rFP, t0)                      #  t0 <- &fp[CC]
    LOAD64_F(ft0, ft0f, rOBJ)
    LOAD64_F(ft1, ft1f, t0)
#ifdef MIPS32REVGE6
    cmp.ult.d ft2, ft0, ft1
    li        rTEMP, -1
    bc1nez    ft2, .L${opcode}_finish
    cmp.ult.d ft2, ft1, ft0
    li        rTEMP, 1
    bc1nez    ft2, .L${opcode}_finish
    cmp.eq.d  ft2, ft0, ft1
    li        rTEMP, 0
    bc1nez    ft2, .L${opcode}_finish
    b         .L${opcode}_nan
#else
    c.olt.d   fcc0, ft0, ft1
    li        rTEMP, -1
    bc1t      fcc0, .L${opcode}_finish
    c.olt.d   fcc0, ft1, ft0
    li        rTEMP, 1
    bc1t      fcc0, .L${opcode}_finish
    c.eq.d    fcc0, ft0, ft1
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
