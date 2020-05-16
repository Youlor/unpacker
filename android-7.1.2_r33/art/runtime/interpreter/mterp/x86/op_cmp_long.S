/*
 * Compare two 64-bit values.  Puts 0, 1, or -1 into the destination
 * register based on the results of the comparison.
 */
    /* cmp-long vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG_HIGH %eax, %eax                # eax <- v[BB+1], BB is clobbered
    cmpl    VREG_HIGH_ADDRESS(%ecx), %eax
    jl      .L${opcode}_smaller
    jg      .L${opcode}_bigger
    movzbl  2(rPC), %eax                    # eax <- BB, restore BB
    GET_VREG %eax, %eax                     # eax <- v[BB]
    sub     VREG_ADDRESS(%ecx), %eax
    ja      .L${opcode}_bigger
    jb      .L${opcode}_smaller
.L${opcode}_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

.L${opcode}_bigger:
    movl    $$1, %eax
    jmp     .L${opcode}_finish

.L${opcode}_smaller:
    movl    $$-1, %eax
    jmp     .L${opcode}_finish
