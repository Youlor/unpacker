/*
 * Unconditional branch, 8-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto +AA */
    movsbl  rINSTbl, rINST                  # rINST <- ssssssAA
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
