/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "art_method-inl.h"
#include "callee_save_frame.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "interpreter/interpreter.h"
#include "linear_alloc.h"
#include "method_reference.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat_quick_method_header.h"
#include "quick_exception_handler.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "stack.h"
#include "debugger.h"

namespace art {

// Visits the arguments as saved to the stack by a Runtime::kRefAndArgs callee save frame.
class QuickArgumentVisitor {
  // Number of bytes for each out register in the caller method's frame.
  static constexpr size_t kBytesStackArgLocation = 4;
  // Frame size in bytes of a callee-save frame for RefsAndArgs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize =
      GetCalleeSaveFrameSize(kRuntimeISA, Runtime::kRefsAndArgs);
#if defined(__arm__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | ...        |    4x6 bytes callee saves
  // | R3         |
  // | R2         |
  // | R1         |
  // | S15        |
  // | :          |
  // | S0         |
  // |            |    4x2 bytes padding
  // | Method*    |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = kArm32QuickCodeUseSoftFloat;
  static constexpr bool kAlignPairRegister = !kArm32QuickCodeUseSoftFloat;
  static constexpr bool kQuickSoftFloatAbi = kArm32QuickCodeUseSoftFloat;
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = !kArm32QuickCodeUseSoftFloat;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 3;
  static constexpr size_t kNumQuickFprArgs = kArm32QuickCodeUseSoftFloat ? 0 : 16;
  static constexpr bool kGprFprLockstep = false;
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset =
      arm::ArmCalleeSaveFpr1Offset(Runtime::kRefsAndArgs);  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset =
      arm::ArmCalleeSaveGpr1Offset(Runtime::kRefsAndArgs);  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset =
      arm::ArmCalleeSaveLrOffset(Runtime::kRefsAndArgs);  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__aarch64__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | X29        |
  // |  :         |
  // | X20        |
  // | X7         |
  // | :          |
  // | X1         |
  // | D7         |
  // |  :         |
  // | D0         |
  // |            |    padding
  // | Method*    |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 7;  // 7 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = false;
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset =
      arm64::Arm64CalleeSaveFpr1Offset(Runtime::kRefsAndArgs);  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset =
      arm64::Arm64CalleeSaveGpr1Offset(Runtime::kRefsAndArgs);  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset =
      arm64::Arm64CalleeSaveLrOffset(Runtime::kRefsAndArgs);  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__mips__) && !defined(__LP64__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | RA         |
  // | ...        |    callee saves
  // | A3         |    arg3
  // | A2         |    arg2
  // | A1         |    arg1
  // | F15        |
  // | F14        |    f_arg1
  // | F13        |
  // | F12        |    f_arg0
  // |            |    padding
  // | A0/Method* |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = true;
  static constexpr bool kQuickSoftFloatAbi = false;
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = true;
  static constexpr size_t kNumQuickGprArgs = 3;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 4;  // 2 arguments passed in FPRs. Floats can be passed
                                                 // only in even numbered registers and each double
                                                 // occupies two registers.
  static constexpr bool kGprFprLockstep = false;
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 16;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 32;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 76;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__mips__) && defined(__LP64__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | RA         |
  // | ...        |    callee saves
  // | A7         |    arg7
  // | A6         |    arg6
  // | A5         |    arg5
  // | A4         |    arg4
  // | A3         |    arg3
  // | A2         |    arg2
  // | A1         |    arg1
  // | F19        |    f_arg7
  // | F18        |    f_arg6
  // | F17        |    f_arg5
  // | F16        |    f_arg4
  // | F15        |    f_arg3
  // | F14        |    f_arg2
  // | F13        |    f_arg1
  // | F12        |    f_arg0
  // |            |    padding
  // | A0/Method* |  <- sp
  // NOTE: for Mip64, when A0 is skipped, F0 is also skipped.
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 7;  // 7 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 7;  // 7 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = true;

  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 24;  // Offset of first FPR arg (F1).
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 80;  // Offset of first GPR arg (A1).
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 200;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__i386__)
  // The callee save frame is pointed to by SP.
  // | argN        |  |
  // | ...         |  |
  // | arg4        |  |
  // | arg3 spill  |  |  Caller's frame
  // | arg2 spill  |  |
  // | arg1 spill  |  |
  // | Method*     | ---
  // | Return      |
  // | EBP,ESI,EDI |    callee saves
  // | EBX         |    arg3
  // | EDX         |    arg2
  // | ECX         |    arg1
  // | XMM3        |    float arg 4
  // | XMM2        |    float arg 3
  // | XMM1        |    float arg 2
  // | XMM0        |    float arg 1
  // | EAX/Method* |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 3;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 4;  // 4 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = false;
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 4;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 4 + 4*8;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 28 + 4*8;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__x86_64__)
  // The callee save frame is pointed to by SP.
  // | argN            |  |
  // | ...             |  |
  // | reg. arg spills |  |  Caller's frame
  // | Method*         | ---
  // | Return          |
  // | R15             |    callee save
  // | R14             |    callee save
  // | R13             |    callee save
  // | R12             |    callee save
  // | R9              |    arg5
  // | R8              |    arg4
  // | RSI/R6          |    arg1
  // | RBP/R5          |    callee save
  // | RBX/R3          |    callee save
  // | RDX/R2          |    arg2
  // | RCX/R1          |    arg3
  // | XMM7            |    float arg 8
  // | XMM6            |    float arg 7
  // | XMM5            |    float arg 6
  // | XMM4            |    float arg 5
  // | XMM3            |    float arg 4
  // | XMM2            |    float arg 3
  // | XMM1            |    float arg 2
  // | XMM0            |    float arg 1
  // | Padding         |
  // | RDI/Method*     |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 5;  // 5 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = false;
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 16;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 80 + 4*8;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 168 + 4*8;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    switch (gpr_index) {
      case 0: return (4 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 1: return (1 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 2: return (0 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 3: return (5 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 4: return (6 * GetBytesPerGprSpillLocation(kRuntimeISA));
      default:
      LOG(FATAL) << "Unexpected GPR index: " << gpr_index;
      return 0;
    }
  }
#else
#error "Unsupported architecture"
#endif

 public:
  // Special handling for proxy methods. Proxy methods are instance methods so the
  // 'this' object is the 1st argument. They also have the same frame layout as the
  // kRefAndArgs runtime method. Since 'this' is a reference, it is located in the
  // 1st GPR.
  static mirror::Object* GetProxyThisObject(ArtMethod** sp)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK((*sp)->IsProxyMethod());
    CHECK_GT(kNumQuickGprArgs, 0u);
    constexpr uint32_t kThisGprIndex = 0u;  // 'this' is in the 1st GPR.
    size_t this_arg_offset = kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset +
        GprIndexToGprOffset(kThisGprIndex);
    uint8_t* this_arg_address = reinterpret_cast<uint8_t*>(sp) + this_arg_offset;
    return reinterpret_cast<StackReference<mirror::Object>*>(this_arg_address)->AsMirrorPtr();
  }

  static ArtMethod* GetCallingMethod(ArtMethod** sp) SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    return GetCalleeSaveMethodCaller(sp, Runtime::kRefsAndArgs);
  }

  static ArtMethod* GetOuterMethod(ArtMethod** sp) SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    uint8_t* previous_sp =
        reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize;
    return *reinterpret_cast<ArtMethod**>(previous_sp);
  }

  static uint32_t GetCallingDexPc(ArtMethod** sp) SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    const size_t callee_frame_size = GetCalleeSaveFrameSize(kRuntimeISA, Runtime::kRefsAndArgs);
    ArtMethod** caller_sp = reinterpret_cast<ArtMethod**>(
        reinterpret_cast<uintptr_t>(sp) + callee_frame_size);
    uintptr_t outer_pc = QuickArgumentVisitor::GetCallingPc(sp);
    const OatQuickMethodHeader* current_code = (*caller_sp)->GetOatQuickMethodHeader(outer_pc);
    uintptr_t outer_pc_offset = current_code->NativeQuickPcOffset(outer_pc);

    if (current_code->IsOptimized()) {
      CodeInfo code_info = current_code->GetOptimizedCodeInfo();
      CodeInfoEncoding encoding = code_info.ExtractEncoding();
      StackMap stack_map = code_info.GetStackMapForNativePcOffset(outer_pc_offset, encoding);
      DCHECK(stack_map.IsValid());
      if (stack_map.HasInlineInfo(encoding.stack_map_encoding)) {
        InlineInfo inline_info = code_info.GetInlineInfoOf(stack_map, encoding);
        return inline_info.GetDexPcAtDepth(encoding.inline_info_encoding,
                                           inline_info.GetDepth(encoding.inline_info_encoding)-1);
      } else {
        return stack_map.GetDexPc(encoding.stack_map_encoding);
      }
    } else {
      return current_code->ToDexPc(*caller_sp, outer_pc);
    }
  }

  // For the given quick ref and args quick frame, return the caller's PC.
  static uintptr_t GetCallingPc(ArtMethod** sp) SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    uint8_t* lr = reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_LrOffset;
    return *reinterpret_cast<uintptr_t*>(lr);
  }

  QuickArgumentVisitor(ArtMethod** sp, bool is_static, const char* shorty,
                       uint32_t shorty_len) SHARED_REQUIRES(Locks::mutator_lock_) :
          is_static_(is_static), shorty_(shorty), shorty_len_(shorty_len),
          gpr_args_(reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset),
          fpr_args_(reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset),
          stack_args_(reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize
              + sizeof(ArtMethod*)),  // Skip ArtMethod*.
          gpr_index_(0), fpr_index_(0), fpr_double_index_(0), stack_index_(0),
          cur_type_(Primitive::kPrimVoid), is_split_long_or_double_(false) {
    static_assert(kQuickSoftFloatAbi == (kNumQuickFprArgs == 0),
                  "Number of Quick FPR arguments unexpected");
    static_assert(!(kQuickSoftFloatAbi && kQuickDoubleRegAlignedFloatBackFilled),
                  "Double alignment unexpected");
    // For register alignment, we want to assume that counters(fpr_double_index_) are even if the
    // next register is even.
    static_assert(!kQuickDoubleRegAlignedFloatBackFilled || kNumQuickFprArgs % 2 == 0,
                  "Number of Quick FPR arguments not even");
    DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), sizeof(void*));
  }

  virtual ~QuickArgumentVisitor() {}

  virtual void Visit() = 0;

  Primitive::Type GetParamPrimitiveType() const {
    return cur_type_;
  }

  uint8_t* GetParamAddress() const {
    if (!kQuickSoftFloatAbi) {
      Primitive::Type type = GetParamPrimitiveType();
      if (UNLIKELY((type == Primitive::kPrimDouble) || (type == Primitive::kPrimFloat))) {
        if (type == Primitive::kPrimDouble && kQuickDoubleRegAlignedFloatBackFilled) {
          if (fpr_double_index_ + 2 < kNumQuickFprArgs + 1) {
            return fpr_args_ + (fpr_double_index_ * GetBytesPerFprSpillLocation(kRuntimeISA));
          }
        } else if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
          return fpr_args_ + (fpr_index_ * GetBytesPerFprSpillLocation(kRuntimeISA));
        }
        return stack_args_ + (stack_index_ * kBytesStackArgLocation);
      }
    }
    if (gpr_index_ < kNumQuickGprArgs) {
      return gpr_args_ + GprIndexToGprOffset(gpr_index_);
    }
    return stack_args_ + (stack_index_ * kBytesStackArgLocation);
  }

  bool IsSplitLongOrDouble() const {
    if ((GetBytesPerGprSpillLocation(kRuntimeISA) == 4) ||
        (GetBytesPerFprSpillLocation(kRuntimeISA) == 4)) {
      return is_split_long_or_double_;
    } else {
      return false;  // An optimization for when GPR and FPRs are 64bit.
    }
  }

  bool IsParamAReference() const {
    return GetParamPrimitiveType() == Primitive::kPrimNot;
  }

  bool IsParamALongOrDouble() const {
    Primitive::Type type = GetParamPrimitiveType();
    return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
  }

  uint64_t ReadSplitLongParam() const {
    // The splitted long is always available through the stack.
    return *reinterpret_cast<uint64_t*>(stack_args_
        + stack_index_ * kBytesStackArgLocation);
  }

  void IncGprIndex() {
    gpr_index_++;
    if (kGprFprLockstep) {
      fpr_index_++;
    }
  }

  void IncFprIndex() {
    fpr_index_++;
    if (kGprFprLockstep) {
      gpr_index_++;
    }
  }

  void VisitArguments() SHARED_REQUIRES(Locks::mutator_lock_) {
    // (a) 'stack_args_' should point to the first method's argument
    // (b) whatever the argument type it is, the 'stack_index_' should
    //     be moved forward along with every visiting.
    gpr_index_ = 0;
    fpr_index_ = 0;
    if (kQuickDoubleRegAlignedFloatBackFilled) {
      fpr_double_index_ = 0;
    }
    stack_index_ = 0;
    if (!is_static_) {  // Handle this.
      cur_type_ = Primitive::kPrimNot;
      is_split_long_or_double_ = false;
      Visit();
      stack_index_++;
      if (kNumQuickGprArgs > 0) {
        IncGprIndex();
      }
    }
    for (uint32_t shorty_index = 1; shorty_index < shorty_len_; ++shorty_index) {
      cur_type_ = Primitive::GetType(shorty_[shorty_index]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          is_split_long_or_double_ = false;
          Visit();
          stack_index_++;
          if (gpr_index_ < kNumQuickGprArgs) {
            IncGprIndex();
          }
          break;
        case Primitive::kPrimFloat:
          is_split_long_or_double_ = false;
          Visit();
          stack_index_++;
          if (kQuickSoftFloatAbi) {
            if (gpr_index_ < kNumQuickGprArgs) {
              IncGprIndex();
            }
          } else {
            if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
              IncFprIndex();
              if (kQuickDoubleRegAlignedFloatBackFilled) {
                // Double should not overlap with float.
                // For example, if fpr_index_ = 3, fpr_double_index_ should be at least 4.
                fpr_double_index_ = std::max(fpr_double_index_, RoundUp(fpr_index_, 2));
                // Float should not overlap with double.
                if (fpr_index_ % 2 == 0) {
                  fpr_index_ = std::max(fpr_double_index_, fpr_index_);
                }
              } else if (kQuickSkipOddFpRegisters) {
                IncFprIndex();
              }
            }
          }
          break;
        case Primitive::kPrimDouble:
        case Primitive::kPrimLong:
          if (kQuickSoftFloatAbi || (cur_type_ == Primitive::kPrimLong)) {
            if (cur_type_ == Primitive::kPrimLong && kAlignPairRegister && gpr_index_ == 0) {
              // Currently, this is only for ARM and MIPS, where the first available parameter
              // register is R1 (on ARM) or A1 (on MIPS). So we skip it, and use R2 (on ARM) or
              // A2 (on MIPS) instead.
              IncGprIndex();
            }
            is_split_long_or_double_ = (GetBytesPerGprSpillLocation(kRuntimeISA) == 4) &&
                ((gpr_index_ + 1) == kNumQuickGprArgs);
            if (!kSplitPairAcrossRegisterAndStack && is_split_long_or_double_) {
              // We don't want to split this. Pass over this register.
              gpr_index_++;
              is_split_long_or_double_ = false;
            }
            Visit();
            if (kBytesStackArgLocation == 4) {
              stack_index_+= 2;
            } else {
              CHECK_EQ(kBytesStackArgLocation, 8U);
              stack_index_++;
            }
            if (gpr_index_ < kNumQuickGprArgs) {
              IncGprIndex();
              if (GetBytesPerGprSpillLocation(kRuntimeISA) == 4) {
                if (gpr_index_ < kNumQuickGprArgs) {
                  IncGprIndex();
                }
              }
            }
          } else {
            is_split_long_or_double_ = (GetBytesPerFprSpillLocation(kRuntimeISA) == 4) &&
                ((fpr_index_ + 1) == kNumQuickFprArgs) && !kQuickDoubleRegAlignedFloatBackFilled;
            Visit();
            if (kBytesStackArgLocation == 4) {
              stack_index_+= 2;
            } else {
              CHECK_EQ(kBytesStackArgLocation, 8U);
              stack_index_++;
            }
            if (kQuickDoubleRegAlignedFloatBackFilled) {
              if (fpr_double_index_ + 2 < kNumQuickFprArgs + 1) {
                fpr_double_index_ += 2;
                // Float should not overlap with double.
                if (fpr_index_ % 2 == 0) {
                  fpr_index_ = std::max(fpr_double_index_, fpr_index_);
                }
              }
            } else if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
              IncFprIndex();
              if (GetBytesPerFprSpillLocation(kRuntimeISA) == 4) {
                if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
                  IncFprIndex();
                }
              }
            }
          }
          break;
        default:
          LOG(FATAL) << "Unexpected type: " << cur_type_ << " in " << shorty_;
      }
    }
  }

 protected:
  const bool is_static_;
  const char* const shorty_;
  const uint32_t shorty_len_;

 private:
  uint8_t* const gpr_args_;  // Address of GPR arguments in callee save frame.
  uint8_t* const fpr_args_;  // Address of FPR arguments in callee save frame.
  uint8_t* const stack_args_;  // Address of stack arguments in caller's frame.
  uint32_t gpr_index_;  // Index into spilled GPRs.
  // Index into spilled FPRs.
  // In case kQuickDoubleRegAlignedFloatBackFilled, it may index a hole while fpr_double_index_
  // holds a higher register number.
  uint32_t fpr_index_;
  // Index into spilled FPRs for aligned double.
  // Only used when kQuickDoubleRegAlignedFloatBackFilled. Next available double register indexed in
  // terms of singles, may be behind fpr_index.
  uint32_t fpr_double_index_;
  uint32_t stack_index_;  // Index into arguments on the stack.
  // The current type of argument during VisitArguments.
  Primitive::Type cur_type_;
  // Does a 64bit parameter straddle the register and stack arguments?
  bool is_split_long_or_double_;
};

// Returns the 'this' object of a proxy method. This function is only used by StackVisitor. It
// allows to use the QuickArgumentVisitor constants without moving all the code in its own module.
extern "C" mirror::Object* artQuickGetProxyThisObject(ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return QuickArgumentVisitor::GetProxyThisObject(sp);
}

// Visits arguments on the stack placing them into the shadow frame.
class BuildQuickShadowFrameVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildQuickShadowFrameVisitor(ArtMethod** sp, bool is_static, const char* shorty,
                               uint32_t shorty_len, ShadowFrame* sf, size_t first_arg_reg) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), sf_(sf), cur_reg_(first_arg_reg) {}

  void Visit() SHARED_REQUIRES(Locks::mutator_lock_) OVERRIDE;

 private:
  ShadowFrame* const sf_;
  uint32_t cur_reg_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickShadowFrameVisitor);
};

void BuildQuickShadowFrameVisitor::Visit() {
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimLong:  // Fall-through.
    case Primitive::kPrimDouble:
      if (IsSplitLongOrDouble()) {
        sf_->SetVRegLong(cur_reg_, ReadSplitLongParam());
      } else {
        sf_->SetVRegLong(cur_reg_, *reinterpret_cast<jlong*>(GetParamAddress()));
      }
      ++cur_reg_;
      break;
    case Primitive::kPrimNot: {
        StackReference<mirror::Object>* stack_ref =
            reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
        sf_->SetVRegReference(cur_reg_, stack_ref->AsMirrorPtr());
      }
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
    case Primitive::kPrimFloat:
      sf_->SetVReg(cur_reg_, *reinterpret_cast<jint*>(GetParamAddress()));
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  ++cur_reg_;
}

extern "C" uint64_t artQuickToInterpreterBridge(ArtMethod* method, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // Ensure we don't get thread suspension until the object arguments are safely in the shadow
  // frame.
  ScopedQuickEntrypointChecks sqec(self);

  if (UNLIKELY(!method->IsInvokable())) {
    method->ThrowInvocationTimeError();
    return 0;
  }

  JValue tmp_value;
  ShadowFrame* deopt_frame = self->PopStackedShadowFrame(
      StackedShadowFrameType::kSingleFrameDeoptimizationShadowFrame, false);
  ManagedStack fragment;

  DCHECK(!method->IsNative()) << PrettyMethod(method);
  uint32_t shorty_len = 0;
  ArtMethod* non_proxy_method = method->GetInterfaceMethodIfProxy(sizeof(void*));
  const DexFile::CodeItem* code_item = non_proxy_method->GetCodeItem();
  DCHECK(code_item != nullptr) << PrettyMethod(method);
  const char* shorty = non_proxy_method->GetShorty(&shorty_len);

  JValue result;

  if (deopt_frame != nullptr) {
    // Coming from single-frame deopt.

    if (kIsDebugBuild) {
      // Sanity-check: are the methods as expected? We check that the last shadow frame (the bottom
      // of the call-stack) corresponds to the called method.
      ShadowFrame* linked = deopt_frame;
      while (linked->GetLink() != nullptr) {
        linked = linked->GetLink();
      }
      CHECK_EQ(method, linked->GetMethod()) << PrettyMethod(method) << " "
          << PrettyMethod(linked->GetMethod());
    }

    if (VLOG_IS_ON(deopt)) {
      // Print out the stack to verify that it was a single-frame deopt.
      LOG(INFO) << "Continue-ing from deopt. Stack is:";
      QuickExceptionHandler::DumpFramesWithType(self, true);
    }

    mirror::Throwable* pending_exception = nullptr;
    bool from_code = false;
    self->PopDeoptimizationContext(&result, &pending_exception, /* out */ &from_code);
    CHECK(from_code);

    // Push a transition back into managed code onto the linked list in thread.
    self->PushManagedStackFragment(&fragment);

    // Ensure that the stack is still in order.
    if (kIsDebugBuild) {
      class DummyStackVisitor : public StackVisitor {
       public:
        explicit DummyStackVisitor(Thread* self_in) SHARED_REQUIRES(Locks::mutator_lock_)
            : StackVisitor(self_in, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames) {}

        bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
          // Nothing to do here. In a debug build, SanityCheckFrame will do the work in the walking
          // logic. Just always say we want to continue.
          return true;
        }
      };
      DummyStackVisitor dsv(self);
      dsv.WalkStack();
    }

    // Restore the exception that was pending before deoptimization then interpret the
    // deoptimized frames.
    if (pending_exception != nullptr) {
      self->SetException(pending_exception);
    }
    interpreter::EnterInterpreterFromDeoptimize(self, deopt_frame, from_code, &result);
  } else {
    const char* old_cause = self->StartAssertNoThreadSuspension(
        "Building interpreter shadow frame");
    uint16_t num_regs = code_item->registers_size_;
    // No last shadow coming from quick.
    ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
        CREATE_SHADOW_FRAME(num_regs, /* link */ nullptr, method, /* dex pc */ 0);
    ShadowFrame* shadow_frame = shadow_frame_unique_ptr.get();
    size_t first_arg_reg = code_item->registers_size_ - code_item->ins_size_;
    BuildQuickShadowFrameVisitor shadow_frame_builder(sp, method->IsStatic(), shorty, shorty_len,
                                                      shadow_frame, first_arg_reg);
    shadow_frame_builder.VisitArguments();
    const bool needs_initialization =
        method->IsStatic() && !method->GetDeclaringClass()->IsInitialized();
    // Push a transition back into managed code onto the linked list in thread.
    self->PushManagedStackFragment(&fragment);
    self->PushShadowFrame(shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    if (needs_initialization) {
      // Ensure static method's class is initialized.
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(shadow_frame->GetMethod()->GetDeclaringClass()));
      if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
        DCHECK(Thread::Current()->IsExceptionPending()) << PrettyMethod(shadow_frame->GetMethod());
        self->PopManagedStackFragment(fragment);
        return 0;
      }
    }

    result = interpreter::EnterInterpreterFromEntryPoint(self, code_item, shadow_frame);
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);

  // Request a stack deoptimization if needed
  ArtMethod* caller = QuickArgumentVisitor::GetCallingMethod(sp);
  if (UNLIKELY(Dbg::IsForcedInterpreterNeededForUpcall(self, caller))) {
    // Push the context of the deoptimization stack so we can restore the return value and the
    // exception before executing the deoptimized frames.
    self->PushDeoptimizationContext(
        result, shorty[0] == 'L', /* from_code */ false, self->GetException());

    // Set special exception to cause deoptimization.
    self->SetException(Thread::GetDeoptimizationException());
  }

  // No need to restore the args since the method has already been run by the interpreter.
  return result.GetJ();
}

// Visits arguments on the stack placing them into the args vector, Object* arguments are converted
// to jobjects.
class BuildQuickArgumentVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildQuickArgumentVisitor(ArtMethod** sp, bool is_static, const char* shorty, uint32_t shorty_len,
                            ScopedObjectAccessUnchecked* soa, std::vector<jvalue>* args) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa), args_(args) {}

  void Visit() SHARED_REQUIRES(Locks::mutator_lock_) OVERRIDE;

  void FixupReferences() SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  ScopedObjectAccessUnchecked* const soa_;
  std::vector<jvalue>* const args_;
  // References which we must update when exiting in case the GC moved the objects.
  std::vector<std::pair<jobject, StackReference<mirror::Object>*>> references_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickArgumentVisitor);
};

void BuildQuickArgumentVisitor::Visit() {
  jvalue val;
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimNot: {
      StackReference<mirror::Object>* stack_ref =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
      val.l = soa_->AddLocalReference<jobject>(stack_ref->AsMirrorPtr());
      references_.push_back(std::make_pair(val.l, stack_ref));
      break;
    }
    case Primitive::kPrimLong:  // Fall-through.
    case Primitive::kPrimDouble:
      if (IsSplitLongOrDouble()) {
        val.j = ReadSplitLongParam();
      } else {
        val.j = *reinterpret_cast<jlong*>(GetParamAddress());
      }
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
    case Primitive::kPrimFloat:
      val.i = *reinterpret_cast<jint*>(GetParamAddress());
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  args_->push_back(val);
}

void BuildQuickArgumentVisitor::FixupReferences() {
  // Fixup any references which may have changed.
  for (const auto& pair : references_) {
    pair.second->Assign(soa_->Decode<mirror::Object*>(pair.first));
    soa_->Env()->DeleteLocalRef(pair.first);
  }
}

// Handler for invocation on proxy methods. On entry a frame will exist for the proxy object method
// which is responsible for recording callee save registers. We explicitly place into jobjects the
// incoming reference arguments (so they survive GC). We invoke the invocation handler, which is a
// field within the proxy object, which will box the primitive arguments and deal with error cases.
extern "C" uint64_t artQuickProxyInvokeHandler(
    ArtMethod* proxy_method, mirror::Object* receiver, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(proxy_method->IsProxyMethod()) << PrettyMethod(proxy_method);
  DCHECK(receiver->GetClass()->IsProxyClass()) << PrettyMethod(proxy_method);
  // Ensure we don't get thread suspension until the object arguments are safely in jobjects.
  const char* old_cause =
      self->StartAssertNoThreadSuspension("Adding to IRT proxy object arguments");
  // Register the top of the managed stack, making stack crawlable.
  DCHECK_EQ((*sp), proxy_method) << PrettyMethod(proxy_method);
  self->VerifyStack();
  // Start new JNI local reference state.
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  // Create local ref. copies of proxy method and the receiver.
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);

  // Placing arguments into args vector and remove the receiver.
  ArtMethod* non_proxy_method = proxy_method->GetInterfaceMethodIfProxy(sizeof(void*));
  CHECK(!non_proxy_method->IsStatic()) << PrettyMethod(proxy_method) << " "
                                       << PrettyMethod(non_proxy_method);
  std::vector<jvalue> args;
  uint32_t shorty_len = 0;
  const char* shorty = non_proxy_method->GetShorty(&shorty_len);
  BuildQuickArgumentVisitor local_ref_visitor(sp, false, shorty, shorty_len, &soa, &args);

  local_ref_visitor.VisitArguments();
  DCHECK_GT(args.size(), 0U) << PrettyMethod(proxy_method);
  args.erase(args.begin());

  // Convert proxy method into expected interface method.
  ArtMethod* interface_method = proxy_method->FindOverriddenMethod(sizeof(void*));
  DCHECK(interface_method != nullptr) << PrettyMethod(proxy_method);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  self->EndAssertNoThreadSuspension(old_cause);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(
      mirror::Method::CreateFromArtMethod(soa.Self(), interface_method));

  // All naked Object*s should now be in jobjects, so its safe to go into the main invoke code
  // that performs allocations.
  JValue result = InvokeProxyInvocationHandler(soa, shorty, rcvr_jobj, interface_method_jobj, args);
  // Restore references which might have moved.
  local_ref_visitor.FixupReferences();
  return result.GetJ();
}

// Read object references held in arguments from quick frames and place in a JNI local references,
// so they don't get garbage collected.
class RememberForGcArgumentVisitor FINAL : public QuickArgumentVisitor {
 public:
  RememberForGcArgumentVisitor(ArtMethod** sp, bool is_static, const char* shorty,
                               uint32_t shorty_len, ScopedObjectAccessUnchecked* soa) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa) {}

  void Visit() SHARED_REQUIRES(Locks::mutator_lock_) OVERRIDE;

  void FixupReferences() SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  ScopedObjectAccessUnchecked* const soa_;
  // References which we must update when exiting in case the GC moved the objects.
  std::vector<std::pair<jobject, StackReference<mirror::Object>*> > references_;

  DISALLOW_COPY_AND_ASSIGN(RememberForGcArgumentVisitor);
};

void RememberForGcArgumentVisitor::Visit() {
  if (IsParamAReference()) {
    StackReference<mirror::Object>* stack_ref =
        reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
    jobject reference =
        soa_->AddLocalReference<jobject>(stack_ref->AsMirrorPtr());
    references_.push_back(std::make_pair(reference, stack_ref));
  }
}

void RememberForGcArgumentVisitor::FixupReferences() {
  // Fixup any references which may have changed.
  for (const auto& pair : references_) {
    pair.second->Assign(soa_->Decode<mirror::Object*>(pair.first));
    soa_->Env()->DeleteLocalRef(pair.first);
  }
}

// Lazily resolve a method for quick. Called by stub code.
extern "C" const void* artQuickResolutionTrampoline(
    ArtMethod* called, mirror::Object* receiver, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // The resolution trampoline stashes the resolved method into the callee-save frame to transport
  // it. Thus, when exiting, the stack cannot be verified (as the resolved method most likely
  // does not have the same stack layout as the callee-save method).
  ScopedQuickEntrypointChecks sqec(self, kIsDebugBuild, false);
  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  const char* old_cause = self->StartAssertNoThreadSuspension("Quick method resolution set up");

  // Compute details about the called method (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  InvokeType invoke_type;
  MethodReference called_method(nullptr, 0);
  const bool called_method_known_on_entry = !called->IsRuntimeMethod();
  ArtMethod* caller = nullptr;
  if (!called_method_known_on_entry) {
    caller = QuickArgumentVisitor::GetCallingMethod(sp);
    uint32_t dex_pc = QuickArgumentVisitor::GetCallingDexPc(sp);
    const DexFile::CodeItem* code;
    called_method.dex_file = caller->GetDexFile();
    code = caller->GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    bool is_range;
    switch (instr_code) {
      case Instruction::INVOKE_DIRECT:
        invoke_type = kDirect;
        is_range = false;
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        invoke_type = kDirect;
        is_range = true;
        break;
      case Instruction::INVOKE_STATIC:
        invoke_type = kStatic;
        is_range = false;
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        invoke_type = kStatic;
        is_range = true;
        break;
      case Instruction::INVOKE_SUPER:
        invoke_type = kSuper;
        is_range = false;
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        invoke_type = kSuper;
        is_range = true;
        break;
      case Instruction::INVOKE_VIRTUAL:
        invoke_type = kVirtual;
        is_range = false;
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        invoke_type = kVirtual;
        is_range = true;
        break;
      case Instruction::INVOKE_INTERFACE:
        invoke_type = kInterface;
        is_range = false;
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        invoke_type = kInterface;
        is_range = true;
        break;
      default:
        LOG(FATAL) << "Unexpected call into trampoline: " << instr->DumpString(nullptr);
        UNREACHABLE();
    }
    called_method.dex_method_index = (is_range) ? instr->VRegB_3rc() : instr->VRegB_35c();
  } else {
    invoke_type = kStatic;
    called_method.dex_file = called->GetDexFile();
    called_method.dex_method_index = called->GetDexMethodIndex();
  }
  uint32_t shorty_len;
  const char* shorty =
      called_method.dex_file->GetMethodShorty(
          called_method.dex_file->GetMethodId(called_method.dex_method_index), &shorty_len);
  RememberForGcArgumentVisitor visitor(sp, invoke_type == kStatic, shorty, shorty_len, &soa);
  visitor.VisitArguments();
  self->EndAssertNoThreadSuspension(old_cause);
  const bool virtual_or_interface = invoke_type == kVirtual || invoke_type == kInterface;
  // Resolve method filling in dex cache.
  if (!called_method_known_on_entry) {
    StackHandleScope<1> hs(self);
    mirror::Object* dummy = nullptr;
    HandleWrapper<mirror::Object> h_receiver(
        hs.NewHandleWrapper(virtual_or_interface ? &receiver : &dummy));
    DCHECK_EQ(caller->GetDexFile(), called_method.dex_file);
    called = linker->ResolveMethod<ClassLinker::kForceICCECheck>(
        self, called_method.dex_method_index, caller, invoke_type);
  }
  const void* code = nullptr;
  if (LIKELY(!self->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type))
        << PrettyMethod(called) << " " << invoke_type;
    if (virtual_or_interface || invoke_type == kSuper) {
      // Refine called method based on receiver for kVirtual/kInterface, and
      // caller for kSuper.
      ArtMethod* orig_called = called;
      if (invoke_type == kVirtual) {
        CHECK(receiver != nullptr) << invoke_type;
        called = receiver->GetClass()->FindVirtualMethodForVirtual(called, sizeof(void*));
      } else if (invoke_type == kInterface) {
        CHECK(receiver != nullptr) << invoke_type;
        called = receiver->GetClass()->FindVirtualMethodForInterface(called, sizeof(void*));
      } else {
        DCHECK_EQ(invoke_type, kSuper);
        CHECK(caller != nullptr) << invoke_type;
        StackHandleScope<2> hs(self);
        Handle<mirror::DexCache> dex_cache(
            hs.NewHandle(caller->GetDeclaringClass()->GetDexCache()));
        Handle<mirror::ClassLoader> class_loader(
            hs.NewHandle(caller->GetDeclaringClass()->GetClassLoader()));
        // TODO Maybe put this into a mirror::Class function.
        mirror::Class* ref_class = linker->ResolveReferencedClassOfMethod(
            called_method.dex_method_index, dex_cache, class_loader);
        if (ref_class->IsInterface()) {
          called = ref_class->FindVirtualMethodForInterfaceSuper(called, sizeof(void*));
        } else {
          called = caller->GetDeclaringClass()->GetSuperClass()->GetVTableEntry(
              called->GetMethodIndex(), sizeof(void*));
        }
      }

      CHECK(called != nullptr) << PrettyMethod(orig_called) << " "
                               << PrettyTypeOf(receiver) << " "
                               << invoke_type << " " << orig_called->GetVtableIndex();

      // We came here because of sharpening. Ensure the dex cache is up-to-date on the method index
      // of the sharpened method avoiding dirtying the dex cache if possible.
      // Note, called_method.dex_method_index references the dex method before the
      // FindVirtualMethodFor... This is ok for FindDexMethodIndexInOtherDexFile that only cares
      // about the name and signature.
      uint32_t update_dex_cache_method_index = called->GetDexMethodIndex();
      if (!called->HasSameDexCacheResolvedMethods(caller, sizeof(void*))) {
        // Calling from one dex file to another, need to compute the method index appropriate to
        // the caller's dex file. Since we get here only if the original called was a runtime
        // method, we've got the correct dex_file and a dex_method_idx from above.
        DCHECK(!called_method_known_on_entry);
        DCHECK_EQ(caller->GetDexFile(), called_method.dex_file);
        const DexFile* caller_dex_file = called_method.dex_file;
        uint32_t caller_method_name_and_sig_index = called_method.dex_method_index;
        update_dex_cache_method_index =
            called->FindDexMethodIndexInOtherDexFile(*caller_dex_file,
                                                     caller_method_name_and_sig_index);
      }
      if ((update_dex_cache_method_index != DexFile::kDexNoIndex) &&
          (caller->GetDexCacheResolvedMethod(
              update_dex_cache_method_index, sizeof(void*)) != called)) {
        caller->SetDexCacheResolvedMethod(update_dex_cache_method_index, called, sizeof(void*));
      }
    } else if (invoke_type == kStatic) {
      const auto called_dex_method_idx = called->GetDexMethodIndex();
      // For static invokes, we may dispatch to the static method in the superclass but resolve
      // using the subclass. To prevent getting slow paths on each invoke, we force set the
      // resolved method for the super class dex method index if we are in the same dex file.
      // b/19175856
      if (called->GetDexFile() == called_method.dex_file &&
          called_method.dex_method_index != called_dex_method_idx) {
        called->GetDexCache()->SetResolvedMethod(called_dex_method_idx, called, sizeof(void*));
      }
    }

    // Ensure that the called method's class is initialized.
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> called_class(hs.NewHandle(called->GetDeclaringClass()));
    linker->EnsureInitialized(soa.Self(), called_class, true, true);
    if (LIKELY(called_class->IsInitialized())) {
      if (UNLIKELY(Dbg::IsForcedInterpreterNeededForResolution(self, called))) {
        // If we are single-stepping or the called method is deoptimized (by a
        // breakpoint, for example), then we have to execute the called method
        // with the interpreter.
        code = GetQuickToInterpreterBridge();
      } else if (UNLIKELY(Dbg::IsForcedInstrumentationNeededForResolution(self, caller))) {
        // If the caller is deoptimized (by a breakpoint, for example), we have to
        // continue its execution with interpreter when returning from the called
        // method. Because we do not want to execute the called method with the
        // interpreter, we wrap its execution into the instrumentation stubs.
        // When the called method returns, it will execute the instrumentation
        // exit hook that will determine the need of the interpreter with a call
        // to Dbg::IsForcedInterpreterNeededForUpcall and deoptimize the stack if
        // it is needed.
        code = GetQuickInstrumentationEntryPoint();
      } else {
        code = called->GetEntryPointFromQuickCompiledCode();
      }
    } else if (called_class->IsInitializing()) {
      if (UNLIKELY(Dbg::IsForcedInterpreterNeededForResolution(self, called))) {
        // If we are single-stepping or the called method is deoptimized (by a
        // breakpoint, for example), then we have to execute the called method
        // with the interpreter.
        code = GetQuickToInterpreterBridge();
      } else if (invoke_type == kStatic) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetQuickOatCodeFor(called);
      } else {
        // No trampoline for non-static methods.
        code = called->GetEntryPointFromQuickCompiledCode();
      }
    } else {
      DCHECK(called_class->IsErroneous());
    }
  }
  CHECK_EQ(code == nullptr, self->IsExceptionPending());
  // Fixup any locally saved objects may have moved during a GC.
  visitor.FixupReferences();
  // Place called method in callee-save frame to be placed as first argument to quick method.
  *sp = called;

  return code;
}

/*
 * This class uses a couple of observations to unite the different calling conventions through
 * a few constants.
 *
 * 1) Number of registers used for passing is normally even, so counting down has no penalty for
 *    possible alignment.
 * 2) Known 64b architectures store 8B units on the stack, both for integral and floating point
 *    types, so using uintptr_t is OK. Also means that we can use kRegistersNeededX to denote
 *    when we have to split things
 * 3) The only soft-float, Arm, is 32b, so no widening needs to be taken into account for floats
 *    and we can use Int handling directly.
 * 4) Only 64b architectures widen, and their stack is aligned 8B anyways, so no padding code
 *    necessary when widening. Also, widening of Ints will take place implicitly, and the
 *    extension should be compatible with Aarch64, which mandates copying the available bits
 *    into LSB and leaving the rest unspecified.
 * 5) Aligning longs and doubles is necessary on arm only, and it's the same in registers and on
 *    the stack.
 * 6) There is only little endian.
 *
 *
 * Actual work is supposed to be done in a delegate of the template type. The interface is as
 * follows:
 *
 * void PushGpr(uintptr_t):   Add a value for the next GPR
 *
 * void PushFpr4(float):      Add a value for the next FPR of size 32b. Is only called if we need
 *                            padding, that is, think the architecture is 32b and aligns 64b.
 *
 * void PushFpr8(uint64_t):   Push a double. We _will_ call this on 32b, it's the callee's job to
 *                            split this if necessary. The current state will have aligned, if
 *                            necessary.
 *
 * void PushStack(uintptr_t): Push a value to the stack.
 *
 * uintptr_t PushHandleScope(mirror::Object* ref): Add a reference to the HandleScope. This _will_ have nullptr,
 *                                          as this might be important for null initialization.
 *                                          Must return the jobject, that is, the reference to the
 *                                          entry in the HandleScope (nullptr if necessary).
 *
 */
template<class T> class BuildNativeCallFrameStateMachine {
 public:
#if defined(__arm__)
  // TODO: These are all dummy values!
  static constexpr bool kNativeSoftFloatAbi = true;
  static constexpr size_t kNumNativeGprArgs = 4;  // 4 arguments passed in GPRs, r0-r3
  static constexpr size_t kNumNativeFprArgs = 0;  // 0 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = true;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = true;
  static constexpr bool kAlignDoubleOnStack = true;
#elif defined(__aarch64__)
  static constexpr bool kNativeSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 8;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 8;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__mips__) && !defined(__LP64__)
  static constexpr bool kNativeSoftFloatAbi = true;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 4;  // 4 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 0;  // 0 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = true;
  static constexpr bool kMultiFPRegistersWidened = true;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = true;
  static constexpr bool kAlignDoubleOnStack = true;
#elif defined(__mips__) && defined(__LP64__)
  // Let the code prepare GPRs only and we will load the FPRs with same data.
  static constexpr bool kNativeSoftFloatAbi = true;
  static constexpr size_t kNumNativeGprArgs = 8;
  static constexpr size_t kNumNativeFprArgs = 0;

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = true;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__i386__)
  // TODO: Check these!
  static constexpr bool kNativeSoftFloatAbi = false;  // Not using int registers for fp
  static constexpr size_t kNumNativeGprArgs = 0;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 0;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = false;  // x86 not using regs, anyways
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__x86_64__)
  static constexpr bool kNativeSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 6;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 8;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#else
#error "Unsupported architecture"
#endif

 public:
  explicit BuildNativeCallFrameStateMachine(T* delegate)
      : gpr_index_(kNumNativeGprArgs),
        fpr_index_(kNumNativeFprArgs),
        stack_entries_(0),
        delegate_(delegate) {
    // For register alignment, we want to assume that counters (gpr_index_, fpr_index_) are even iff
    // the next register is even; counting down is just to make the compiler happy...
    static_assert(kNumNativeGprArgs % 2 == 0U, "Number of native GPR arguments not even");
    static_assert(kNumNativeFprArgs % 2 == 0U, "Number of native FPR arguments not even");
  }

  virtual ~BuildNativeCallFrameStateMachine() {}

  bool HavePointerGpr() const {
    return gpr_index_ > 0;
  }

  void AdvancePointer(const void* val) {
    if (HavePointerGpr()) {
      gpr_index_--;
      PushGpr(reinterpret_cast<uintptr_t>(val));
    } else {
      stack_entries_++;  // TODO: have a field for pointer length as multiple of 32b
      PushStack(reinterpret_cast<uintptr_t>(val));
      gpr_index_ = 0;
    }
  }

  bool HaveHandleScopeGpr() const {
    return gpr_index_ > 0;
  }

  void AdvanceHandleScope(mirror::Object* ptr) SHARED_REQUIRES(Locks::mutator_lock_) {
    uintptr_t handle = PushHandle(ptr);
    if (HaveHandleScopeGpr()) {
      gpr_index_--;
      PushGpr(handle);
    } else {
      stack_entries_++;
      PushStack(handle);
      gpr_index_ = 0;
    }
  }

  bool HaveIntGpr() const {
    return gpr_index_ > 0;
  }

  void AdvanceInt(uint32_t val) {
    if (HaveIntGpr()) {
      gpr_index_--;
      if (kMultiGPRegistersWidened) {
        DCHECK_EQ(sizeof(uintptr_t), sizeof(int64_t));
        PushGpr(static_cast<int64_t>(bit_cast<int32_t, uint32_t>(val)));
      } else {
        PushGpr(val);
      }
    } else {
      stack_entries_++;
      if (kMultiGPRegistersWidened) {
        DCHECK_EQ(sizeof(uintptr_t), sizeof(int64_t));
        PushStack(static_cast<int64_t>(bit_cast<int32_t, uint32_t>(val)));
      } else {
        PushStack(val);
      }
      gpr_index_ = 0;
    }
  }

  bool HaveLongGpr() const {
    return gpr_index_ >= kRegistersNeededForLong + (LongGprNeedsPadding() ? 1 : 0);
  }

  bool LongGprNeedsPadding() const {
    return kRegistersNeededForLong > 1 &&     // only pad when using multiple registers
        kAlignLongOnStack &&                  // and when it needs alignment
        (gpr_index_ & 1) == 1;                // counter is odd, see constructor
  }

  bool LongStackNeedsPadding() const {
    return kRegistersNeededForLong > 1 &&     // only pad when using multiple registers
        kAlignLongOnStack &&                  // and when it needs 8B alignment
        (stack_entries_ & 1) == 1;            // counter is odd
  }

  void AdvanceLong(uint64_t val) {
    if (HaveLongGpr()) {
      if (LongGprNeedsPadding()) {
        PushGpr(0);
        gpr_index_--;
      }
      if (kRegistersNeededForLong == 1) {
        PushGpr(static_cast<uintptr_t>(val));
      } else {
        PushGpr(static_cast<uintptr_t>(val & 0xFFFFFFFF));
        PushGpr(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
      }
      gpr_index_ -= kRegistersNeededForLong;
    } else {
      if (LongStackNeedsPadding()) {
        PushStack(0);
        stack_entries_++;
      }
      if (kRegistersNeededForLong == 1) {
        PushStack(static_cast<uintptr_t>(val));
        stack_entries_++;
      } else {
        PushStack(static_cast<uintptr_t>(val & 0xFFFFFFFF));
        PushStack(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
        stack_entries_ += 2;
      }
      gpr_index_ = 0;
    }
  }

  bool HaveFloatFpr() const {
    return fpr_index_ > 0;
  }

  void AdvanceFloat(float val) {
    if (kNativeSoftFloatAbi) {
      AdvanceInt(bit_cast<uint32_t, float>(val));
    } else {
      if (HaveFloatFpr()) {
        fpr_index_--;
        if (kRegistersNeededForDouble == 1) {
          if (kMultiFPRegistersWidened) {
            PushFpr8(bit_cast<uint64_t, double>(val));
          } else {
            // No widening, just use the bits.
            PushFpr8(static_cast<uint64_t>(bit_cast<uint32_t, float>(val)));
          }
        } else {
          PushFpr4(val);
        }
      } else {
        stack_entries_++;
        if (kRegistersNeededForDouble == 1 && kMultiFPRegistersWidened) {
          // Need to widen before storing: Note the "double" in the template instantiation.
          // Note: We need to jump through those hoops to make the compiler happy.
          DCHECK_EQ(sizeof(uintptr_t), sizeof(uint64_t));
          PushStack(static_cast<uintptr_t>(bit_cast<uint64_t, double>(val)));
        } else {
          PushStack(static_cast<uintptr_t>(bit_cast<uint32_t, float>(val)));
        }
        fpr_index_ = 0;
      }
    }
  }

  bool HaveDoubleFpr() const {
    return fpr_index_ >= kRegistersNeededForDouble + (DoubleFprNeedsPadding() ? 1 : 0);
  }

  bool DoubleFprNeedsPadding() const {
    return kRegistersNeededForDouble > 1 &&     // only pad when using multiple registers
        kAlignDoubleOnStack &&                  // and when it needs alignment
        (fpr_index_ & 1) == 1;                  // counter is odd, see constructor
  }

  bool DoubleStackNeedsPadding() const {
    return kRegistersNeededForDouble > 1 &&     // only pad when using multiple registers
        kAlignDoubleOnStack &&                  // and when it needs 8B alignment
        (stack_entries_ & 1) == 1;              // counter is odd
  }

  void AdvanceDouble(uint64_t val) {
    if (kNativeSoftFloatAbi) {
      AdvanceLong(val);
    } else {
      if (HaveDoubleFpr()) {
        if (DoubleFprNeedsPadding()) {
          PushFpr4(0);
          fpr_index_--;
        }
        PushFpr8(val);
        fpr_index_ -= kRegistersNeededForDouble;
      } else {
        if (DoubleStackNeedsPadding()) {
          PushStack(0);
          stack_entries_++;
        }
        if (kRegistersNeededForDouble == 1) {
          PushStack(static_cast<uintptr_t>(val));
          stack_entries_++;
        } else {
          PushStack(static_cast<uintptr_t>(val & 0xFFFFFFFF));
          PushStack(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
          stack_entries_ += 2;
        }
        fpr_index_ = 0;
      }
    }
  }

  uint32_t GetStackEntries() const {
    return stack_entries_;
  }

  uint32_t GetNumberOfUsedGprs() const {
    return kNumNativeGprArgs - gpr_index_;
  }

  uint32_t GetNumberOfUsedFprs() const {
    return kNumNativeFprArgs - fpr_index_;
  }

 private:
  void PushGpr(uintptr_t val) {
    delegate_->PushGpr(val);
  }
  void PushFpr4(float val) {
    delegate_->PushFpr4(val);
  }
  void PushFpr8(uint64_t val) {
    delegate_->PushFpr8(val);
  }
  void PushStack(uintptr_t val) {
    delegate_->PushStack(val);
  }
  uintptr_t PushHandle(mirror::Object* ref) SHARED_REQUIRES(Locks::mutator_lock_) {
    return delegate_->PushHandle(ref);
  }

  uint32_t gpr_index_;      // Number of free GPRs
  uint32_t fpr_index_;      // Number of free FPRs
  uint32_t stack_entries_;  // Stack entries are in multiples of 32b, as floats are usually not
                            // extended
  T* const delegate_;             // What Push implementation gets called
};

// Computes the sizes of register stacks and call stack area. Handling of references can be extended
// in subclasses.
//
// To handle native pointers, use "L" in the shorty for an object reference, which simulates
// them with handles.
class ComputeNativeCallFrameSize {
 public:
  ComputeNativeCallFrameSize() : num_stack_entries_(0) {}

  virtual ~ComputeNativeCallFrameSize() {}

  uint32_t GetStackSize() const {
    return num_stack_entries_ * sizeof(uintptr_t);
  }

  uint8_t* LayoutCallStack(uint8_t* sp8) const {
    sp8 -= GetStackSize();
    // Align by kStackAlignment.
    sp8 = reinterpret_cast<uint8_t*>(RoundDown(reinterpret_cast<uintptr_t>(sp8), kStackAlignment));
    return sp8;
  }

  uint8_t* LayoutCallRegisterStacks(uint8_t* sp8, uintptr_t** start_gpr, uint32_t** start_fpr)
      const {
    // Assumption is OK right now, as we have soft-float arm
    size_t fregs = BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>::kNumNativeFprArgs;
    sp8 -= fregs * sizeof(uintptr_t);
    *start_fpr = reinterpret_cast<uint32_t*>(sp8);
    size_t iregs = BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>::kNumNativeGprArgs;
    sp8 -= iregs * sizeof(uintptr_t);
    *start_gpr = reinterpret_cast<uintptr_t*>(sp8);
    return sp8;
  }

  uint8_t* LayoutNativeCall(uint8_t* sp8, uintptr_t** start_stack, uintptr_t** start_gpr,
                            uint32_t** start_fpr) const {
    // Native call stack.
    sp8 = LayoutCallStack(sp8);
    *start_stack = reinterpret_cast<uintptr_t*>(sp8);

    // Put fprs and gprs below.
    sp8 = LayoutCallRegisterStacks(sp8, start_gpr, start_fpr);

    // Return the new bottom.
    return sp8;
  }

  virtual void WalkHeader(
      BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm ATTRIBUTE_UNUSED)
      SHARED_REQUIRES(Locks::mutator_lock_) {
  }

  void Walk(const char* shorty, uint32_t shorty_len) SHARED_REQUIRES(Locks::mutator_lock_) {
    BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize> sm(this);

    WalkHeader(&sm);

    for (uint32_t i = 1; i < shorty_len; ++i) {
      Primitive::Type cur_type_ = Primitive::GetType(shorty[i]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
          // TODO: fix abuse of mirror types.
          sm.AdvanceHandleScope(
              reinterpret_cast<mirror::Object*>(0x12345678));
          break;

        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          sm.AdvanceInt(0);
          break;
        case Primitive::kPrimFloat:
          sm.AdvanceFloat(0);
          break;
        case Primitive::kPrimDouble:
          sm.AdvanceDouble(0);
          break;
        case Primitive::kPrimLong:
          sm.AdvanceLong(0);
          break;
        default:
          LOG(FATAL) << "Unexpected type: " << cur_type_ << " in " << shorty;
          UNREACHABLE();
      }
    }

    num_stack_entries_ = sm.GetStackEntries();
  }

  void PushGpr(uintptr_t /* val */) {
    // not optimizing registers, yet
  }

  void PushFpr4(float /* val */) {
    // not optimizing registers, yet
  }

  void PushFpr8(uint64_t /* val */) {
    // not optimizing registers, yet
  }

  void PushStack(uintptr_t /* val */) {
    // counting is already done in the superclass
  }

  virtual uintptr_t PushHandle(mirror::Object* /* ptr */) {
    return reinterpret_cast<uintptr_t>(nullptr);
  }

 protected:
  uint32_t num_stack_entries_;
};

class ComputeGenericJniFrameSize FINAL : public ComputeNativeCallFrameSize {
 public:
  ComputeGenericJniFrameSize() : num_handle_scope_references_(0) {}

  // Lays out the callee-save frame. Assumes that the incorrect frame corresponding to RefsAndArgs
  // is at *m = sp. Will update to point to the bottom of the save frame.
  //
  // Note: assumes ComputeAll() has been run before.
  void LayoutCalleeSaveFrame(Thread* self, ArtMethod*** m, void* sp, HandleScope** handle_scope)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ArtMethod* method = **m;

    DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), sizeof(void*));

    uint8_t* sp8 = reinterpret_cast<uint8_t*>(sp);

    // First, fix up the layout of the callee-save frame.
    // We have to squeeze in the HandleScope, and relocate the method pointer.

    // "Free" the slot for the method.
    sp8 += sizeof(void*);  // In the callee-save frame we use a full pointer.

    // Under the callee saves put handle scope and new method stack reference.
    size_t handle_scope_size = HandleScope::SizeOf(num_handle_scope_references_);
    size_t scope_and_method = handle_scope_size + sizeof(ArtMethod*);

    sp8 -= scope_and_method;
    // Align by kStackAlignment.
    sp8 = reinterpret_cast<uint8_t*>(RoundDown(reinterpret_cast<uintptr_t>(sp8), kStackAlignment));

    uint8_t* sp8_table = sp8 + sizeof(ArtMethod*);
    *handle_scope = HandleScope::Create(sp8_table, self->GetTopHandleScope(),
                                        num_handle_scope_references_);

    // Add a slot for the method pointer, and fill it. Fix the pointer-pointer given to us.
    uint8_t* method_pointer = sp8;
    auto** new_method_ref = reinterpret_cast<ArtMethod**>(method_pointer);
    *new_method_ref = method;
    *m = new_method_ref;
  }

  // Adds space for the cookie. Note: may leave stack unaligned.
  void LayoutCookie(uint8_t** sp) const {
    // Reference cookie and padding
    *sp -= 8;
  }

  // Re-layout the callee-save frame (insert a handle-scope). Then add space for the cookie.
  // Returns the new bottom. Note: this may be unaligned.
  uint8_t* LayoutJNISaveFrame(Thread* self, ArtMethod*** m, void* sp, HandleScope** handle_scope)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    // First, fix up the layout of the callee-save frame.
    // We have to squeeze in the HandleScope, and relocate the method pointer.
    LayoutCalleeSaveFrame(self, m, sp, handle_scope);

    // The bottom of the callee-save frame is now where the method is, *m.
    uint8_t* sp8 = reinterpret_cast<uint8_t*>(*m);

    // Add space for cookie.
    LayoutCookie(&sp8);

    return sp8;
  }

  // WARNING: After this, *sp won't be pointing to the method anymore!
  uint8_t* ComputeLayout(Thread* self, ArtMethod*** m, const char* shorty, uint32_t shorty_len,
                         HandleScope** handle_scope, uintptr_t** start_stack, uintptr_t** start_gpr,
                         uint32_t** start_fpr)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Walk(shorty, shorty_len);

    // JNI part.
    uint8_t* sp8 = LayoutJNISaveFrame(self, m, reinterpret_cast<void*>(*m), handle_scope);

    sp8 = LayoutNativeCall(sp8, start_stack, start_gpr, start_fpr);

    // Return the new bottom.
    return sp8;
  }

  uintptr_t PushHandle(mirror::Object* /* ptr */) OVERRIDE;

  // Add JNIEnv* and jobj/jclass before the shorty-derived elements.
  void WalkHeader(BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  uint32_t num_handle_scope_references_;
};

uintptr_t ComputeGenericJniFrameSize::PushHandle(mirror::Object* /* ptr */) {
  num_handle_scope_references_++;
  return reinterpret_cast<uintptr_t>(nullptr);
}

void ComputeGenericJniFrameSize::WalkHeader(
    BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm) {
  // JNIEnv
  sm->AdvancePointer(nullptr);

  // Class object or this as first argument
  sm->AdvanceHandleScope(reinterpret_cast<mirror::Object*>(0x12345678));
}

// Class to push values to three separate regions. Used to fill the native call part. Adheres to
// the template requirements of BuildGenericJniFrameStateMachine.
class FillNativeCall {
 public:
  FillNativeCall(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) :
      cur_gpr_reg_(gpr_regs), cur_fpr_reg_(fpr_regs), cur_stack_arg_(stack_args) {}

  virtual ~FillNativeCall() {}

  void Reset(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) {
    cur_gpr_reg_ = gpr_regs;
    cur_fpr_reg_ = fpr_regs;
    cur_stack_arg_ = stack_args;
  }

  void PushGpr(uintptr_t val) {
    *cur_gpr_reg_ = val;
    cur_gpr_reg_++;
  }

  void PushFpr4(float val) {
    *cur_fpr_reg_ = val;
    cur_fpr_reg_++;
  }

  void PushFpr8(uint64_t val) {
    uint64_t* tmp = reinterpret_cast<uint64_t*>(cur_fpr_reg_);
    *tmp = val;
    cur_fpr_reg_ += 2;
  }

  void PushStack(uintptr_t val) {
    *cur_stack_arg_ = val;
    cur_stack_arg_++;
  }

  virtual uintptr_t PushHandle(mirror::Object*) SHARED_REQUIRES(Locks::mutator_lock_) {
    LOG(FATAL) << "(Non-JNI) Native call does not use handles.";
    UNREACHABLE();
  }

 private:
  uintptr_t* cur_gpr_reg_;
  uint32_t* cur_fpr_reg_;
  uintptr_t* cur_stack_arg_;
};

// Visits arguments on the stack placing them into a region lower down the stack for the benefit
// of transitioning into native code.
class BuildGenericJniFrameVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildGenericJniFrameVisitor(Thread* self, bool is_static, const char* shorty, uint32_t shorty_len,
                              ArtMethod*** sp)
     : QuickArgumentVisitor(*sp, is_static, shorty, shorty_len),
       jni_call_(nullptr, nullptr, nullptr, nullptr), sm_(&jni_call_) {
    ComputeGenericJniFrameSize fsc;
    uintptr_t* start_gpr_reg;
    uint32_t* start_fpr_reg;
    uintptr_t* start_stack_arg;
    bottom_of_used_area_ = fsc.ComputeLayout(self, sp, shorty, shorty_len,
                                             &handle_scope_,
                                             &start_stack_arg,
                                             &start_gpr_reg, &start_fpr_reg);

    jni_call_.Reset(start_gpr_reg, start_fpr_reg, start_stack_arg, handle_scope_);

    // jni environment is always first argument
    sm_.AdvancePointer(self->GetJniEnv());

    if (is_static) {
      sm_.AdvanceHandleScope((**sp)->GetDeclaringClass());
    }
  }

  void Visit() SHARED_REQUIRES(Locks::mutator_lock_) OVERRIDE;

  void FinalizeHandleScope(Thread* self) SHARED_REQUIRES(Locks::mutator_lock_);

  StackReference<mirror::Object>* GetFirstHandleScopeEntry() {
    return handle_scope_->GetHandle(0).GetReference();
  }

  jobject GetFirstHandleScopeJObject() const SHARED_REQUIRES(Locks::mutator_lock_) {
    return handle_scope_->GetHandle(0).ToJObject();
  }

  void* GetBottomOfUsedArea() const {
    return bottom_of_used_area_;
  }

 private:
  // A class to fill a JNI call. Adds reference/handle-scope management to FillNativeCall.
  class FillJniCall FINAL : public FillNativeCall {
   public:
    FillJniCall(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args,
                HandleScope* handle_scope) : FillNativeCall(gpr_regs, fpr_regs, stack_args),
                                             handle_scope_(handle_scope), cur_entry_(0) {}

    uintptr_t PushHandle(mirror::Object* ref) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_);

    void Reset(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args, HandleScope* scope) {
      FillNativeCall::Reset(gpr_regs, fpr_regs, stack_args);
      handle_scope_ = scope;
      cur_entry_ = 0U;
    }

    void ResetRemainingScopeSlots() SHARED_REQUIRES(Locks::mutator_lock_) {
      // Initialize padding entries.
      size_t expected_slots = handle_scope_->NumberOfReferences();
      while (cur_entry_ < expected_slots) {
        handle_scope_->GetMutableHandle(cur_entry_++).Assign(nullptr);
      }
      DCHECK_NE(cur_entry_, 0U);
    }

   private:
    HandleScope* handle_scope_;
    size_t cur_entry_;
  };

  HandleScope* handle_scope_;
  FillJniCall jni_call_;
  void* bottom_of_used_area_;

  BuildNativeCallFrameStateMachine<FillJniCall> sm_;

  DISALLOW_COPY_AND_ASSIGN(BuildGenericJniFrameVisitor);
};

uintptr_t BuildGenericJniFrameVisitor::FillJniCall::PushHandle(mirror::Object* ref) {
  uintptr_t tmp;
  MutableHandle<mirror::Object> h = handle_scope_->GetMutableHandle(cur_entry_);
  h.Assign(ref);
  tmp = reinterpret_cast<uintptr_t>(h.ToJObject());
  cur_entry_++;
  return tmp;
}

void BuildGenericJniFrameVisitor::Visit() {
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimLong: {
      jlong long_arg;
      if (IsSplitLongOrDouble()) {
        long_arg = ReadSplitLongParam();
      } else {
        long_arg = *reinterpret_cast<jlong*>(GetParamAddress());
      }
      sm_.AdvanceLong(long_arg);
      break;
    }
    case Primitive::kPrimDouble: {
      uint64_t double_arg;
      if (IsSplitLongOrDouble()) {
        // Read into union so that we don't case to a double.
        double_arg = ReadSplitLongParam();
      } else {
        double_arg = *reinterpret_cast<uint64_t*>(GetParamAddress());
      }
      sm_.AdvanceDouble(double_arg);
      break;
    }
    case Primitive::kPrimNot: {
      StackReference<mirror::Object>* stack_ref =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
      sm_.AdvanceHandleScope(stack_ref->AsMirrorPtr());
      break;
    }
    case Primitive::kPrimFloat:
      sm_.AdvanceFloat(*reinterpret_cast<float*>(GetParamAddress()));
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
      sm_.AdvanceInt(*reinterpret_cast<jint*>(GetParamAddress()));
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

void BuildGenericJniFrameVisitor::FinalizeHandleScope(Thread* self) {
  // Clear out rest of the scope.
  jni_call_.ResetRemainingScopeSlots();
  // Install HandleScope.
  self->PushHandleScope(handle_scope_);
}

#if defined(__arm__) || defined(__aarch64__)
extern "C" void* artFindNativeMethod();
#else
extern "C" void* artFindNativeMethod(Thread* self);
#endif

uint64_t artQuickGenericJniEndJNIRef(Thread* self, uint32_t cookie, jobject l, jobject lock) {
  if (lock != nullptr) {
    return reinterpret_cast<uint64_t>(JniMethodEndWithReferenceSynchronized(l, cookie, lock, self));
  } else {
    return reinterpret_cast<uint64_t>(JniMethodEndWithReference(l, cookie, self));
  }
}

void artQuickGenericJniEndJNINonRef(Thread* self, uint32_t cookie, jobject lock) {
  if (lock != nullptr) {
    JniMethodEndSynchronized(cookie, lock, self);
  } else {
    JniMethodEnd(cookie, self);
  }
}

/*
 * Initializes an alloca region assumed to be directly below sp for a native call:
 * Create a HandleScope and call stack and fill a mini stack with values to be pushed to registers.
 * The final element on the stack is a pointer to the native code.
 *
 * On entry, the stack has a standard callee-save frame above sp, and an alloca below it.
 * We need to fix this, as the handle scope needs to go into the callee-save frame.
 *
 * The return of this function denotes:
 * 1) How many bytes of the alloca can be released, if the value is non-negative.
 * 2) An error, if the value is negative.
 */
extern "C" TwoWordReturn artQuickGenericJniTrampoline(Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtMethod* called = *sp;
  DCHECK(called->IsNative()) << PrettyMethod(called, true);
  uint32_t shorty_len = 0;
  const char* shorty = called->GetShorty(&shorty_len);

  // Run the visitor and update sp.
  BuildGenericJniFrameVisitor visitor(self, called->IsStatic(), shorty, shorty_len, &sp);
  visitor.VisitArguments();
  visitor.FinalizeHandleScope(self);

  // Fix up managed-stack things in Thread.
  self->SetTopOfStack(sp);

  self->VerifyStack();

  // Start JNI, save the cookie.
  uint32_t cookie;
  if (called->IsSynchronized()) {
    cookie = JniMethodStartSynchronized(visitor.GetFirstHandleScopeJObject(), self);
    if (self->IsExceptionPending()) {
      self->PopHandleScope();
      // A negative value denotes an error.
      return GetTwoWordFailureValue();
    }
  } else {
    cookie = JniMethodStart(self);
  }
  uint32_t* sp32 = reinterpret_cast<uint32_t*>(sp);
  *(sp32 - 1) = cookie;

  // Retrieve the stored native code.
  void* nativeCode = called->GetEntryPointFromJni();

  // There are two cases for the content of nativeCode:
  // 1) Pointer to the native function.
  // 2) Pointer to the trampoline for native code binding.
  // In the second case, we need to execute the binding and continue with the actual native function
  // pointer.
  DCHECK(nativeCode != nullptr);
  if (nativeCode == GetJniDlsymLookupStub()) {
#if defined(__arm__) || defined(__aarch64__)
    nativeCode = artFindNativeMethod();
#else
    nativeCode = artFindNativeMethod(self);
#endif

    if (nativeCode == nullptr) {
      DCHECK(self->IsExceptionPending());    // There should be an exception pending now.

      // End JNI, as the assembly will move to deliver the exception.
      jobject lock = called->IsSynchronized() ? visitor.GetFirstHandleScopeJObject() : nullptr;
      if (shorty[0] == 'L') {
        artQuickGenericJniEndJNIRef(self, cookie, nullptr, lock);
      } else {
        artQuickGenericJniEndJNINonRef(self, cookie, lock);
      }

      return GetTwoWordFailureValue();
    }
    // Note that the native code pointer will be automatically set by artFindNativeMethod().
  }

  // Return native code addr(lo) and bottom of alloca address(hi).
  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(visitor.GetBottomOfUsedArea()),
                                reinterpret_cast<uintptr_t>(nativeCode));
}

// Defined in quick_jni_entrypoints.cc.
extern uint64_t GenericJniMethodEnd(Thread* self, uint32_t saved_local_ref_cookie,
                                    jvalue result, uint64_t result_f, ArtMethod* called,
                                    HandleScope* handle_scope);
/*
 * Is called after the native JNI code. Responsible for cleanup (handle scope, saved state) and
 * unlocking.
 */
extern "C" uint64_t artQuickGenericJniEndTrampoline(Thread* self,
                                                    jvalue result,
                                                    uint64_t result_f) {
  // We're here just back from a native call. We don't have the shared mutator lock at this point
  // yet until we call GoToRunnable() later in GenericJniMethodEnd(). Accessing objects or doing
  // anything that requires a mutator lock before that would cause problems as GC may have the
  // exclusive mutator lock and may be moving objects, etc.
  ArtMethod** sp = self->GetManagedStack()->GetTopQuickFrame();
  uint32_t* sp32 = reinterpret_cast<uint32_t*>(sp);
  ArtMethod* called = *sp;
  uint32_t cookie = *(sp32 - 1);
  HandleScope* table = reinterpret_cast<HandleScope*>(reinterpret_cast<uint8_t*>(sp) + sizeof(*sp));
  return GenericJniMethodEnd(self, cookie, result, result_f, called, table);
}

// We use TwoWordReturn to optimize scalar returns. We use the hi value for code, and the lo value
// for the method pointer.
//
// It is valid to use this, as at the usage points here (returns from C functions) we are assuming
// to hold the mutator lock (see SHARED_REQUIRES(Locks::mutator_lock_) annotations).

template<InvokeType type, bool access_check>
static TwoWordReturn artInvokeCommon(uint32_t method_idx, mirror::Object* this_object, Thread* self,
                                     ArtMethod** sp) {
  ScopedQuickEntrypointChecks sqec(self);
  DCHECK_EQ(*sp, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs));
  ArtMethod* caller_method = QuickArgumentVisitor::GetCallingMethod(sp);
  ArtMethod* method = FindMethodFast(method_idx, this_object, caller_method, access_check, type);
  if (UNLIKELY(method == nullptr)) {
    const DexFile* dex_file = caller_method->GetDeclaringClass()->GetDexCache()->GetDexFile();
    uint32_t shorty_len;
    const char* shorty = dex_file->GetMethodShorty(dex_file->GetMethodId(method_idx), &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, type == kStatic, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<type, access_check>(method_idx, &this_object, caller_method,
                                                      self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == nullptr)) {
      CHECK(self->IsExceptionPending());
      return GetTwoWordFailureValue();  // Failure.
    }
  }
  DCHECK(!self->IsExceptionPending());
  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was null in method: " << PrettyMethod(method)
                          << " location: "
                          << method->GetDexFile()->GetLocation();

  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(code),
                                reinterpret_cast<uintptr_t>(method));
}

// Explicit artInvokeCommon template function declarations to please analysis tool.
#define EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(type, access_check)                                \
  template SHARED_REQUIRES(Locks::mutator_lock_)                                          \
  TwoWordReturn artInvokeCommon<type, access_check>(                                            \
      uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)

EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kVirtual, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kVirtual, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kInterface, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kInterface, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kDirect, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kDirect, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kStatic, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kStatic, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kSuper, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kSuper, true);
#undef EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL

// See comments in runtime_support_asm.S
extern "C" TwoWordReturn artInvokeInterfaceTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return artInvokeCommon<kInterface, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeDirectTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return artInvokeCommon<kDirect, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeStaticTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return artInvokeCommon<kStatic, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeSuperTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return artInvokeCommon<kSuper, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeVirtualTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return artInvokeCommon<kVirtual, true>(method_idx, this_object, self, sp);
}

// Determine target of interface dispatch. This object is known non-null. First argument
// is there for consistency but should not be used, as some architectures overwrite it
// in the assembly trampoline.
extern "C" TwoWordReturn artInvokeInterfaceTrampoline(uint32_t deadbeef ATTRIBUTE_UNUSED,
                                                      mirror::Object* this_object,
                                                      Thread* self,
                                                      ArtMethod** sp)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> cls(hs.NewHandle(this_object->GetClass()));

  // The optimizing compiler currently does not inline methods that have an interface
  // invocation. We use the outer method directly to avoid fetching a stack map, which is
  // more expensive.
  ArtMethod* caller_method = QuickArgumentVisitor::GetOuterMethod(sp);
  DCHECK_EQ(caller_method, QuickArgumentVisitor::GetCallingMethod(sp));

  // Fetch the dex_method_idx of the target interface method from the caller.
  uint32_t dex_pc = QuickArgumentVisitor::GetCallingDexPc(sp);

  const DexFile::CodeItem* code_item = caller_method->GetCodeItem();
  CHECK_LT(dex_pc, code_item->insns_size_in_code_units_);
  const Instruction* instr = Instruction::At(&code_item->insns_[dex_pc]);
  Instruction::Code instr_code = instr->Opcode();
  CHECK(instr_code == Instruction::INVOKE_INTERFACE ||
        instr_code == Instruction::INVOKE_INTERFACE_RANGE)
      << "Unexpected call into interface trampoline: " << instr->DumpString(nullptr);
  uint32_t dex_method_idx;
  if (instr_code == Instruction::INVOKE_INTERFACE) {
    dex_method_idx = instr->VRegB_35c();
  } else {
    CHECK_EQ(instr_code, Instruction::INVOKE_INTERFACE_RANGE);
    dex_method_idx = instr->VRegB_3rc();
  }

  ArtMethod* interface_method = caller_method->GetDexCacheResolvedMethod(
      dex_method_idx, sizeof(void*));
  DCHECK(interface_method != nullptr) << dex_method_idx << " " << PrettyMethod(caller_method);
  ArtMethod* method = nullptr;
  ImTable* imt = cls->GetImt(sizeof(void*));

  if (LIKELY(interface_method->GetDexMethodIndex() != DexFile::kDexNoIndex)) {
    // If the dex cache already resolved the interface method, look whether we have
    // a match in the ImtConflictTable.
    uint32_t imt_index = interface_method->GetDexMethodIndex();
    ArtMethod* conflict_method = imt->Get(imt_index % ImTable::kSize, sizeof(void*));
    if (LIKELY(conflict_method->IsRuntimeMethod())) {
      ImtConflictTable* current_table = conflict_method->GetImtConflictTable(sizeof(void*));
      DCHECK(current_table != nullptr);
      method = current_table->Lookup(interface_method, sizeof(void*));
    } else {
      // It seems we aren't really a conflict method!
      method = cls->FindVirtualMethodForInterface(interface_method, sizeof(void*));
    }
    if (method != nullptr) {
      return GetTwoWordSuccessValue(
          reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCode()),
          reinterpret_cast<uintptr_t>(method));
    }

    // No match, use the IfTable.
    method = cls->FindVirtualMethodForInterface(interface_method, sizeof(void*));
    if (UNLIKELY(method == nullptr)) {
      ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(
          interface_method, this_object, caller_method);
      return GetTwoWordFailureValue();  // Failure.
    }
  } else {
    // The dex cache did not resolve the method, look it up in the dex file
    // of the caller,
    DCHECK_EQ(interface_method, Runtime::Current()->GetResolutionMethod());
    const DexFile* dex_file = caller_method->GetDeclaringClass()->GetDexCache()
        ->GetDexFile();
    uint32_t shorty_len;
    const char* shorty = dex_file->GetMethodShorty(dex_file->GetMethodId(dex_method_idx),
                                                   &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, false, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<kInterface, false>(dex_method_idx, &this_object, caller_method,
                                                     self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == nullptr)) {
      CHECK(self->IsExceptionPending());
      return GetTwoWordFailureValue();  // Failure.
    }
    interface_method = caller_method->GetDexCacheResolvedMethod(dex_method_idx, sizeof(void*));
    DCHECK(!interface_method->IsRuntimeMethod());
  }

  // We arrive here if we have found an implementation, and it is not in the ImtConflictTable.
  // We create a new table with the new pair { interface_method, method }.
  uint32_t imt_index = interface_method->GetDexMethodIndex();
  ArtMethod* conflict_method = imt->Get(imt_index % ImTable::kSize, sizeof(void*));
  if (conflict_method->IsRuntimeMethod()) {
    ArtMethod* new_conflict_method = Runtime::Current()->GetClassLinker()->AddMethodToConflictTable(
        cls.Get(),
        conflict_method,
        interface_method,
        method,
        /*force_new_conflict_method*/false);
    if (new_conflict_method != conflict_method) {
      // Update the IMT if we create a new conflict method. No fence needed here, as the
      // data is consistent.
      imt->Set(imt_index % ImTable::kSize,
               new_conflict_method,
               sizeof(void*));
    }
  }

  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was null in method: " << PrettyMethod(method)
                          << " location: " << method->GetDexFile()->GetLocation();

  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(code),
                                reinterpret_cast<uintptr_t>(method));
}

}  // namespace art
