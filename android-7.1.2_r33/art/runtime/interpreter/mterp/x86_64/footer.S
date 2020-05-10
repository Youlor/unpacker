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
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogDivideByZeroException)
#endif
    jmp     MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogArrayIndexException)
#endif
    jmp     MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNegativeArraySizeException)
#endif
    jmp     MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNoSuchMethodException)
#endif
    jmp     MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogNullObjectException)
#endif
    jmp     MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogExceptionThrownException)
#endif
    jmp     MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    THREAD_FLAGS_OFFSET(OUT_ARG0), OUT_32_ARG2
    call    SYMBOL(MterpLogSuspendFallback)
#endif
    jmp     MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    movq    rSELF, %rcx
    cmpq    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    jz      MterpFallback
    /* intentional fallthrough - handle pending exception. */

/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpHandleException)
    testb   %al, %al
    jz      MterpExceptionReturn
    movq    OFF_FP_CODE_ITEM(rFP), %rax
    mov     OFF_FP_DEX_PC(rFP), %ecx
    leaq    CODEITEM_INSNS_OFFSET(%rax), rPC
    leaq    (rPC, %rcx, 2), rPC
    movq    rPC, OFF_FP_DEX_PC_PTR(rFP)
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
 *    rPROFILE       <= signed hotness countdown (expanded to 32 bits)
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
    cmpl    $$JIT_CHECK_OSR, rPROFILE
    je      .L_osr_check
    decl    rPROFILE
    je      .L_add_batch                    # counted down to zero - report
.L_resume_backward_branch:
    movq    rSELF, %rax
    testl   $$(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST), THREAD_FLAGS_OFFSET(%rax)
    REFRESH_IBASE
    leaq    (rPC, rINSTq, 2), rPC
    FETCH_INST
    jnz     .L_suspend_request_pending
    GOTO_NEXT

.L_suspend_request_pending:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    call    SYMBOL(MterpSuspendCheck)       # (self)
    testb   %al, %al
    jnz     MterpFallback
    REFRESH_IBASE                           # might have changed during suspend
    GOTO_NEXT

.L_no_count_backwards:
    cmpl    $$JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    jne     .L_resume_backward_branch
.L_osr_check:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jz      .L_resume_backward_branch
    jmp     MterpOnStackReplacement

.L_forward_branch:
    cmpl    $$JIT_CHECK_OSR, rPROFILE         # possible OSR re-entry?
    je      .L_check_osr_forward
.L_resume_forward_branch:
    leaq    (rPC, rINSTq, 2), rPC
    FETCH_INST
    GOTO_NEXT

.L_check_osr_forward:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rINSTq, OUT_ARG2
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jz      .L_resume_forward_branch
    jmp     MterpOnStackReplacement

.L_add_batch:
    movl    rPROFILE, %eax
    movq    OFF_FP_METHOD(rFP), OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movw    %ax, OFF_FP_COUNTDOWN_OFFSET(rFP)
    movq    rSELF, OUT_ARG2
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    movswl  %ax, rPROFILE
    jmp     .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    $$2, OUT_32_ARG2
    call    SYMBOL(MterpMaybeDoOnStackReplacement) # (self, shadow_frame, offset)
    testb   %al, %al
    jnz     MterpOnStackReplacement
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movl    rINST, OUT_32_ARG2
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
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    call    SYMBOL(MterpLogFallback)
#endif
MterpCommonFallback:
    xorl    %eax, %eax
    jmp     MterpDone

/*
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    movl    $$1, %eax
    jmp     MterpDone
MterpReturn:
    movq    OFF_FP_RESULT_REGISTER(rFP), %rdx
    movq    %rax, (%rdx)
    movl    $$1, %eax
MterpDone:
/*
 * At this point, we expect rPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending rPROFILE and the cached hotness counter).  rPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    testl   rPROFILE, rPROFILE
    jle     MRestoreFrame                   # if > 0, we may have some counts to report.

    movl    %eax, rINST                     # stash return value
    /* Report cached hotness counts */
    movl    rPROFILE, %eax
    movq    OFF_FP_METHOD(rFP), OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movw    %ax, OFF_FP_COUNTDOWN_OFFSET(rFP)
    movq    rSELF, OUT_ARG2
    call    SYMBOL(MterpAddHotnessBatch)    # (method, shadow_frame, self)
    movl    rINST, %eax                     # restore return value

    /* pop up frame */
MRestoreFrame:
    addq    $$FRAME_SIZE, %rsp
    .cfi_adjust_cfa_offset -FRAME_SIZE

    /* Restore callee save register */
    POP %r15
    POP %r14
    POP %r13
    POP %r12
    POP %rbp
    POP %rbx
    ret
    .cfi_endproc
    SIZE(ExecuteMterpImpl,ExecuteMterpImpl)
