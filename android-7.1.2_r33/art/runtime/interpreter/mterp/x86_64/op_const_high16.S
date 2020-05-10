    /* const/high16 vAA, #+BBBB0000 */
    movzwl  2(rPC), %eax                    # eax <- 0000BBBB
    sall    $$16, %eax                      # eax <- BBBB0000
    SET_VREG %eax, rINSTq                   # vAA <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
