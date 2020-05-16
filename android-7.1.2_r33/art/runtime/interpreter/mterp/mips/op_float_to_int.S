%include "mips/funop.S" {"instr":"b f2i_doconv"}
%break

/*
 * Not an entry point as it is used only once !!
 */
f2i_doconv:
#ifdef MIPS32REVGE6
    l.s       fa1, .LFLOAT_TO_INT_max
    cmp.ule.s ft2, fa1, fa0
    l.s       fv0, .LFLOAT_TO_INT_ret_max
    bc1nez    ft2, .L${opcode}_set_vreg_f

    l.s       fa1, .LFLOAT_TO_INT_min
    cmp.ule.s ft2, fa0, fa1
    l.s       fv0, .LFLOAT_TO_INT_ret_min
    bc1nez    ft2, .L${opcode}_set_vreg_f

    mov.s     fa1, fa0
    cmp.un.s  ft2, fa0, fa1
    li.s      fv0, 0
    bc1nez    ft2, .L${opcode}_set_vreg_f
#else
    l.s       fa1, .LFLOAT_TO_INT_max
    c.ole.s   fcc0, fa1, fa0
    l.s       fv0, .LFLOAT_TO_INT_ret_max
    bc1t      .L${opcode}_set_vreg_f

    l.s       fa1, .LFLOAT_TO_INT_min
    c.ole.s   fcc0, fa0, fa1
    l.s       fv0, .LFLOAT_TO_INT_ret_min
    bc1t      .L${opcode}_set_vreg_f

    mov.s     fa1, fa0
    c.un.s    fcc0, fa0, fa1
    li.s      fv0, 0
    bc1t      .L${opcode}_set_vreg_f
#endif

    trunc.w.s  fv0, fa0
    b         .L${opcode}_set_vreg_f

.LFLOAT_TO_INT_max:
    .word 0x4f000000
.LFLOAT_TO_INT_min:
    .word 0xcf000000
.LFLOAT_TO_INT_ret_max:
    .word 0x7fffffff
.LFLOAT_TO_INT_ret_min:
    .word 0x80000000
