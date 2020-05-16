    /* const/4 vA, #+B */
    sbfx    r1, rINST, #12, #4          @ r1<- sssssssB (sign-extended)
    ubfx    r0, rINST, #8, #4           @ r0<- A
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ ip<- opcode from rINST
    SET_VREG r1, r0                     @ fp[A]<- r1
    GOTO_OPCODE ip                      @ execute next instruction
