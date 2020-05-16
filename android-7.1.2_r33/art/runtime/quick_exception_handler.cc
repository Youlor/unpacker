/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "quick_exception_handler.h"

#include "arch/context.h"
#include "art_method-inl.h"
#include "dex_instruction.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "handle_scope-inl.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/throwable.h"
#include "oat_quick_method_header.h"
#include "stack_map.h"
#include "verifier/method_verifier.h"

namespace art {

static constexpr bool kDebugExceptionDelivery = false;
static constexpr size_t kInvalidFrameDepth = 0xffffffff;

QuickExceptionHandler::QuickExceptionHandler(Thread* self, bool is_deoptimization)
    : self_(self),
      context_(self->GetLongJumpContext()),
      is_deoptimization_(is_deoptimization),
      method_tracing_active_(is_deoptimization ||
                             Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled()),
      handler_quick_frame_(nullptr),
      handler_quick_frame_pc_(0),
      handler_method_header_(nullptr),
      handler_quick_arg0_(0),
      handler_method_(nullptr),
      handler_dex_pc_(0),
      clear_exception_(false),
      handler_frame_depth_(kInvalidFrameDepth) {}

// Finds catch handler.
class CatchBlockStackVisitor FINAL : public StackVisitor {
 public:
  CatchBlockStackVisitor(Thread* self, Context* context, Handle<mirror::Throwable>* exception,
                         QuickExceptionHandler* exception_handler)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(self, context, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        exception_(exception),
        exception_handler_(exception_handler) {
  }

  bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* method = GetMethod();
    exception_handler_->SetHandlerFrameDepth(GetFrameDepth());
    if (method == nullptr) {
      // This is the upcall, we remember the frame and last pc so that we may long jump to them.
      exception_handler_->SetHandlerQuickFramePc(GetCurrentQuickFramePc());
      exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
      exception_handler_->SetHandlerMethodHeader(GetCurrentOatQuickMethodHeader());
      uint32_t next_dex_pc;
      ArtMethod* next_art_method;
      bool has_next = GetNextMethodAndDexPc(&next_art_method, &next_dex_pc);
      // Report the method that did the down call as the handler.
      exception_handler_->SetHandlerDexPc(next_dex_pc);
      exception_handler_->SetHandlerMethod(next_art_method);
      if (!has_next) {
        // No next method? Check exception handler is set up for the unhandled exception handler
        // case.
        DCHECK_EQ(0U, exception_handler_->GetHandlerDexPc());
        DCHECK(nullptr == exception_handler_->GetHandlerMethod());
      }
      return false;  // End stack walk.
    }
    if (method->IsRuntimeMethod()) {
      // Ignore callee save method.
      DCHECK(method->IsCalleeSaveMethod());
      return true;
    }
    return HandleTryItems(method);
  }

 private:
  bool HandleTryItems(ArtMethod* method)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    uint32_t dex_pc = DexFile::kDexNoIndex;
    if (!method->IsNative()) {
      dex_pc = GetDexPc();
    }
    if (dex_pc != DexFile::kDexNoIndex) {
      bool clear_exception = false;
      StackHandleScope<1> hs(GetThread());
      Handle<mirror::Class> to_find(hs.NewHandle((*exception_)->GetClass()));
      uint32_t found_dex_pc = method->FindCatchBlock(to_find, dex_pc, &clear_exception);
      exception_handler_->SetClearException(clear_exception);
      if (found_dex_pc != DexFile::kDexNoIndex) {
        exception_handler_->SetHandlerMethod(method);
        exception_handler_->SetHandlerDexPc(found_dex_pc);
        exception_handler_->SetHandlerQuickFramePc(
            GetCurrentOatQuickMethodHeader()->ToNativeQuickPc(
                method, found_dex_pc, /* is_catch_handler */ true));
        exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
        exception_handler_->SetHandlerMethodHeader(GetCurrentOatQuickMethodHeader());
        return false;  // End stack walk.
      } else if (UNLIKELY(GetThread()->HasDebuggerShadowFrames())) {
        // We are going to unwind this frame. Did we prepare a shadow frame for debugging?
        size_t frame_id = GetFrameId();
        ShadowFrame* frame = GetThread()->FindDebuggerShadowFrame(frame_id);
        if (frame != nullptr) {
          // We will not execute this shadow frame so we can safely deallocate it.
          GetThread()->RemoveDebuggerShadowFrameMapping(frame_id);
          ShadowFrame::DeleteDeoptimizedFrame(frame);
        }
      }
    }
    return true;  // Continue stack walk.
  }

  // The exception we're looking for the catch block of.
  Handle<mirror::Throwable>* exception_;
  // The quick exception handler we're visiting for.
  QuickExceptionHandler* const exception_handler_;

  DISALLOW_COPY_AND_ASSIGN(CatchBlockStackVisitor);
};

void QuickExceptionHandler::FindCatch(mirror::Throwable* exception) {
  DCHECK(!is_deoptimization_);
  if (kDebugExceptionDelivery) {
    mirror::String* msg = exception->GetDetailMessage();
    std::string str_msg(msg != nullptr ? msg->ToModifiedUtf8() : "");
    self_->DumpStack(LOG(INFO) << "Delivering exception: " << PrettyTypeOf(exception)
                     << ": " << str_msg << "\n");
  }
  StackHandleScope<1> hs(self_);
  Handle<mirror::Throwable> exception_ref(hs.NewHandle(exception));

  // Walk the stack to find catch handler.
  CatchBlockStackVisitor visitor(self_, context_, &exception_ref, this);
  visitor.WalkStack(true);

  if (kDebugExceptionDelivery) {
    if (*handler_quick_frame_ == nullptr) {
      LOG(INFO) << "Handler is upcall";
    }
    if (handler_method_ != nullptr) {
      const DexFile& dex_file = *handler_method_->GetDeclaringClass()->GetDexCache()->GetDexFile();
      int line_number = dex_file.GetLineNumFromPC(handler_method_, handler_dex_pc_);
      LOG(INFO) << "Handler: " << PrettyMethod(handler_method_) << " (line: " << line_number << ")";
    }
  }
  if (clear_exception_) {
    // Exception was cleared as part of delivery.
    DCHECK(!self_->IsExceptionPending());
  } else {
    // Put exception back in root set with clear throw location.
    self_->SetException(exception_ref.Get());
  }
  // If the handler is in optimized code, we need to set the catch environment.
  if (*handler_quick_frame_ != nullptr &&
      handler_method_header_ != nullptr &&
      handler_method_header_->IsOptimized()) {
    SetCatchEnvironmentForOptimizedHandler(&visitor);
  }
}

static VRegKind ToVRegKind(DexRegisterLocation::Kind kind) {
  // Slightly hacky since we cannot map DexRegisterLocationKind and VRegKind
  // one to one. However, StackVisitor::GetVRegFromOptimizedCode only needs to
  // distinguish between core/FPU registers and low/high bits on 64-bit.
  switch (kind) {
    case DexRegisterLocation::Kind::kConstant:
    case DexRegisterLocation::Kind::kInStack:
      // VRegKind is ignored.
      return VRegKind::kUndefined;

    case DexRegisterLocation::Kind::kInRegister:
      // Selects core register. For 64-bit registers, selects low 32 bits.
      return VRegKind::kLongLoVReg;

    case DexRegisterLocation::Kind::kInRegisterHigh:
      // Selects core register. For 64-bit registers, selects high 32 bits.
      return VRegKind::kLongHiVReg;

    case DexRegisterLocation::Kind::kInFpuRegister:
      // Selects FPU register. For 64-bit registers, selects low 32 bits.
      return VRegKind::kDoubleLoVReg;

    case DexRegisterLocation::Kind::kInFpuRegisterHigh:
      // Selects FPU register. For 64-bit registers, selects high 32 bits.
      return VRegKind::kDoubleHiVReg;

    default:
      LOG(FATAL) << "Unexpected vreg location " << kind;
      UNREACHABLE();
  }
}

void QuickExceptionHandler::SetCatchEnvironmentForOptimizedHandler(StackVisitor* stack_visitor) {
  DCHECK(!is_deoptimization_);
  DCHECK(*handler_quick_frame_ != nullptr) << "Method should not be called on upcall exceptions";
  DCHECK(handler_method_ != nullptr && handler_method_header_->IsOptimized());

  if (kDebugExceptionDelivery) {
    self_->DumpStack(LOG(INFO) << "Setting catch phis: ");
  }

  const size_t number_of_vregs = handler_method_->GetCodeItem()->registers_size_;
  CodeInfo code_info = handler_method_header_->GetOptimizedCodeInfo();
  CodeInfoEncoding encoding = code_info.ExtractEncoding();

  // Find stack map of the catch block.
  StackMap catch_stack_map = code_info.GetCatchStackMapForDexPc(GetHandlerDexPc(), encoding);
  DCHECK(catch_stack_map.IsValid());
  DexRegisterMap catch_vreg_map =
      code_info.GetDexRegisterMapOf(catch_stack_map, encoding, number_of_vregs);
  if (!catch_vreg_map.IsValid()) {
    return;
  }

  // Find stack map of the throwing instruction.
  StackMap throw_stack_map =
      code_info.GetStackMapForNativePcOffset(stack_visitor->GetNativePcOffset(), encoding);
  DCHECK(throw_stack_map.IsValid());
  DexRegisterMap throw_vreg_map =
      code_info.GetDexRegisterMapOf(throw_stack_map, encoding, number_of_vregs);
  DCHECK(throw_vreg_map.IsValid());

  // Copy values between them.
  for (uint16_t vreg = 0; vreg < number_of_vregs; ++vreg) {
    DexRegisterLocation::Kind catch_location =
        catch_vreg_map.GetLocationKind(vreg, number_of_vregs, code_info, encoding);
    if (catch_location == DexRegisterLocation::Kind::kNone) {
      continue;
    }
    DCHECK(catch_location == DexRegisterLocation::Kind::kInStack);

    // Get vreg value from its current location.
    uint32_t vreg_value;
    VRegKind vreg_kind = ToVRegKind(throw_vreg_map.GetLocationKind(vreg,
                                                                   number_of_vregs,
                                                                   code_info,
                                                                   encoding));
    bool get_vreg_success = stack_visitor->GetVReg(stack_visitor->GetMethod(),
                                                   vreg,
                                                   vreg_kind,
                                                   &vreg_value);
    CHECK(get_vreg_success) << "VReg " << vreg << " was optimized out ("
                            << "method=" << PrettyMethod(stack_visitor->GetMethod()) << ", "
                            << "dex_pc=" << stack_visitor->GetDexPc() << ", "
                            << "native_pc_offset=" << stack_visitor->GetNativePcOffset() << ")";

    // Copy value to the catch phi's stack slot.
    int32_t slot_offset = catch_vreg_map.GetStackOffsetInBytes(vreg,
                                                               number_of_vregs,
                                                               code_info,
                                                               encoding);
    ArtMethod** frame_top = stack_visitor->GetCurrentQuickFrame();
    uint8_t* slot_address = reinterpret_cast<uint8_t*>(frame_top) + slot_offset;
    uint32_t* slot_ptr = reinterpret_cast<uint32_t*>(slot_address);
    *slot_ptr = vreg_value;
  }
}

// Prepares deoptimization.
class DeoptimizeStackVisitor FINAL : public StackVisitor {
 public:
  DeoptimizeStackVisitor(Thread* self,
                         Context* context,
                         QuickExceptionHandler* exception_handler,
                         bool single_frame)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(self, context, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        exception_handler_(exception_handler),
        prev_shadow_frame_(nullptr),
        stacked_shadow_frame_pushed_(false),
        single_frame_deopt_(single_frame),
        single_frame_done_(false),
        single_frame_deopt_method_(nullptr),
        single_frame_deopt_quick_method_header_(nullptr) {
  }

  ArtMethod* GetSingleFrameDeoptMethod() const {
    return single_frame_deopt_method_;
  }

  const OatQuickMethodHeader* GetSingleFrameDeoptQuickMethodHeader() const {
    return single_frame_deopt_quick_method_header_;
  }

  bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    exception_handler_->SetHandlerFrameDepth(GetFrameDepth());
    ArtMethod* method = GetMethod();
    if (method == nullptr || single_frame_done_) {
      // This is the upcall (or the next full frame in single-frame deopt), we remember the frame
      // and last pc so that we may long jump to them.
      exception_handler_->SetHandlerQuickFramePc(GetCurrentQuickFramePc());
      exception_handler_->SetHandlerQuickFrame(GetCurrentQuickFrame());
      exception_handler_->SetHandlerMethodHeader(GetCurrentOatQuickMethodHeader());
      if (!stacked_shadow_frame_pushed_) {
        // In case there is no deoptimized shadow frame for this upcall, we still
        // need to push a nullptr to the stack since there is always a matching pop after
        // the long jump.
        GetThread()->PushStackedShadowFrame(nullptr,
                                            StackedShadowFrameType::kDeoptimizationShadowFrame);
        stacked_shadow_frame_pushed_ = true;
      }
      return false;  // End stack walk.
    } else if (method->IsRuntimeMethod()) {
      // Ignore callee save method.
      DCHECK(method->IsCalleeSaveMethod());
      return true;
    } else if (method->IsNative()) {
      // If we return from JNI with a pending exception and want to deoptimize, we need to skip
      // the native method.
      // The top method is a runtime method, the native method comes next.
      CHECK_EQ(GetFrameDepth(), 1U);
      return true;
    } else {
      // Check if a shadow frame already exists for debugger's set-local-value purpose.
      const size_t frame_id = GetFrameId();
      ShadowFrame* new_frame = GetThread()->FindDebuggerShadowFrame(frame_id);
      const bool* updated_vregs;
      const size_t num_regs = method->GetCodeItem()->registers_size_;
      if (new_frame == nullptr) {
        new_frame = ShadowFrame::CreateDeoptimizedFrame(num_regs, nullptr, method, GetDexPc());
        updated_vregs = nullptr;
      } else {
        updated_vregs = GetThread()->GetUpdatedVRegFlags(frame_id);
        DCHECK(updated_vregs != nullptr);
      }
      HandleOptimizingDeoptimization(method, new_frame, updated_vregs);
      if (updated_vregs != nullptr) {
        // Calling Thread::RemoveDebuggerShadowFrameMapping will also delete the updated_vregs
        // array so this must come after we processed the frame.
        GetThread()->RemoveDebuggerShadowFrameMapping(frame_id);
        DCHECK(GetThread()->FindDebuggerShadowFrame(frame_id) == nullptr);
      }
      if (prev_shadow_frame_ != nullptr) {
        prev_shadow_frame_->SetLink(new_frame);
      } else {
        // Will be popped after the long jump after DeoptimizeStack(),
        // right before interpreter::EnterInterpreterFromDeoptimize().
        stacked_shadow_frame_pushed_ = true;
        GetThread()->PushStackedShadowFrame(
            new_frame,
            single_frame_deopt_
                ? StackedShadowFrameType::kSingleFrameDeoptimizationShadowFrame
                : StackedShadowFrameType::kDeoptimizationShadowFrame);
      }
      prev_shadow_frame_ = new_frame;

      if (single_frame_deopt_ && !IsInInlinedFrame()) {
        // Single-frame deopt ends at the first non-inlined frame and needs to store that method.
        exception_handler_->SetHandlerQuickArg0(reinterpret_cast<uintptr_t>(method));
        single_frame_done_ = true;
        single_frame_deopt_method_ = method;
        single_frame_deopt_quick_method_header_ = GetCurrentOatQuickMethodHeader();
      }
      return true;
    }
  }

 private:
  void HandleOptimizingDeoptimization(ArtMethod* m,
                                      ShadowFrame* new_frame,
                                      const bool* updated_vregs)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
    CodeInfo code_info = method_header->GetOptimizedCodeInfo();
    uintptr_t native_pc_offset = method_header->NativeQuickPcOffset(GetCurrentQuickFramePc());
    CodeInfoEncoding encoding = code_info.ExtractEncoding();
    StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset, encoding);
    const size_t number_of_vregs = m->GetCodeItem()->registers_size_;
    uint32_t register_mask = stack_map.GetRegisterMask(encoding.stack_map_encoding);
    DexRegisterMap vreg_map = IsInInlinedFrame()
        ? code_info.GetDexRegisterMapAtDepth(GetCurrentInliningDepth() - 1,
                                             code_info.GetInlineInfoOf(stack_map, encoding),
                                             encoding,
                                             number_of_vregs)
        : code_info.GetDexRegisterMapOf(stack_map, encoding, number_of_vregs);

    if (!vreg_map.IsValid()) {
      return;
    }

    for (uint16_t vreg = 0; vreg < number_of_vregs; ++vreg) {
      if (updated_vregs != nullptr && updated_vregs[vreg]) {
        // Keep the value set by debugger.
        continue;
      }

      DexRegisterLocation::Kind location =
          vreg_map.GetLocationKind(vreg, number_of_vregs, code_info, encoding);
      static constexpr uint32_t kDeadValue = 0xEBADDE09;
      uint32_t value = kDeadValue;
      bool is_reference = false;

      switch (location) {
        case DexRegisterLocation::Kind::kInStack: {
          const int32_t offset = vreg_map.GetStackOffsetInBytes(vreg,
                                                                number_of_vregs,
                                                                code_info,
                                                                encoding);
          const uint8_t* addr = reinterpret_cast<const uint8_t*>(GetCurrentQuickFrame()) + offset;
          value = *reinterpret_cast<const uint32_t*>(addr);
          uint32_t bit = (offset >> 2);
          if (stack_map.GetNumberOfStackMaskBits(encoding.stack_map_encoding) > bit &&
              stack_map.GetStackMaskBit(encoding.stack_map_encoding, bit)) {
            is_reference = true;
          }
          break;
        }
        case DexRegisterLocation::Kind::kInRegister:
        case DexRegisterLocation::Kind::kInRegisterHigh:
        case DexRegisterLocation::Kind::kInFpuRegister:
        case DexRegisterLocation::Kind::kInFpuRegisterHigh: {
          uint32_t reg = vreg_map.GetMachineRegister(vreg, number_of_vregs, code_info, encoding);
          bool result = GetRegisterIfAccessible(reg, ToVRegKind(location), &value);
          CHECK(result);
          if (location == DexRegisterLocation::Kind::kInRegister) {
            if (((1u << reg) & register_mask) != 0) {
              is_reference = true;
            }
          }
          break;
        }
        case DexRegisterLocation::Kind::kConstant: {
          value = vreg_map.GetConstant(vreg, number_of_vregs, code_info, encoding);
          if (value == 0) {
            // Make it a reference for extra safety.
            is_reference = true;
          }
          break;
        }
        case DexRegisterLocation::Kind::kNone: {
          break;
        }
        default: {
          LOG(FATAL)
              << "Unexpected location kind "
              << vreg_map.GetLocationInternalKind(vreg,
                                                  number_of_vregs,
                                                  code_info,
                                                  encoding);
          UNREACHABLE();
        }
      }
      if (is_reference) {
        new_frame->SetVRegReference(vreg, reinterpret_cast<mirror::Object*>(value));
      } else {
        new_frame->SetVReg(vreg, value);
      }
    }
  }

  static VRegKind GetVRegKind(uint16_t reg, const std::vector<int32_t>& kinds) {
    return static_cast<VRegKind>(kinds.at(reg * 2));
  }

  QuickExceptionHandler* const exception_handler_;
  ShadowFrame* prev_shadow_frame_;
  bool stacked_shadow_frame_pushed_;
  const bool single_frame_deopt_;
  bool single_frame_done_;
  ArtMethod* single_frame_deopt_method_;
  const OatQuickMethodHeader* single_frame_deopt_quick_method_header_;

  DISALLOW_COPY_AND_ASSIGN(DeoptimizeStackVisitor);
};

void QuickExceptionHandler::DeoptimizeStack() {
  DCHECK(is_deoptimization_);
  if (kDebugExceptionDelivery) {
    self_->DumpStack(LOG(INFO) << "Deoptimizing: ");
  }

  DeoptimizeStackVisitor visitor(self_, context_, this, false);
  visitor.WalkStack(true);

  // Restore deoptimization exception
  self_->SetException(Thread::GetDeoptimizationException());
}

void QuickExceptionHandler::DeoptimizeSingleFrame() {
  DCHECK(is_deoptimization_);

  if (VLOG_IS_ON(deopt) || kDebugExceptionDelivery) {
    LOG(INFO) << "Single-frame deopting:";
    DumpFramesWithType(self_, true);
  }

  DeoptimizeStackVisitor visitor(self_, context_, this, true);
  visitor.WalkStack(true);

  // Compiled code made an explicit deoptimization.
  ArtMethod* deopt_method = visitor.GetSingleFrameDeoptMethod();
  DCHECK(deopt_method != nullptr);
  if (Runtime::Current()->UseJitCompilation()) {
    Runtime::Current()->GetJit()->GetCodeCache()->InvalidateCompiledCodeFor(
        deopt_method, visitor.GetSingleFrameDeoptQuickMethodHeader());
  } else {
    // Transfer the code to interpreter.
    Runtime::Current()->GetInstrumentation()->UpdateMethodsCode(
        deopt_method, GetQuickToInterpreterBridge());
  }

  // PC needs to be of the quick-to-interpreter bridge.
  int32_t offset;
  #ifdef __LP64__
      offset = GetThreadOffset<8>(kQuickQuickToInterpreterBridge).Int32Value();
  #else
      offset = GetThreadOffset<4>(kQuickQuickToInterpreterBridge).Int32Value();
  #endif
  handler_quick_frame_pc_ = *reinterpret_cast<uintptr_t*>(
      reinterpret_cast<uint8_t*>(self_) + offset);
}

void QuickExceptionHandler::DeoptimizeSingleFrameArchDependentFixup() {
  // Architecture-dependent work. This is to get the LR right for x86 and x86-64.

  if (kRuntimeISA == InstructionSet::kX86 || kRuntimeISA == InstructionSet::kX86_64) {
    // On x86, the return address is on the stack, so just reuse it. Otherwise we would have to
    // change how longjump works.
    handler_quick_frame_ = reinterpret_cast<ArtMethod**>(
        reinterpret_cast<uintptr_t>(handler_quick_frame_) - sizeof(void*));
  }
}

// Unwinds all instrumentation stack frame prior to catch handler or upcall.
class InstrumentationStackVisitor : public StackVisitor {
 public:
  InstrumentationStackVisitor(Thread* self, size_t frame_depth)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(self, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        frame_depth_(frame_depth),
        instrumentation_frames_to_pop_(0) {
    CHECK_NE(frame_depth_, kInvalidFrameDepth);
  }

  bool VisitFrame() SHARED_REQUIRES(Locks::mutator_lock_) {
    size_t current_frame_depth = GetFrameDepth();
    if (current_frame_depth < frame_depth_) {
      CHECK(GetMethod() != nullptr);
      if (UNLIKELY(reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc()) == GetReturnPc())) {
        if (!IsInInlinedFrame()) {
          // We do not count inlined frames, because we do not instrument them. The reason we
          // include them in the stack walking is the check against `frame_depth_`, which is
          // given to us by a visitor that visits inlined frames.
          ++instrumentation_frames_to_pop_;
        }
      }
      return true;
    } else {
      // We reached the frame of the catch handler or the upcall.
      return false;
    }
  }

  size_t GetInstrumentationFramesToPop() const {
    return instrumentation_frames_to_pop_;
  }

 private:
  const size_t frame_depth_;
  size_t instrumentation_frames_to_pop_;

  DISALLOW_COPY_AND_ASSIGN(InstrumentationStackVisitor);
};

void QuickExceptionHandler::UpdateInstrumentationStack() {
  if (method_tracing_active_) {
    InstrumentationStackVisitor visitor(self_, handler_frame_depth_);
    visitor.WalkStack(true);

    size_t instrumentation_frames_to_pop = visitor.GetInstrumentationFramesToPop();
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    for (size_t i = 0; i < instrumentation_frames_to_pop; ++i) {
      instrumentation->PopMethodForUnwind(self_, is_deoptimization_);
    }
  }
}

void QuickExceptionHandler::DoLongJump(bool smash_caller_saves) {
  // Place context back on thread so it will be available when we continue.
  self_->ReleaseLongJumpContext(context_);
  context_->SetSP(reinterpret_cast<uintptr_t>(handler_quick_frame_));
  CHECK_NE(handler_quick_frame_pc_, 0u);
  context_->SetPC(handler_quick_frame_pc_);
  context_->SetArg0(handler_quick_arg0_);
  if (smash_caller_saves) {
    context_->SmashCallerSaves();
  }
  context_->DoLongJump();
  UNREACHABLE();
}

// Prints out methods with their type of frame.
class DumpFramesWithTypeStackVisitor FINAL : public StackVisitor {
 public:
  DumpFramesWithTypeStackVisitor(Thread* self, bool show_details = false)
      SHARED_REQUIRES(Locks::mutator_lock_)
      : StackVisitor(self, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        show_details_(show_details) {}

  bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* method = GetMethod();
    if (show_details_) {
      LOG(INFO) << "|> pc   = " << std::hex << GetCurrentQuickFramePc();
      LOG(INFO) << "|> addr = " << std::hex << reinterpret_cast<uintptr_t>(GetCurrentQuickFrame());
      if (GetCurrentQuickFrame() != nullptr && method != nullptr) {
        LOG(INFO) << "|> ret  = " << std::hex << GetReturnPc();
      }
    }
    if (method == nullptr) {
      // Transition, do go on, we want to unwind over bridges, all the way.
      if (show_details_) {
        LOG(INFO) << "N  <transition>";
      }
      return true;
    } else if (method->IsRuntimeMethod()) {
      if (show_details_) {
        LOG(INFO) << "R  " << PrettyMethod(method, true);
      }
      return true;
    } else {
      bool is_shadow = GetCurrentShadowFrame() != nullptr;
      LOG(INFO) << (is_shadow ? "S" : "Q")
                << ((!is_shadow && IsInInlinedFrame()) ? "i" : " ")
                << " "
                << PrettyMethod(method, true);
      return true;  // Go on.
    }
  }

 private:
  bool show_details_;

  DISALLOW_COPY_AND_ASSIGN(DumpFramesWithTypeStackVisitor);
};

void QuickExceptionHandler::DumpFramesWithType(Thread* self, bool details) {
  DumpFramesWithTypeStackVisitor visitor(self, details);
  visitor.WalkStack(true);
}

}  // namespace art
