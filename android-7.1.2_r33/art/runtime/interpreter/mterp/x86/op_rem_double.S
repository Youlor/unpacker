    /* rem_double vAA, vBB, vCC */
    movzbl  3(rPC), %ecx                    # ecx <- BB
    movzbl  2(rPC), %eax                    # eax <- CC
    fldl    VREG_ADDRESS(%ecx)              # %st1 <- fp[vBB]
    fldl    VREG_ADDRESS(%eax)              # %st0 <- fp[vCC]
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(rINST)             # fp[vAA] <- %st
    CLEAR_WIDE_REF rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
