    /*
     * Return the length of an array.
     */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG w0, w1                     // w0<- vB (object ref)
    cbz     w0, common_errNullObject    // yup, fail
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    ldr     w3, [x0, #MIRROR_ARRAY_LENGTH_OFFSET]    // w3<- array length
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w3, w2                     // vB<- length
    GOTO_OPCODE ip                      // jump to next instruction
