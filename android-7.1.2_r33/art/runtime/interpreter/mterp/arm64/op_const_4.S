    /* const/4 vA, #+B */
    lsl     w1, wINST, #16              // w1<- Bxxx0000
    ubfx    w0, wINST, #8, #4           // w0<- A
    FETCH_ADVANCE_INST 1                // advance xPC, load wINST
    asr     w1, w1, #28                 // w1<- sssssssB (sign-extended)
    GET_INST_OPCODE ip                  // ip<- opcode from xINST
    SET_VREG w1, w0                     // fp[A]<- w1
    GOTO_OPCODE ip                      // execute next instruction
