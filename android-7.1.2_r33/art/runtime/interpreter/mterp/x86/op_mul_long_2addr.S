/*
 * Signed 64-bit integer multiply, 2-addr version
 *
 * We could definately use more free registers for
 * this code.  We must spill %edx (rIBASE) because it
 * is used by imul.  We'll also spill rINST (ebx),
 * giving us eax, ebc, ecx and rIBASE as computational
 * temps.  On top of that, we'll spill %esi (edi)
 * for use as the vA pointer and rFP (esi) for use
 * as the vB pointer.  Yuck.
 */
    /* mul-long/2addr vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- BA
    andb    $$0xf, %al                      # eax <- A
    CLEAR_WIDE_REF %eax                     # clear refs in advance
    sarl    $$4, rINST                      # rINST <- B
    mov     rPC, LOCAL0(%esp)               # save Interpreter PC
    mov     rFP, LOCAL1(%esp)               # save FP
    mov     rIBASE, LOCAL2(%esp)            # save rIBASE
    leal    (rFP,%eax,4), %esi              # esi <- &v[A]
    leal    (rFP,rINST,4), rFP              # rFP <- &v[B]
    movl    4(%esi), %ecx                   # ecx <- Amsw
    imull   (rFP), %ecx                     # ecx <- (Amsw*Blsw)
    movl    4(rFP), %eax                    # eax <- Bmsw
    imull   (%esi), %eax                    # eax <- (Bmsw*Alsw)
    addl    %eax, %ecx                      # ecx <- (Amsw*Blsw)+(Bmsw*Alsw)
    movl    (rFP), %eax                     # eax <- Blsw
    mull    (%esi)                          # eax <- (Blsw*Alsw)
    leal    (%ecx,rIBASE), rIBASE           # full result now in %edx:%eax
    movl    rIBASE, 4(%esi)                 # v[A+1] <- rIBASE
    movl    %eax, (%esi)                    # v[A] <- %eax
    mov     LOCAL0(%esp), rPC               # restore Interpreter PC
    mov     LOCAL2(%esp), rIBASE            # restore IBASE
    mov     LOCAL1(%esp), rFP               # restore FP
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
