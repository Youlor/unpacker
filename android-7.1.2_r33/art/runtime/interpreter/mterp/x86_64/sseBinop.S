%default {"instr":"","suff":""}
    movzbq  2(rPC), %rcx                    # ecx <- BB
    movzbq  3(rPC), %rax                    # eax <- CC
    movs${suff}   VREG_ADDRESS(%rcx), %xmm0       # %xmm0 <- 1st src
    ${instr}${suff} VREG_ADDRESS(%rax), %xmm0
    movs${suff}   %xmm0, VREG_ADDRESS(rINSTq)     # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movs${suff}   %xmm0, VREG_REF_ADDRESS(rINSTq) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
