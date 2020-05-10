    FETCH_ADVANCE_INST 1                // advance to next instr, load rINST
    GET_INST_OPCODE ip                  // ip<- opcode from rINST
    GOTO_OPCODE ip                      // execute it
