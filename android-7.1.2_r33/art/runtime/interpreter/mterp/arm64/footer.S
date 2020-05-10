/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */


/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogDivideByZeroException
#endif
    b MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogArrayIndexException
#endif
    b MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNegativeArraySizeException
#endif
    b MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNoSuchMethodException
#endif
    b MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNullObjectException
#endif
    b MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogExceptionThrownException
#endif
    b MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    ldr  x2, [xSELF, #THREAD_FLAGS_OFFSET]
    bl MterpLogSuspendFallback
#endif
    b MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    ldr     x0, [xSELF, #THREAD_EXCEPTION_OFFSET]
    cbz     x0, MterpFallback                       // If not, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    bl      MterpHandleException                    // (self, shadow_frame)
    cbz     w0, MterpExceptionReturn                // no local catch, back to caller.
    ldr     x0, [xFP, #OFF_FP_CODE_ITEM]
    ldr     w1, [xFP, #OFF_FP_DEX_PC]
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]
    add     xPC, x0, #CODEITEM_INSNS_OFFSET
    add     xPC, xPC, x1, lsl #1                    // generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    bl      MterpShouldSwitchInterpreters
    cbnz    w0, MterpFallback
    /* resume execution at catch block */
    EXPORT_PC
    FETCH_INST
    GET_INST_OPCODE ip
    GOTO_OPCODE ip
    /* NOTE: no fallthrough */
/*
 * Common handling for branches with support for Jit profiling.
 * On entry:
 *    wINST          <= signed offset
 *    wPROFILE       <= signed hotness countdown (expanded to 32 bits)
 *    condition bits <= set to establish sign of offset (use "NoFlags" entry if not)
 *
 * We have quite a few different cases for branch profiling, OSR detection and
 * suspend check support here.
 *
 * Taken backward branches:
 *    If profiling active, do hotness countdown and report if we hit zero.
 *    If in osr check mode, see if our target is a compiled loop header entry and do OSR if so.
 *    Is there a pending suspend request?  If so, suspend.
 *
 * Taken forward branches and not-taken backward branches:
 *    If in osr check mode, see if our target is a compiled loop header entry and do OSR if so.
 *
 * Our most common case is expected to be a taken backward branch with active jit profiling,
 * but no full OSR check and no pending suspend request.
 * Next most common case is not-taken branch with no full OSR check.
 *
 */
MterpCommonTakenBranchNoFlags:
    cmp     wINST, #0
    b.gt    .L_forward_branch           // don't add forward branches to hotness
    tbnz    wPROFILE, #31, .L_no_count_backwards  // go if negative
    subs    wPROFILE, wPROFILE, #1      // countdown
    b.eq    .L_add_batch                // counted down to zero - report
.L_resume_backward_branch:
    ldr     lr, [xSELF, #THREAD_FLAGS_OFFSET]
    add     w2, wINST, wINST            // w2<- byte offset
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    REFRESH_IBASE
    ands    lr, lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .L_suspend_request_pending
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

.L_suspend_request_pending:
    EXPORT_PC
    mov     x0, xSELF
    bl      MterpSuspendCheck           // (self)
    cbnz    x0, MterpFallback
    REFRESH_IBASE                       // might have changed during suspend
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

.L_no_count_backwards:
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.ne    .L_resume_backward_branch
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  // (self, shadow_frame, offset)
    cbnz    x0, MterpOnStackReplacement
    b       .L_resume_backward_branch

.L_forward_branch:
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_osr_forward
.L_resume_forward_branch:
    add     w2, wINST, wINST            // w2<- byte offset
    FETCH_ADVANCE_INST_RB w2            // update rPC, load wINST
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

.L_check_osr_forward:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  // (self, shadow_frame, offset)
    cbnz    x0, MterpOnStackReplacement
    b       .L_resume_forward_branch

.L_add_batch:
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    strh    wPROFILE, [x1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    ldr     x0, [xFP, #OFF_FP_METHOD]
    mov     x2, xSELF
    bl      MterpAddHotnessBatch        // (method, shadow_frame, self)
    mov     wPROFILE, w0                // restore new hotness countdown to wPROFILE
    b       .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    mov     x0, xSELF
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, #2
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  // (self, shadow_frame, offset)
    cbnz    x0, MterpOnStackReplacement
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction


/*
 * Check for suspend check request.  Assumes wINST already loaded, xPC advanced and
 * still needs to get the opcode and branch to it, and flags are in lr.
 */
MterpCheckSuspendAndContinue:
    ldr     xIBASE, [xSELF, #THREAD_CURRENT_IBASE_OFFSET]  // refresh xIBASE
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    check1
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
check1:
    EXPORT_PC
    mov     x0, xSELF
    bl      MterpSuspendCheck           // (self)
    cbnz    x0, MterpFallback           // Something in the environment changed, switch interpreters
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    sbfm x2, xINST, 0, 31
    bl MterpLogOSR
#endif
    mov  x0, #1                         // Signal normal return
    b    MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  x0, xSELF
    add  x1, xFP, #OFF_FP_SHADOWFRAME
    bl MterpLogFallback
#endif
MterpCommonFallback:
    mov     x0, #0                                  // signal retry with reference interpreter.
    b       MterpDone

/*
 * We pushed some registers on the stack in ExecuteMterpImpl, then saved
 * SP and LR.  Here we restore SP, restore the registers, and then restore
 * LR to PC.
 *
 * On entry:
 *  uint32_t* xFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    mov     x0, #1                                  // signal return to caller.
    b MterpDone
MterpReturn:
    ldr     x2, [xFP, #OFF_FP_RESULT_REGISTER]
    str     x0, [x2]
    mov     x0, #1                                  // signal return to caller.
MterpDone:
/*
 * At this point, we expect wPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending wPROFILE and the cached hotness counter).  wPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    cmp     wPROFILE, #0
    bgt     MterpProfileActive                      // if > 0, we may have some counts to report.
    ldp     fp, lr, [sp, #64]
    ldp     xPC, xFP, [sp, #48]
    ldp     xSELF, xINST, [sp, #32]
    ldp     xIBASE, xREFS, [sp, #16]
    ldp     xPROFILE, x27, [sp], #80
    ret

MterpProfileActive:
    mov     xINST, x0                               // stash return value
    /* Report cached hotness counts */
    ldr     x0, [xFP, #OFF_FP_METHOD]
    add     x1, xFP, #OFF_FP_SHADOWFRAME
    mov     x2, xSELF
    strh    wPROFILE, [x1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    bl      MterpAddHotnessBatch                    // (method, shadow_frame, self)
    mov     x0, xINST                               // restore return value
    ldp     fp, lr, [sp, #64]
    ldp     xPC, xFP, [sp, #48]
    ldp     xSELF, xINST, [sp, #32]
    ldp     xIBASE, xREFS, [sp, #16]
    ldp     xPROFILE, x27, [sp], #80
    ret

    .cfi_endproc
    .size   ExecuteMterpImpl, .-ExecuteMterpImpl

