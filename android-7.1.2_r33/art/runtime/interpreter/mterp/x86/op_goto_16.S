/*
 * Unconditional branch, 16-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto/16 +AAAA */
    movswl  2(rPC), rINST                   # rINST <- ssssAAAA
    testl   rINST, rINST
    jmp     MterpCommonTakenBranch
