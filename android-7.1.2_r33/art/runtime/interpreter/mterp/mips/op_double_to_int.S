%include "mips/unopNarrower.S" {"instr":"b d2i_doconv"}
/*
 * Convert the double in a0/a1 to an int in a0.
 *
 * We have to clip values to int min/max per the specification.  The
 * expected common case is a "reasonable" value that converts directly
 * to modest integer.  The EABI convert function isn't doing this for us.
 */
%break

d2i_doconv:
#ifdef MIPS32REVGE6
    la        t0, .LDOUBLE_TO_INT_max
    LOAD64_F(fa1, fa1f, t0)
    cmp.ule.d ft2, fa1, fa0
    l.s       fv0, .LDOUBLE_TO_INT_maxret
    bc1nez    ft2, .L${opcode}_set_vreg_f

    la        t0, .LDOUBLE_TO_INT_min
    LOAD64_F(fa1, fa1f, t0)
    cmp.ule.d ft2, fa0, fa1
    l.s       fv0, .LDOUBLE_TO_INT_minret
    bc1nez    ft2, .L${opcode}_set_vreg_f

    mov.d     fa1, fa0
    cmp.un.d  ft2, fa0, fa1
    li.s      fv0, 0
    bc1nez    ft2, .L${opcode}_set_vreg_f
#else
    la        t0, .LDOUBLE_TO_INT_max
    LOAD64_F(fa1, fa1f, t0)
    c.ole.d   fcc0, fa1, fa0
    l.s       fv0, .LDOUBLE_TO_INT_maxret
    bc1t      .L${opcode}_set_vreg_f

    la        t0, .LDOUBLE_TO_INT_min
    LOAD64_F(fa1, fa1f, t0)
    c.ole.d   fcc0, fa0, fa1
    l.s       fv0, .LDOUBLE_TO_INT_minret
    bc1t      .L${opcode}_set_vreg_f

    mov.d     fa1, fa0
    c.un.d    fcc0, fa0, fa1
    li.s      fv0, 0
    bc1t      .L${opcode}_set_vreg_f
#endif

    trunc.w.d  fv0, fa0
    b         .L${opcode}_set_vreg_f

.LDOUBLE_TO_INT_max:
    .dword 0x41dfffffffc00000
.LDOUBLE_TO_INT_min:
    .dword 0xc1e0000000000000              #  minint, as a double (high word)
.LDOUBLE_TO_INT_maxret:
    .word 0x7fffffff
.LDOUBLE_TO_INT_minret:
    .word 0x80000000
