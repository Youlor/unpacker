%default {"instr":"","load":"","store":"","wide":"0"}
/*
 * Generic 32-bit FP conversion operation.
 */
    /* unop vA, vB */
    movzbl  rINSTbl, %ecx                   # ecx <- A+
    sarl    $$4, rINST                      # rINST <- B
    $load   VREG_ADDRESS(rINST)             # %st0 <- vB
    andb    $$0xf, %cl                      # ecx <- A
    $instr
    $store  VREG_ADDRESS(%ecx)              # vA <- %st0
    .if $wide
    CLEAR_WIDE_REF %ecx
    .else
    CLEAR_REF %ecx
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
