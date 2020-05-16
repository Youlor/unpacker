/*
 * Long integer shift.  This is different from the generic 32/64-bit
 * binary operations because vAA/vBB are 64-bit but vCC (the shift
 * distance) is 32-bit.  Also, Dalvik requires us to mask off the low
 * 6 bits of the shift distance.  x86 shifts automatically mask off
 * the low 5 bits of %cl, so have to handle the 64 > shiftcount > 31
 * case specially.
 */
    /* shl-long vAA, vBB, vCC */
    /* ecx gets shift count */
    /* Need to spill rINST */
    /* rINSTw gets AA */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    movl    rIBASE, LOCAL0(%esp)
    GET_VREG_HIGH rIBASE, %eax              # ecx <- v[BB+1]
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    GET_VREG %eax, %eax                     # eax <- v[BB+0]
    shldl   %eax,rIBASE
    sall    %cl, %eax
    testb   $$32, %cl
    je      2f
    movl    %eax, rIBASE
    xorl    %eax, %eax
2:
    SET_VREG_HIGH rIBASE, rINST             # v[AA+1] <- rIBASE
    movl    LOCAL0(%esp), rIBASE
    SET_VREG %eax, rINST                    # v[AA+0] <- %eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
