%default {"instr":"","suff":""}
    movzx   rINSTbl, %ecx                   # ecx <- A+
    andl    $$0xf, %ecx                     # ecx <- A
    movs${suff} VREG_ADDRESS(%ecx), %xmm0      # %xmm0 <- 1st src
    sarl    $$4, rINST                      # rINST<- B
    ${instr}${suff} VREG_ADDRESS(rINST), %xmm0
    movs${suff} %xmm0, VREG_ADDRESS(%ecx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movs${suff} %xmm0, VREG_REF_ADDRESS(rINST)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
