    /* int to long vA, vB */
    movzbq  rINSTbl, %rax                   # rax <- +A
    sarl    $$4, %eax                       # eax <- B
    andb    $$0xf, rINSTbl                  # rINST <- A
    movslq  VREG_ADDRESS(%rax), %rax
    SET_WIDE_VREG %rax, rINSTq              # v[A] <- %rax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

