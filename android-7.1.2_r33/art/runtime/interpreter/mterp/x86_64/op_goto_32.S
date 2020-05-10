/*
 * Unconditional branch, 32-bit offset.
 *
 * The branch distance is a signed code-unit offset, which we need to
 * double to get a byte offset.
 *
 *  Because we need the SF bit set, we'll use an adds
 * to convert from Dalvik offset to byte offset.
 */
    /* goto/32 +AAAAAAAA */
    movslq  2(rPC), rINSTq                  # rINSTq <- AAAAAAAA
    testq   rINSTq, rINSTq
    jmp     MterpCommonTakenBranch
