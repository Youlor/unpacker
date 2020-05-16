%default {"instr":"","suff":""}
    movl    rINST, %ecx                     # ecx <- A+
    andl    $$0xf, %ecx                     # ecx <- A
    movs${suff} VREG_ADDRESS(%rcx), %xmm0        # %xmm0 <- 1st src
    sarl    $$4, rINST                      # rINST<- B
    ${instr}${suff} VREG_ADDRESS(rINSTq), %xmm0
    movs${suff} %xmm0, VREG_ADDRESS(%rcx)   # vAA<- %xmm0
    pxor    %xmm0, %xmm0
    movs${suff} %xmm0, VREG_REF_ADDRESS(rINSTq)  # clear ref
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
