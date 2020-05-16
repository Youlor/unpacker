    /* rem_double/2addr vA, vB */
    movzx   rINSTbl, %ecx                   # ecx <- A+
    sarl    $$4, rINST                      # rINST <- B
    fldl    VREG_ADDRESS(rINST)             # vB to fp stack
    andb    $$0xf, %cl                      # ecx <- A
    fldl    VREG_ADDRESS(%ecx)              # vA to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(%ecx)              # %st to vA
    CLEAR_WIDE_REF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
