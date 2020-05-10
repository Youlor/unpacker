/*
 * Unconditional branch, 16-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 */
    /* goto/16 +AAAA */
    movswq  2(rPC), rINSTq                  # rINSTq <- ssssAAAA
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
