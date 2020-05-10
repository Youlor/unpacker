%include "mips64/fcvtHeader.S" { "suffix":"_DOUBLE", "valreg":"f0" }
    /*
     * TODO: simplify this when the MIPS64R6 emulator
     * supports NAN2008=1.
     */
    dli     t0, LONG_MIN_AS_DOUBLE
    dmtc1   t0, f1
    cmp.le.d f1, f1, f0
    bc1nez  f1, .L${opcode}_trunc
    cmp.eq.d f1, f0, f0
    dli     t0, LONG_MIN
    mfc1    t1, f1
    and     t0, t0, t1
    b       .L${opcode}_done
%break
.L${opcode}_trunc:
    trunc.l.d f0, f0
    dmfc1   t0, f0
.L${opcode}_done:
    /* Can't include fcvtFooter.S after break */
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE t0, a1
    GOTO_OPCODE v0                      # jump to next instruction
