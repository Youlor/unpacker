%default {"fp_suffix":"","i_suffix":"","max_const":"","result_reg":"","wide":""}
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.
 */
    /* float/double to int/long vA, vB */
    movl    rINST, %ecx                     # rcx <- A+
    sarl    $$4, rINST                      # rINST <- B
    andb    $$0xf, %cl                      # ecx <- A
    movs${fp_suffix}   VREG_ADDRESS(rINSTq), %xmm0
    mov${i_suffix}  ${max_const}, ${result_reg}
    cvtsi2s${fp_suffix}${i_suffix} ${result_reg}, %xmm1
    comis${fp_suffix}    %xmm1, %xmm0
    jae     1f
    jp      2f
    cvtts${fp_suffix}2si${i_suffix}  %xmm0, ${result_reg}
    jmp     1f
2:
    xor${i_suffix}    ${result_reg}, ${result_reg}
1:
    .if $wide
    SET_WIDE_VREG ${result_reg}, %rcx
    .else
    SET_VREG ${result_reg}, %rcx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
