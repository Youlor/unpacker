%default { "is_object":"0" }
    /* for: move-result, move-result-object */
    /* op vAA */
    mov     r2, rINST, lsr #8           @ r2<- AA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    ldr     r0, [rFP, #OFF_FP_RESULT_REGISTER]  @ get pointer to result JType.
    ldr     r0, [r0]                    @ r0 <- result.i.
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT r0, r2, r1          @ fp[AA]<- r0
    .else
    SET_VREG r0, r2                     @ fp[AA]<- r0
    .endif
    GOTO_OPCODE ip                      @ jump to next instruction
