    /*
     * Unconditional branch, 32-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     *
     * Unlike most opcodes, this one is allowed to branch to itself, so
     * our "backward branch" test must be "<=0" instead of "<0".  Because
     * we need the V bit set, we'll use an adds to convert from Dalvik
     * offset to byte offset.
     */
    /* goto/32 +AAAAAAAA */
    FETCH w0, 1                         // w0<- aaaa (lo)
    FETCH w1, 2                         // w1<- AAAA (hi)
    orr     wINST, w0, w1, lsl #16      // wINST<- AAAAaaaa
    b       MterpCommonTakenBranchNoFlags
