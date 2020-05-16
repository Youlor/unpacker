%include "mips/funopWider.S" {"instr":"b f2l_doconv", "st_result":"SET_VREG64(rRESULT0, rRESULT1, rOBJ)"}
%break

f2l_doconv:
#ifdef MIPS32REVGE6
    l.s       fa1, .LLONG_TO_max
    cmp.ule.s ft2, fa1, fa0
    li        rRESULT0, ~0
    li        rRESULT1, ~0x80000000
    bc1nez    ft2, .L${opcode}_set_vreg

    l.s       fa1, .LLONG_TO_min
    cmp.ule.s ft2, fa0, fa1
    li        rRESULT0, 0
    li        rRESULT1, 0x80000000
    bc1nez    ft2, .L${opcode}_set_vreg

    mov.s     fa1, fa0
    cmp.un.s  ft2, fa0, fa1
    li        rRESULT0, 0
    li        rRESULT1, 0
    bc1nez    ft2, .L${opcode}_set_vreg
#else
    l.s       fa1, .LLONG_TO_max
    c.ole.s   fcc0, fa1, fa0
    li        rRESULT0, ~0
    li        rRESULT1, ~0x80000000
    bc1t      .L${opcode}_set_vreg

    l.s       fa1, .LLONG_TO_min
    c.ole.s   fcc0, fa0, fa1
    li        rRESULT0, 0
    li        rRESULT1, 0x80000000
    bc1t      .L${opcode}_set_vreg

    mov.s     fa1, fa0
    c.un.s    fcc0, fa0, fa1
    li        rRESULT0, 0
    li        rRESULT1, 0
    bc1t      .L${opcode}_set_vreg
#endif

    JAL(__fixsfdi)

    b         .L${opcode}_set_vreg

.LLONG_TO_max:
    .word 0x5f000000

.LLONG_TO_min:
    .word 0xdf000000
