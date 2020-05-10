    /* move-exception vAA */
    mov     r2, rINST, lsr #8           @ r2<- AA
    ldr     r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov     r1, #0                      @ r1<- 0
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    SET_VREG_OBJECT r3, r2              @ fp[AA]<- exception obj
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    str     r1, [rSELF, #THREAD_EXCEPTION_OFFSET]  @ clear exception
    GOTO_OPCODE ip                      @ jump to next instruction
