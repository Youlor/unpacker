/*
 * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
 * 32-bit shift distance.
 */
    /* shl-long/2addr vA, vB */
    /* ecx gets shift count */
    /* Need to spill rIBASE */
    /* rINSTw gets AA */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    andb    $$0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, rINST                    # eax <- v[AA+0]
    sarl    $$4, %ecx                       # ecx <- B
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, rINST             # rIBASE <- v[AA+1]
    GET_VREG %ecx, %ecx                     # ecx <- vBB
    shrdl   rIBASE, %eax
    sarl    %cl, rIBASE
    testb   $$32, %cl
    je      2f
    movl    rIBASE, %eax
    sarl    $$31, rIBASE
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
