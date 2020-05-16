%include "mips64/fcvtHeader.S" { "suffix":"_FLOAT", "valreg":"f0" }
    /*
     * TODO: simplify this when the MIPS64R6 emulator
     * supports NAN2008=1.
     */
    li      t0, INT_MIN_AS_FLOAT
    mtc1    t0, f1
    cmp.le.s f1, f1, f0
    bc1nez  f1, .L${opcode}_trunc
    cmp.eq.s f1, f0, f0
    li      t0, INT_MIN
    mfc1    t1, f1
    and     t0, t0, t1
    b       .L${opcode}_done
%break
.L${opcode}_trunc:
    trunc.w.s f0, f0
    mfc1    t0, f0
.L${opcode}_done:
    /* Can't include fcvtFooter.S after break */
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG t0, a1
    GOTO_OPCODE v0                      # jump to next instruction
