    /* move-exception vAA */
    lsr     w2, wINST, #8               // w2<- AA
    ldr     x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    mov     x1, #0                      // w1<- 0
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    SET_VREG_OBJECT w3, w2              // fp[AA]<- exception obj
    GET_INST_OPCODE ip                  // extract opcode from rINST
    str     x1, [xSELF, #THREAD_EXCEPTION_OFFSET]  // clear exception
    GOTO_OPCODE ip                      // jump to next instruction
