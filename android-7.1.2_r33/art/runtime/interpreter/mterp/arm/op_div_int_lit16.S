%default {}
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r0 = r0 div r1". The selection between sdiv or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * div-int/lit16
     *
     */
    FETCH_S r1, 1                       @ r1<- ssssCCCC (sign-extended)
    mov     r2, rINST, lsr #12          @ r2<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    GET_VREG r0, r2                     @ r0<- vB
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r0, r0, r1                  @ r0<- op
#else
    bl       __aeabi_idiv               @ r0<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r0, r9                     @ vAA<- r0
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-13 instructions */
