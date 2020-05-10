/*
 * Signed 64-bit integer multiply.
 *
 * We could definately use more free registers for
 * this code.   We spill rINSTw (ebx),
 * giving us eax, ebc, ecx and edx as computational
 * temps.  On top of that, we'll spill edi (rFP)
 * for use as the vB pointer and esi (rPC) for use
 * as the vC pointer.  Yuck.
 *
 */
    /* mul-long vAA, vBB, vCC */
    movzbl  2(rPC), %eax                    # eax <- B
    movzbl  3(rPC), %ecx                    # ecx <- C
    mov     rPC, LOCAL0(%esp)               # save Interpreter PC
    mov     rFP, LOCAL1(%esp)               # save FP
    mov     rIBASE, LOCAL2(%esp)            # save rIBASE
    leal    (rFP,%eax,4), %esi              # esi <- &v[B]
    leal    (rFP,%ecx,4), rFP               # rFP <- &v[C]
    movl    4(%esi), %ecx                   # ecx <- Bmsw
    imull   (rFP), %ecx                     # ecx <- (Bmsw*Clsw)
    movl    4(rFP), %eax                    # eax <- Cmsw
    imull   (%esi), %eax                    # eax <- (Cmsw*Blsw)
    addl    %eax, %ecx                      # ecx <- (Bmsw*Clsw)+(Cmsw*Blsw)
    movl    (rFP), %eax                     # eax <- Clsw
    mull    (%esi)                          # eax <- (Clsw*Alsw)
    mov     LOCAL0(%esp), rPC               # restore Interpreter PC
    mov     LOCAL1(%esp), rFP               # restore FP
    leal    (%ecx,rIBASE), rIBASE           # full result now in rIBASE:%eax
    SET_VREG_HIGH rIBASE, rINST             # v[B+1] <- rIBASE
    mov     LOCAL2(%esp), rIBASE            # restore IBASE
    SET_VREG %eax, rINST                    # v[B] <- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
