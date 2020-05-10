    /* rem_float vAA, vBB, vCC */
    movzbl  3(rPC), %ecx                    # ecx <- BB
    movzbl  2(rPC), %eax                    # eax <- CC
    flds    VREG_ADDRESS(%ecx)              # vBB to fp stack
    flds    VREG_ADDRESS(%eax)              # vCC to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(rINST)             # %st to vAA
    CLEAR_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
