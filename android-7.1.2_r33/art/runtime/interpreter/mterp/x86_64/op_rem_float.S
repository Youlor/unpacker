    /* rem_float vAA, vBB, vCC */
    movzbq  3(rPC), %rcx                    # ecx <- BB
    movzbq  2(rPC), %rax                    # eax <- CC
    flds    VREG_ADDRESS(%rcx)              # vBB to fp stack
    flds    VREG_ADDRESS(%rax)              # vCC to fp stack
1:
    fprem
    fstsw   %ax
    sahf
    jp      1b
    fstp    %st(1)
    fstps   VREG_ADDRESS(rINSTq)            # %st to vAA
    CLEAR_REF rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
