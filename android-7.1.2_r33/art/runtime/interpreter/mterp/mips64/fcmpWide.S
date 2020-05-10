%default {}
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     *
     * For: cmpl-double, cmpg-double
     */
    /* op vAA, vBB, vCC */
    srl     a4, rINST, 8                # a4 <- AA
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    GET_VREG_DOUBLE f0, a2              # f0 <- vBB
    GET_VREG_DOUBLE f1, a3              # f1 <- vCC
    cmp.eq.d f2, f0, f1
    li      a0, 0
    bc1nez  f2, 1f                      # done if vBB == vCC (ordered)
    .if $gt_bias
    cmp.lt.d f2, f0, f1
    li      a0, -1
    bc1nez  f2, 1f                      # done if vBB < vCC (ordered)
    li      a0, 1                       # vBB > vCC or unordered
    .else
    cmp.lt.d f2, f1, f0
    li      a0, 1
    bc1nez  f2, 1f                      # done if vBB > vCC (ordered)
    li      a0, -1                      # vBB < vCC or unordered
    .endif
1:
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG a0, a4                     # vAA <- a0
    GOTO_OPCODE v0                      # jump to next instruction
