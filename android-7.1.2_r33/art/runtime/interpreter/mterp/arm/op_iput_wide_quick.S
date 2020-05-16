    /* iput-wide-quick vA, vB, offset@CCCC */
    mov     r2, rINST, lsr #12          @ r2<- B
    FETCH r3, 1                         @ r3<- field byte offset
    GET_VREG r2, r2                     @ r2<- fp[B], the object pointer
    ubfx    r0, rINST, #8, #4           @ r0<- A
    cmp     r2, #0                      @ check object for null
    beq     common_errNullObject        @ object was null
    VREG_INDEX_TO_ADDR r0, r0           @ r0<- &fp[A]
    ldmia   r0, {r0-r1}                 @ r0/r1<- fp[A]/fp[A+1]
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    strd    r0, [r2, r3]                @ obj.field<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
