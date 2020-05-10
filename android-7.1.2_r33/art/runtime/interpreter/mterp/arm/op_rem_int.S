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
     * rem-int
     *
     */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    mov     r3, r0, lsr #8              @ r3<- CC
    and     r2, r0, #255                @ r2<- BB
    GET_VREG r1, r3                     @ r1<- vCC
    GET_VREG r0, r2                     @ r0<- vBB
    cmp     r1, #0                      @ is second operand zero?
    beq     common_errDivideByZero

    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
#ifdef __ARM_ARCH_EXT_IDIV__
    sdiv    r2, r0, r1
    mls  r1, r1, r2, r0                 @ r1<- op, r0-r2 changed
#else
    bl   __aeabi_idivmod                @ r1<- op, r0-r3 changed
#endif
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    SET_VREG r1, r9                     @ vAA<- r1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 11-14 instructions */
