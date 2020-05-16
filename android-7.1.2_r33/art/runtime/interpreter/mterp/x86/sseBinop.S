%default {"instr":"","suff":""}
    movzbl  2(rPC), %ecx                    # ecx <- BB
    movzbl  3(rPC), %eax                    # eax <- CC
    movs${suff}   VREG_ADDRESS(%ecx), %xmm0  # %xmm0 <- 1st src
    ${instr}${suff} VREG_ADDRESS(%eax), %xmm0
    movs${suff}   %xmm0, VREG_ADDRESS(rINST) # vAA <- %xmm0
    pxor    %xmm0, %xmm0
    movs${suff}   %xmm0, VREG_REF_ADDRESS(rINST) # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
