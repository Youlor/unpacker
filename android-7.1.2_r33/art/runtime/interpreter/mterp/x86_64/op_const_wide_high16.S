    /* const-wide/high16 vAA, #+BBBB000000000000 */
    movzwq  2(rPC), %rax                    # eax <- 0000BBBB
    salq    $$48, %rax                      # eax <- BBBB0000
    SET_WIDE_VREG %rax, rINSTq              # v[AA+0] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
