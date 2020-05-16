%default {"result":"","special":"","rem":""}
/*
 * 32-bit binary div/rem operation.  Handles special case of op0=minint and
 * op1=-1.
 */
    /* div/rem vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- BB
    movzbl  3(rPC), %ecx                    # ecx <- CC
    GET_VREG %eax, %eax                     # eax <- vBB
    GET_VREG %ecx, %ecx                     # ecx <- vCC
    mov     rIBASE, LOCAL0(%esp)
    testl   %ecx, %ecx
    je      common_errDivideByZero
    movl    %eax, %edx
    orl     %ecx, %edx
    testl   $$0xFFFFFF00, %edx              # If both arguments are less
                                            #   than 8-bit and +ve
    jz      .L${opcode}_8                   # Do 8-bit divide
    testl   $$0xFFFF0000, %edx              # If both arguments are less
                                            #   than 16-bit and +ve
    jz      .L${opcode}_16                  # Do 16-bit divide
    cmpl    $$-1, %ecx
    jne     .L${opcode}_32
    cmpl    $$0x80000000, %eax
    jne     .L${opcode}_32
    movl    $special, $result
    jmp     .L${opcode}_finish
.L${opcode}_32:
    cltd
    idivl   %ecx
    jmp     .L${opcode}_finish
.L${opcode}_8:
    div     %cl                             # 8-bit divide otherwise.
                                            # Remainder in %ah, quotient in %al
    .if $rem
    movl    %eax, %edx
    shr     $$8, %edx
    .else
    andl    $$0x000000FF, %eax
    .endif
    jmp     .L${opcode}_finish
.L${opcode}_16:
    xorl    %edx, %edx                      # Clear %edx before divide
    div     %cx
.L${opcode}_finish:
    SET_VREG $result, rINST
    mov     LOCAL0(%esp), rIBASE
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
