    /* const-wide/high16 vAA, #+BBBB000000000000 */
    movzwl  2(rPC), %eax                    # eax <- 0000BBBB
    sall    $$16, %eax                      # eax <- BBBB0000
    SET_VREG_HIGH %eax, rINST               # v[AA+1] <- eax
    xorl    %eax, %eax
    SET_VREG %eax, rINST                    # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
