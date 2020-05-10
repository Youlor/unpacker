    /* rem_double vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx <- BB
    movzbq  2(rPC), %rax                    # eax <- CC
    fldl    VREG_ADDRESS(%rcx)              # %st1 <- fp[vBB]
    fldl    VREG_ADDRESS(%rax)              # %st0 <- fp[vCC]
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstpl   VREG_ADDRESS(rINSTq)            # fp[vAA] <- %st
    CLEAR_WIDE_REF rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
