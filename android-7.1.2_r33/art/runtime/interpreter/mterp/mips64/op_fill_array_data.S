    /* fill-array-data vAA, +BBBBBBBB */
    .extern MterpFillArrayData
    EXPORT_PC
    lh      a1, 2(rPC)                  # a1 <- bbbb (lo)
    lh      a0, 4(rPC)                  # a0 <- BBBB (hi)
    srl     a3, rINST, 8                # a3 <- AA
    ins     a1, a0, 16, 16              # a1 <- BBBBbbbb
    GET_VREG_U a0, a3                   # a0 <- vAA (array object)
    dlsa    a1, a1, rPC, 1              # a1 <- PC + BBBBbbbb*2 (array data off.)
    jal     MterpFillArrayData          # (obj, payload)
    beqzc   v0, MterpPossibleException  # exception?
    FETCH_ADVANCE_INST 3                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
