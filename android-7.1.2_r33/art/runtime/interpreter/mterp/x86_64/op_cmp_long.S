/*
 * Compare two 64-bit values.  Puts 0, 1, or -1 into the destination
 * register based on the results of the comparison.
 */
    /* cmp-long vAA, vBB, vCC */
    movzbq  2(rPC), %rdx                    # edx <- BB
    movzbq  3(rPC), %rcx                    # ecx <- CC
    GET_WIDE_VREG %rdx, %rdx                # rdx <- v[BB]
    xorl    %eax, %eax
    xorl    %edi, %edi
    addb    $$1, %al
    movl    $$-1, %esi
    cmpq    VREG_ADDRESS(%rcx), %rdx
    cmovl   %esi, %edi
    cmovg   %eax, %edi
    SET_VREG %edi, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
