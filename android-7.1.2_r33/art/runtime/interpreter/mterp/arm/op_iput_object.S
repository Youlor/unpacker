    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    mov     r3, rSELF
    bl      MterpIputObject
    cmp     r0, #0
    beq     MterpException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
