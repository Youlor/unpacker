    /* move-result-wide vAA */
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    ldr     r3, [rFP, #OFF_FP_RESULT_REGISTER]
    VREG_INDEX_TO_ADDR r2, rINST        @ r2<- &fp[AA]
    ldmia   r3, {r0-r1}                 @ r0/r1<- retval.j
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    stmia   r2, {r0-r1}                 @ fp[AA]<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
