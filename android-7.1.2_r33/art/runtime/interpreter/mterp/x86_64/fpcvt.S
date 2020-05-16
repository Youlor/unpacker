%default {"source_suffix":"","dest_suffix":"","wide":""}
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    cvts${source_suffix}2s${dest_suffix}    VREG_ADDRESS(rINSTq), %xmm0
    .if $wide
    movsd   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_WIDE_REF %rcx
    .else
    movss   %xmm0, VREG_ADDRESS(%rcx)
    CLEAR_REF %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
