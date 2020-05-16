%default { "is_object":"0" }
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    FETCH(a1, 2)                           #  a1 <- BBBB
    FETCH(a0, 1)                           #  a0 <- AAAA
    FETCH_ADVANCE_INST(3)                  #  advance rPC, load rINST
    GET_VREG(a2, a1)                       #  a2 <- fp[BBBB]
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT(a2, a0)                #  fp[AAAA] <- a2
    .else
    SET_VREG(a2, a0)                       #  fp[AAAA] <- a2
    .endif
    GOTO_OPCODE(t0)                        #  jump to next instruction
