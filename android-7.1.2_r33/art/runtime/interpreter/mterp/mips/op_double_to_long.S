%include "mips/funopWide.S" {"instr":"b d2l_doconv", "st_result":"SET_VREG64(rRESULT0, rRESULT1, rOBJ)"}
%break

d2l_doconv:
#ifdef MIPS32REVGE6
    la        t0, .LDOUBLE_TO_LONG_max
    LOAD64_F(fa1, fa1f, t0)
    cmp.ule.d ft2, fa1, fa0
    la        t0, .LDOUBLE_TO_LONG_ret_max
    LOAD64(rRESULT0, rRESULT1, t0)
    bc1nez    ft2, .L${opcode}_set_vreg

    la        t0, .LDOUBLE_TO_LONG_min
    LOAD64_F(fa1, fa1f, t0)
    cmp.ule.d ft2, fa0, fa1
    la        t0, .LDOUBLE_TO_LONG_ret_min
    LOAD64(rRESULT0, rRESULT1, t0)
    bc1nez    ft2, .L${opcode}_set_vreg

    mov.d     fa1, fa0
    cmp.un.d  ft2, fa0, fa1
    li        rRESULT0, 0
    li        rRESULT1, 0
    bc1nez    ft2, .L${opcode}_set_vreg
#else
    la        t0, .LDOUBLE_TO_LONG_max
    LOAD64_F(fa1, fa1f, t0)
    c.ole.d   fcc0, fa1, fa0
    la        t0, .LDOUBLE_TO_LONG_ret_max
    LOAD64(rRESULT0, rRESULT1, t0)
    bc1t      .L${opcode}_set_vreg

    la        t0, .LDOUBLE_TO_LONG_min
    LOAD64_F(fa1, fa1f, t0)
    c.ole.d   fcc0, fa0, fa1
    la        t0, .LDOUBLE_TO_LONG_ret_min
    LOAD64(rRESULT0, rRESULT1, t0)
    bc1t      .L${opcode}_set_vreg

    mov.d     fa1, fa0
    c.un.d    fcc0, fa0, fa1
    li        rRESULT0, 0
    li        rRESULT1, 0
    bc1t      .L${opcode}_set_vreg
#endif
    JAL(__fixdfdi)
    b         .L${opcode}_set_vreg

.LDOUBLE_TO_LONG_max:
    .dword 0x43e0000000000000              #  maxlong, as a double (high word)
.LDOUBLE_TO_LONG_min:
    .dword 0xc3e0000000000000              #  minlong, as a double (high word)
.LDOUBLE_TO_LONG_ret_max:
    .dword 0x7fffffffffffffff
.LDOUBLE_TO_LONG_ret_min:
    .dword 0x8000000000000000
