/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */

    .text
    .align  2

/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogDivideByZeroException)
#endif
    jmp     MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogArrayIndexException)
#endif
    jmp     MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogNegativeArraySizeException)
#endif
    jmp     MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogNoSuchMethodException)
#endif
    jmp     MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogNullObjectException)
#endif
    jmp     MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG0(%esp)
    call    SYMBOL(MterpLogExceptionThrownException)
#endif
    jmp     MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG0(%esp)
    movl    THREAD_FLAGS_OFFSET(%eax), %eax
    movl    %eax, OUT_ARG2(%esp)
    call    SYMBOL(MterpLogSuspendFallback)
#endif
    jmp     MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    movl    rSELF, %eax
    testl   $$-1, THREAD_EXCEPTION_OFFSET(%eax)
    jz      MterpFallback
    /* intentional fallthrough - handle pending exception. */

/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpHandleException)
    testb   %al, %al
    jz      MterpExceptionReturn
    movl    OFF_FP_CODE_ITEM(rFP), %eax
    movl    OFF_FP_DEX_PC(rFP), %ecx
    lea     CODEITEM_INSNS_OFFSET(%eax), rPC
    lea     (rPC, %ecx, 2), rPC
    movl    rPC, OFF_FP_DEX_PC_PTR(rFP)
    /* Do we need to switch interpreters? */
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    /* resume execution at catch block */
    REFRESH_IBASE
    FETCH_INST
    GOTO_NEXT
    /* NOTE: no fallthrough */

/*
 * Common handling for branches with support for Jit profiling.
 * On entry:
 *    rINST          <= signed offset
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
MterpCommonTakenBranch:
    jg      .L_forward_branch               # don't add forward branches to hotness
/*
 * We need to subtract 1 from positive values and we should not see 0 here,
 * so we may use the result of the comparison with -1.
 */
#if JIT_CHECK_OSR != -1
#  error "JIT_CHECK_OSR must be -1."
#endif
    cmpw    $$JIT_CHECK_OSR, rPROFILE
    je      .L_osr_check
    decw    rPROFILE
    je      .L_add_batch                    # counted down to zero - report
.L_resume_backward_branch:
    movl    rSELF, %eax
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%eax)
    leal    (rPC, rINST, 2), rPC
    FETCH_INST
    jnz     .L_suspend_request_pending
    REFRESH_IBASE
    GOTO_NEXT

.L_suspend_request_pending:
    EXPORT_PC
    movl    %eax, OUT_ARG0(%esp)            # rSELF in eax
    call    SYMBOL(MterpSuspendCheck)       # (self)
    testb   %al, %al
    jnz     MterpFallback
    REFRESH_IBASE                           # might have changed during suspend
    GOTO_NEXT

.L_no_count_backwards:
    cmpw    $$JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    jne     .L_resume_backward_branch
.L_osr_check:
    EXPORT_PC
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jz      .L_resume_backward_branch
    jmp     MterpOnStackReplacement

.L_forward_branch:
    cmpw    $$JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    je      .L_check_osr_forward
.L_resume_forward_branch:
    leal    (rPC, rINST, 2), rPC
    FETCH_INST
    GOTO_NEXT

.L_check_osr_forward:
    EXPORT_PC
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    REFRESH_IBASE
    jz      .L_resume_forward_branch
    jmp     MterpOnStackReplacement

.L_add_batch:
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG2(%esp)
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    jmp     .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    EXPORT_PC
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    $$2, OUT_ARG2(%esp)
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    REFRESH_IBASE
    jnz     MterpOnStackReplacement
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rINST, OUT_ARG2(%esp)
    call    SYMBOL(MterpLogOSR)
#endif
    movl    $$1, %eax
    jmp     MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movl    rSELF, %eax
    movl    %eax, OUT_ARG0(%esp)
    lea     OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    call    SYMBOL(MterpLogFallback)
#endif
MterpCommonFallback:
    xor     %eax, %eax
    jmp     MterpDone

/*
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    movl    $$1, %eax
    jmp     MterpDone
MterpReturn:
    movl    OFF_FP_RESULT_REGISTER(rFP), %edx
    movl    %eax, (%edx)
    movl    %ecx, 4(%edx)
    mov     $$1, %eax
MterpDone:
/*
 * At this point, we expect rPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending rPROFILE and the cached hotness counter).  rPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    cmpw    $$0, rPROFILE
    jle     MRestoreFrame                   # if > 0, we may have some counts to report.

    movl    %eax, rINST                     # stash return value
    /* Report cached hotness counts */
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %ecx
    movl    %ecx, OUT_ARG1(%esp)
    movl    rSELF, %eax
    movl    %eax, OUT_ARG2(%esp)
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    movl    rINST, %eax                     # restore return value

    /* pop up frame */
MRestoreFrame:
    addl    $$FRAME_SIZE, %esp
    .cfi_adjust_cfa_offset -FRAME_SIZE

    /* Restore callee save register */
    POP     %ebx
    POP     %esi
    POP     %edi
    POP     %ebp
    ret
    .cfi_endproc
    SIZE(ExecuteMterpImpl,ExecuteMterpImpl)
