    /* rem_float/2addr vA, vB */
    movzbq  rINSTbl, %rcx                   # ecx <- A+
    sarl    $$4, rINST                      # rINST <- B
    flds    VREG_ADDRESS(rINSTq)            # vB to fp stack
    andb    $$0xf, %cl                      # ecx <- A
    flds    VREG_ADDRESS(%rcx)              # vA to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(%rcx)              # %st to vA
    CLEAR_REF %rcx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
