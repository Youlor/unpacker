    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     w2, wINST
    bl      MterpIputObjectQuick
    cbz     w0, MterpException
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
