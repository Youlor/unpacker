%default {"srcdouble":"1","tgtlong":"1"}
/* On fp to int conversions, Java requires that
 * if the result > maxint, it should be clamped to maxint.  If it is less
 * than minint, it should be clamped to minint.  If it is a nan, the result
 * should be zero.  Further, the rounding mode is to truncate.  This model
 * differs from what is delivered normally via the x86 fpu, so we have
 * to play some games.
 */
    /* float/double to int/long vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $$4, rINST                      # rINST <- B
    .if $srcdouble
    fldl    VREG_ADDRESS(rINST)             # %st0 <- vB
    .else
    flds    VREG_ADDRESS(rINST)             # %st0 <- vB
    .endif
    ftst
    fnstcw  LOCAL0(%esp)                    # remember original rounding mode
    movzwl  LOCAL0(%esp), %eax
    movb    $$0xc, %ah
    movw    %ax, LOCAL0+2(%esp)
    fldcw   LOCAL0+2(%esp)                  # set "to zero" rounding mode
    andb    $$0xf, %cl                      # ecx <- A
    .if $tgtlong
    fistpll VREG_ADDRESS(%ecx)              # convert and store
    .else
    fistpl  VREG_ADDRESS(%ecx)              # convert and store
    .endif
    fldcw   LOCAL0(%esp)                    # restore previous rounding mode
    .if $tgtlong
    movl    $$0x80000000, %eax
    xorl    VREG_HIGH_ADDRESS(%ecx), %eax
    orl     VREG_ADDRESS(%ecx), %eax
    .else
    cmpl    $$0x80000000, VREG_ADDRESS(%ecx)
    .endif
    je      .L${opcode}_special_case # fix up result

.L${opcode}_finish:
    xor     %eax, %eax
    mov     %eax, VREG_REF_ADDRESS(%ecx)
    .if $tgtlong
    mov     %eax, VREG_REF_HIGH_ADDRESS(%ecx)
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1

.L${opcode}_special_case:
    fnstsw  %ax
    sahf
    jp      .L${opcode}_isNaN
    adcl    $$-1, VREG_ADDRESS(%ecx)
    .if $tgtlong
    adcl    $$-1, VREG_HIGH_ADDRESS(%ecx)
    .endif
   jmp      .L${opcode}_finish
.L${opcode}_isNaN:
    movl    $$0, VREG_ADDRESS(%ecx)
    .if $tgtlong
    movl    $$0, VREG_HIGH_ADDRESS(%ecx)
    .endif
    jmp     .L${opcode}_finish
