    /* const vAA, #+BBBBbbbb */
    movl    2(rPC), %eax                    # grab all 32 bits at once
    SET_VREG %eax, rINSTq                   # vAA<- eax
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
