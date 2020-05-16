/*
 * Generic two-operand compare-and-branch operation.  Provide a "revcmp"
 * fragment that specifies the *reverse* comparison to perform, e.g.
 * for "if-le" you would use "gt".
 *
 * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
 */
    /* if-cmp vA, vB, +CCCC */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andb    $$0xf, %cl                      # ecx <- A
    GET_VREG %eax, %ecx                     # eax <- vA
    sarl    $$4, rINST                      # rINST <- B
    cmpl    VREG_ADDRESS(rINST), %eax       # compare (vA, vB)
    j${revcmp}   1f
    movswl  2(rPC), rINST                   # Get signed branch offset
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
1:
    cmpw    $$JIT_CHECK_OSR, rPROFILE
    je      .L_check_not_taken_osr
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
