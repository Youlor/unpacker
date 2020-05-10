    /* const/4 vA, #+B */
    movsbl  rINSTbl, %eax                   # eax <-ssssssBx
    movl    $$0xf, rINST
    andl    %eax, rINST                     # rINST <- A
    sarl    $$4, %eax
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
