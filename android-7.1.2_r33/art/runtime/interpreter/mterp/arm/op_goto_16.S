    /*
     * Unconditional branch, 16-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto/16 +AAAA */
    FETCH_S rINST, 1                    @ rINST<- ssssAAAA (sign-extended)
    b       MterpCommonTakenBranchNoFlags
