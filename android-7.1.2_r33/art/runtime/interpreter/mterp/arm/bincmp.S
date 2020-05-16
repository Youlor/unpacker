    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r0, rINST, #8, #4           @ r0<- A
    GET_VREG r3, r1                     @ r3<- vB
    GET_VREG r0, r0                     @ r0<- vA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, r3                      @ compare (vA, vB)
    b${condition} MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
