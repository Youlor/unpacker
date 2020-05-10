    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- BA
    sarl    $$4, %ecx                       # ecx <- B
    andb    $$0xf, rINSTbl                  # rINST <- A
    GET_VREG %eax, %ecx                     # eax <- v[B+0]
    GET_VREG_HIGH %ecx, %ecx                # ecx <- v[B+1]
    notl    %eax
    notl    %ecx
    SET_VREG %eax, rINST                    # v[A+0] <- eax
    SET_VREG_HIGH %ecx, rINST               # v[A+1] <- ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
