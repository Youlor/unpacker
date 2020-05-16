%default {}
    /*
     * Specialized 32-bit binary operation
     *
     * Performs "r1 = r0 rem r1". The selection between sdiv block or the gcc helper
     * depends on the compile time value of __ARM_ARCH_EXT_IDIV__ (defined for
     * ARMv7 CPUs that have hardware division support).
     *
     * NOTE: idivmod returns quotient in r0 and remainder in r1
     *
     * rem-int/lit8
     *
     */
    FETCH_S r3, 1                       @ r3<- ssssCCBB (sign-extended for CC)
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r3, #255                @ r2<- BB
    GET_VREG r0, r2                     @ r0<- vBB
    movs    r1, r3, asr #8              @ r1<- ssssssCC (sign extended)
    @cmp     r1, #0                     @ is second operand zero?
    beq     common_errDivideByZero
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST

#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r2, r0, r1
    mls     r1, r1, r2, r0              @ r1<- op
#else
    bl       __aeabi_idivmod            @ r1<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r1, r9                     @ vAA<- r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 10-12 instructions */
