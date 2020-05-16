%default {"suff":"d","nanval":"pos"}
/*
 * Compare two floating-point values.  Puts 0, 1, or -1 into the
 * destination register based on the results of the comparison.
 *
 * int compare(x, y) {
 *     if (x == y) {
 *         return 0;
 *     } else if (x < y) {
 *         return -1;
 *     } else if (x > y) {
 *         return 1;
 *     } else {
 *         return nanval ? 1 : -1;
 *     }
 * }
 */
    /* op vAA, vBB, vCC */
    movzbl  3(rPC), %ecx                    # ecx<- CC
    movzbl  2(rPC), %eax                    # eax<- BB
    movs${suff} VREG_ADDRESS(%eax), %xmm0
    xor     %eax, %eax
    ucomis${suff} VREG_ADDRESS(%ecx), %xmm0
    jp      .L${opcode}_nan_is_${nanval}
    je      .L${opcode}_finish
    jb      .L${opcode}_less
.L${opcode}_nan_is_pos:
    incl    %eax
    jmp     .L${opcode}_finish
.L${opcode}_nan_is_neg:
.L${opcode}_less:
    decl    %eax
.L${opcode}_finish:
    SET_VREG %eax, rINST
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
