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

#include "register_line.h"

#include "base/stringprintf.h"
#include "dex_instruction-inl.h"
#include "method_verifier-inl.h"
#include "register_line-inl.h"
#include "reg_type-inl.h"

namespace art {
namespace verifier {

bool RegisterLine::CheckConstructorReturn(MethodVerifier* verifier) const {
  if (kIsDebugBuild && this_initialized_) {
    // Ensure that there is no UninitializedThisReference type anymore if this_initialized_ is true.
    for (size_t i = 0; i < num_regs_; i++) {
      const RegType& type = GetRegisterType(verifier, i);
      CHECK(!type.IsUninitializedThisReference() &&
            !type.IsUnresolvedAndUninitializedThisReference())
          << i << ": " << type.IsUninitializedThisReference() << " in "
          << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                          *verifier->GetMethodReference().dex_file);
    }
  }
  if (!this_initialized_) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "Constructor returning without calling superclass constructor";
  }
  return this_initialized_;
}

const RegType& RegisterLine::GetInvocationThis(MethodVerifier* verifier, const Instruction* inst,
                                               bool is_range, bool allow_failure) {
  const size_t args_count = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  if (args_count < 1) {
    if (!allow_failure) {
      verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invoke lacks 'this'";
    }
    return verifier->GetRegTypeCache()->Conflict();
  }
  /* Get the element type of the array held in vsrc */
  const uint32_t this_reg = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
  const RegType& this_type = GetRegisterType(verifier, this_reg);
  if (!this_type.IsReferenceTypes()) {
    if (!allow_failure) {
      verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD)
          << "tried to get class from non-reference register v" << this_reg
          << " (type=" << this_type << ")";
    }
    return verifier->GetRegTypeCache()->Conflict();
  }
  return this_type;
}

bool RegisterLine::VerifyRegisterTypeWide(MethodVerifier* verifier, uint32_t vsrc,
                                          const RegType& check_type1,
                                          const RegType& check_type2) {
  DCHECK(check_type1.CheckWidePair(check_type2));
  // Verify the src register type against the check type refining the type of the register
  const RegType& src_type = GetRegisterType(verifier, vsrc);
  if (!check_type1.IsAssignableFrom(src_type)) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "register v" << vsrc << " has type " << src_type
                               << " but expected " << check_type1;
    return false;
  }
  const RegType& src_type_h = GetRegisterType(verifier, vsrc + 1);
  if (!src_type.CheckWidePair(src_type_h)) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "wide register v" << vsrc << " has type "
        << src_type << "/" << src_type_h;
    return false;
  }
  // The register at vsrc has a defined type, we know the lower-upper-bound, but this is less
  // precise than the subtype in vsrc so leave it for reference types. For primitive types
  // if they are a defined type then they are as precise as we can get, however, for constant
  // types we may wish to refine them. Unfortunately constant propagation has rendered this useless.
  return true;
}

void RegisterLine::MarkRefsAsInitialized(MethodVerifier* verifier, const RegType& uninit_type) {
  DCHECK(uninit_type.IsUninitializedTypes());
  const RegType& init_type = verifier->GetRegTypeCache()->FromUninitialized(uninit_type);
  size_t changed = 0;
  for (uint32_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(verifier, i).Equals(uninit_type)) {
      line_[i] = init_type.GetId();
      changed++;
    }
  }
  // Is this initializing "this"?
  if (uninit_type.IsUninitializedThisReference() ||
      uninit_type.IsUnresolvedAndUninitializedThisReference()) {
    this_initialized_ = true;
  }
  DCHECK_GT(changed, 0u);
}

void RegisterLine::MarkAllRegistersAsConflicts(MethodVerifier* verifier) {
  uint16_t conflict_type_id = verifier->GetRegTypeCache()->Conflict().GetId();
  for (uint32_t i = 0; i < num_regs_; i++) {
    line_[i] = conflict_type_id;
  }
}

void RegisterLine::MarkAllRegistersAsConflictsExcept(MethodVerifier* verifier, uint32_t vsrc) {
  uint16_t conflict_type_id = verifier->GetRegTypeCache()->Conflict().GetId();
  for (uint32_t i = 0; i < num_regs_; i++) {
    if (i != vsrc) {
      line_[i] = conflict_type_id;
    }
  }
}

void RegisterLine::MarkAllRegistersAsConflictsExceptWide(MethodVerifier* verifier, uint32_t vsrc) {
  uint16_t conflict_type_id = verifier->GetRegTypeCache()->Conflict().GetId();
  for (uint32_t i = 0; i < num_regs_; i++) {
    if ((i != vsrc) && (i != (vsrc + 1))) {
      line_[i] = conflict_type_id;
    }
  }
}

std::string RegisterLine::Dump(MethodVerifier* verifier) const {
  std::string result;
  for (size_t i = 0; i < num_regs_; i++) {
    result += StringPrintf("%zd:[", i);
    result += GetRegisterType(verifier, i).Dump();
    result += "],";
  }
  for (const auto& monitor : monitors_) {
    result += StringPrintf("{%d},", monitor);
  }
  for (auto& pairs : reg_to_lock_depths_) {
    result += StringPrintf("<%d -> %x>", pairs.first, pairs.second);
  }
  return result;
}

void RegisterLine::MarkUninitRefsAsInvalid(MethodVerifier* verifier, const RegType& uninit_type) {
  for (size_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(verifier, i).Equals(uninit_type)) {
      line_[i] = verifier->GetRegTypeCache()->Conflict().GetId();
      ClearAllRegToLockDepths(i);
    }
  }
}

void RegisterLine::CopyResultRegister1(MethodVerifier* verifier, uint32_t vdst, bool is_reference) {
  const RegType& type = verifier->GetRegTypeCache()->GetFromId(result_[0]);
  if ((!is_reference && !type.IsCategory1Types()) ||
      (is_reference && !type.IsReferenceTypes())) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "copyRes1 v" << vdst << "<- result0"  << " type=" << type;
  } else {
    DCHECK(verifier->GetRegTypeCache()->GetFromId(result_[1]).IsUndefined());
    SetRegisterType<LockOp::kClear>(verifier, vdst, type);
    result_[0] = verifier->GetRegTypeCache()->Undefined().GetId();
  }
}

/*
 * Implement "move-result-wide". Copy the category-2 value from the result
 * register to another register, and reset the result register.
 */
void RegisterLine::CopyResultRegister2(MethodVerifier* verifier, uint32_t vdst) {
  const RegType& type_l = verifier->GetRegTypeCache()->GetFromId(result_[0]);
  const RegType& type_h = verifier->GetRegTypeCache()->GetFromId(result_[1]);
  if (!type_l.IsCategory2Types()) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "copyRes2 v" << vdst << "<- result0"  << " type=" << type_l;
  } else {
    DCHECK(type_l.CheckWidePair(type_h));  // Set should never allow this case
    SetRegisterTypeWide(verifier, vdst, type_l, type_h);  // also sets the high
    result_[0] = verifier->GetRegTypeCache()->Undefined().GetId();
    result_[1] = verifier->GetRegTypeCache()->Undefined().GetId();
  }
}

void RegisterLine::CheckUnaryOp(MethodVerifier* verifier, const Instruction* inst,
                                const RegType& dst_type, const RegType& src_type) {
  if (VerifyRegisterType(verifier, inst->VRegB_12x(), src_type)) {
    SetRegisterType<LockOp::kClear>(verifier, inst->VRegA_12x(), dst_type);
  }
}

void RegisterLine::CheckUnaryOpWide(MethodVerifier* verifier, const Instruction* inst,
                                    const RegType& dst_type1, const RegType& dst_type2,
                                    const RegType& src_type1, const RegType& src_type2) {
  if (VerifyRegisterTypeWide(verifier, inst->VRegB_12x(), src_type1, src_type2)) {
    SetRegisterTypeWide(verifier, inst->VRegA_12x(), dst_type1, dst_type2);
  }
}

void RegisterLine::CheckUnaryOpToWide(MethodVerifier* verifier, const Instruction* inst,
                                      const RegType& dst_type1, const RegType& dst_type2,
                                      const RegType& src_type) {
  if (VerifyRegisterType(verifier, inst->VRegB_12x(), src_type)) {
    SetRegisterTypeWide(verifier, inst->VRegA_12x(), dst_type1, dst_type2);
  }
}

void RegisterLine::CheckUnaryOpFromWide(MethodVerifier* verifier, const Instruction* inst,
                                        const RegType& dst_type,
                                        const RegType& src_type1, const RegType& src_type2) {
  if (VerifyRegisterTypeWide(verifier, inst->VRegB_12x(), src_type1, src_type2)) {
    SetRegisterType<LockOp::kClear>(verifier, inst->VRegA_12x(), dst_type);
  }
}

void RegisterLine::CheckBinaryOp(MethodVerifier* verifier, const Instruction* inst,
                                 const RegType& dst_type,
                                 const RegType& src_type1, const RegType& src_type2,
                                 bool check_boolean_op) {
  const uint32_t vregB = inst->VRegB_23x();
  const uint32_t vregC = inst->VRegC_23x();
  if (VerifyRegisterType(verifier, vregB, src_type1) &&
      VerifyRegisterType(verifier, vregC, src_type2)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      if (GetRegisterType(verifier, vregB).IsBooleanTypes() &&
          GetRegisterType(verifier, vregC).IsBooleanTypes()) {
        SetRegisterType<LockOp::kClear>(verifier,
                                        inst->VRegA_23x(),
                                        verifier->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType<LockOp::kClear>(verifier, inst->VRegA_23x(), dst_type);
  }
}

void RegisterLine::CheckBinaryOpWide(MethodVerifier* verifier, const Instruction* inst,
                                     const RegType& dst_type1, const RegType& dst_type2,
                                     const RegType& src_type1_1, const RegType& src_type1_2,
                                     const RegType& src_type2_1, const RegType& src_type2_2) {
  if (VerifyRegisterTypeWide(verifier, inst->VRegB_23x(), src_type1_1, src_type1_2) &&
      VerifyRegisterTypeWide(verifier, inst->VRegC_23x(), src_type2_1, src_type2_2)) {
    SetRegisterTypeWide(verifier, inst->VRegA_23x(), dst_type1, dst_type2);
  }
}

void RegisterLine::CheckBinaryOpWideShift(MethodVerifier* verifier, const Instruction* inst,
                                          const RegType& long_lo_type, const RegType& long_hi_type,
                                          const RegType& int_type) {
  if (VerifyRegisterTypeWide(verifier, inst->VRegB_23x(), long_lo_type, long_hi_type) &&
      VerifyRegisterType(verifier, inst->VRegC_23x(), int_type)) {
    SetRegisterTypeWide(verifier, inst->VRegA_23x(), long_lo_type, long_hi_type);
  }
}

void RegisterLine::CheckBinaryOp2addr(MethodVerifier* verifier, const Instruction* inst,
                                      const RegType& dst_type, const RegType& src_type1,
                                      const RegType& src_type2, bool check_boolean_op) {
  const uint32_t vregA = inst->VRegA_12x();
  const uint32_t vregB = inst->VRegB_12x();
  if (VerifyRegisterType(verifier, vregA, src_type1) &&
      VerifyRegisterType(verifier, vregB, src_type2)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      if (GetRegisterType(verifier, vregA).IsBooleanTypes() &&
          GetRegisterType(verifier, vregB).IsBooleanTypes()) {
        SetRegisterType<LockOp::kClear>(verifier,
                                        vregA,
                                        verifier->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType<LockOp::kClear>(verifier, vregA, dst_type);
  }
}

void RegisterLine::CheckBinaryOp2addrWide(MethodVerifier* verifier, const Instruction* inst,
                                          const RegType& dst_type1, const RegType& dst_type2,
                                          const RegType& src_type1_1, const RegType& src_type1_2,
                                          const RegType& src_type2_1, const RegType& src_type2_2) {
  const uint32_t vregA = inst->VRegA_12x();
  const uint32_t vregB = inst->VRegB_12x();
  if (VerifyRegisterTypeWide(verifier, vregA, src_type1_1, src_type1_2) &&
      VerifyRegisterTypeWide(verifier, vregB, src_type2_1, src_type2_2)) {
    SetRegisterTypeWide(verifier, vregA, dst_type1, dst_type2);
  }
}

void RegisterLine::CheckBinaryOp2addrWideShift(MethodVerifier* verifier, const Instruction* inst,
                                               const RegType& long_lo_type, const RegType& long_hi_type,
                                               const RegType& int_type) {
  const uint32_t vregA = inst->VRegA_12x();
  const uint32_t vregB = inst->VRegB_12x();
  if (VerifyRegisterTypeWide(verifier, vregA, long_lo_type, long_hi_type) &&
      VerifyRegisterType(verifier, vregB, int_type)) {
    SetRegisterTypeWide(verifier, vregA, long_lo_type, long_hi_type);
  }
}

void RegisterLine::CheckLiteralOp(MethodVerifier* verifier, const Instruction* inst,
                                  const RegType& dst_type, const RegType& src_type,
                                  bool check_boolean_op, bool is_lit16) {
  const uint32_t vregA = is_lit16 ? inst->VRegA_22s() : inst->VRegA_22b();
  const uint32_t vregB = is_lit16 ? inst->VRegB_22s() : inst->VRegB_22b();
  if (VerifyRegisterType(verifier, vregB, src_type)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      /* check vB with the call, then check the constant manually */
      const uint32_t val = is_lit16 ? inst->VRegC_22s() : inst->VRegC_22b();
      if (GetRegisterType(verifier, vregB).IsBooleanTypes() && (val == 0 || val == 1)) {
        SetRegisterType<LockOp::kClear>(verifier,
                                        vregA,
                                        verifier->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType<LockOp::kClear>(verifier, vregA, dst_type);
  }
}

static constexpr uint32_t kVirtualNullRegister = std::numeric_limits<uint32_t>::max();

void RegisterLine::PushMonitor(MethodVerifier* verifier, uint32_t reg_idx, int32_t insn_idx) {
  const RegType& reg_type = GetRegisterType(verifier, reg_idx);
  if (!reg_type.IsReferenceTypes()) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "monitor-enter on non-object ("
        << reg_type << ")";
  } else if (monitors_.size() >= 32) {
    verifier->Fail(VERIFY_ERROR_LOCKING);
    if (kDumpLockFailures) {
      VLOG(verifier) << "monitor-enter stack overflow while verifying "
                     << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                     *verifier->GetMethodReference().dex_file);
    }
  } else {
    if (SetRegToLockDepth(reg_idx, monitors_.size())) {
      // Null literals can establish aliases that we can't easily track. As such, handle the zero
      // case as the 2^32-1 register (which isn't available in dex bytecode).
      if (reg_type.IsZero()) {
        SetRegToLockDepth(kVirtualNullRegister, monitors_.size());
      }

      monitors_.push_back(insn_idx);
    } else {
      verifier->Fail(VERIFY_ERROR_LOCKING);
      if (kDumpLockFailures) {
        VLOG(verifier) << "unexpected monitor-enter on register v" <<  reg_idx << " in "
                       << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                       *verifier->GetMethodReference().dex_file);
      }
    }
  }
}

void RegisterLine::PopMonitor(MethodVerifier* verifier, uint32_t reg_idx) {
  const RegType& reg_type = GetRegisterType(verifier, reg_idx);
  if (!reg_type.IsReferenceTypes()) {
    verifier->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "monitor-exit on non-object (" << reg_type << ")";
  } else if (monitors_.empty()) {
    verifier->Fail(VERIFY_ERROR_LOCKING);
    if (kDumpLockFailures) {
      VLOG(verifier) << "monitor-exit stack underflow while verifying "
                     << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                     *verifier->GetMethodReference().dex_file);
    }
  } else {
    monitors_.pop_back();

    bool success = IsSetLockDepth(reg_idx, monitors_.size());

    if (!success && reg_type.IsZero()) {
      // Null literals can establish aliases that we can't easily track. As such, handle the zero
      // case as the 2^32-1 register (which isn't available in dex bytecode).
      success = IsSetLockDepth(kVirtualNullRegister, monitors_.size());
      if (success) {
        reg_idx = kVirtualNullRegister;
      }
    }

    if (!success) {
      verifier->Fail(VERIFY_ERROR_LOCKING);
      if (kDumpLockFailures) {
        VLOG(verifier) << "monitor-exit not unlocking the top of the monitor stack while verifying "
                       << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                       *verifier->GetMethodReference().dex_file);
      }
    } else {
      // Record the register was unlocked. This clears all aliases, thus it will also clear the
      // null lock, if necessary.
      ClearRegToLockDepth(reg_idx, monitors_.size());
    }
  }
}

bool FindLockAliasedRegister(uint32_t src,
                             const RegisterLine::RegToLockDepthsMap& src_map,
                             const RegisterLine::RegToLockDepthsMap& search_map) {
  auto it = src_map.find(src);
  if (it == src_map.end()) {
    // "Not locked" is trivially aliased.
    return true;
  }
  uint32_t src_lock_levels = it->second;
  if (src_lock_levels == 0) {
    // "Not locked" is trivially aliased.
    return true;
  }

  // Scan the map for the same value.
  for (const std::pair<uint32_t, uint32_t>& pair : search_map) {
    if (pair.first != src && pair.second == src_lock_levels) {
      return true;
    }
  }

  // Nothing found, no alias.
  return false;
}

bool RegisterLine::MergeRegisters(MethodVerifier* verifier, const RegisterLine* incoming_line) {
  bool changed = false;
  DCHECK(incoming_line != nullptr);
  for (size_t idx = 0; idx < num_regs_; idx++) {
    if (line_[idx] != incoming_line->line_[idx]) {
      const RegType& incoming_reg_type = incoming_line->GetRegisterType(verifier, idx);
      const RegType& cur_type = GetRegisterType(verifier, idx);
      const RegType& new_type = cur_type.Merge(incoming_reg_type, verifier->GetRegTypeCache());
      changed = changed || !cur_type.Equals(new_type);
      line_[idx] = new_type.GetId();
    }
  }
  if (monitors_.size() > 0 || incoming_line->monitors_.size() > 0) {
    if (monitors_.size() != incoming_line->monitors_.size()) {
      verifier->Fail(VERIFY_ERROR_LOCKING);
      if (kDumpLockFailures) {
        VLOG(verifier) << "mismatched stack depths (depth=" << MonitorStackDepth()
                       << ", incoming depth=" << incoming_line->MonitorStackDepth() << ") in "
                       << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                       *verifier->GetMethodReference().dex_file);
      }
    } else if (reg_to_lock_depths_ != incoming_line->reg_to_lock_depths_) {
      for (uint32_t idx = 0; idx < num_regs_; idx++) {
        size_t depths = reg_to_lock_depths_.count(idx);
        size_t incoming_depths = incoming_line->reg_to_lock_depths_.count(idx);
        if (depths != incoming_depths) {
          // Stack levels aren't matching. This is potentially bad, as we don't do a
          // flow-sensitive analysis.
          // However, this could be an alias of something locked in one path, and the alias was
          // destroyed in another path. It is fine to drop this as long as there's another alias
          // for the lock around. The last vanishing alias will then report that things would be
          // left unlocked. We need to check for aliases for both lock levels.
          //
          // Example (lock status in curly braces as pair of register and lock leels):
          //
          //                            lock v1 {v1=1}
          //                        |                    |
          //              v0 = v1 {v0=1, v1=1}       v0 = v2 {v1=1}
          //                        |                    |
          //                                 {v1=1}
          //                                         // Dropping v0, as the status can't be merged
          //                                         // but the lock info ("locked at depth 1" and)
          //                                         // "not locked at all") is available.
          if (!FindLockAliasedRegister(idx,
                                       reg_to_lock_depths_,
                                       reg_to_lock_depths_) ||
              !FindLockAliasedRegister(idx,
                                       incoming_line->reg_to_lock_depths_,
                                       reg_to_lock_depths_)) {
            verifier->Fail(VERIFY_ERROR_LOCKING);
            if (kDumpLockFailures) {
              VLOG(verifier) << "mismatched stack depths for register v" << idx
                             << ": " << depths  << " != " << incoming_depths << " in "
                             << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                             *verifier->GetMethodReference().dex_file);
            }
            break;
          }
          // We found aliases, set this to zero.
          reg_to_lock_depths_.erase(idx);
        } else if (depths > 0) {
          // Check whether they're actually the same levels.
          uint32_t locked_levels = reg_to_lock_depths_.find(idx)->second;
          uint32_t incoming_locked_levels = incoming_line->reg_to_lock_depths_.find(idx)->second;
          if (locked_levels != incoming_locked_levels) {
            // Lock levels aren't matching. This is potentially bad, as we don't do a
            // flow-sensitive analysis.
            // However, this could be an alias of something locked in one path, and the alias was
            // destroyed in another path. It is fine to drop this as long as there's another alias
            // for the lock around. The last vanishing alias will then report that things would be
            // left unlocked. We need to check for aliases for both lock levels.
            //
            // Example (lock status in curly braces as pair of register and lock leels):
            //
            //                          lock v1 {v1=1}
            //                          lock v2 {v1=1, v2=2}
            //                        |                      |
            //         v0 = v1 {v0=1, v1=1, v2=2}  v0 = v2 {v0=2, v1=1, v2=2}
            //                        |                      |
            //                             {v1=1, v2=2}
            //                                           // Dropping v0, as the status can't be
            //                                           // merged but the lock info ("locked at
            //                                           // depth 1" and "locked at depth 2") is
            //                                           // available.
            if (!FindLockAliasedRegister(idx,
                                         reg_to_lock_depths_,
                                         reg_to_lock_depths_) ||
                !FindLockAliasedRegister(idx,
                                         incoming_line->reg_to_lock_depths_,
                                         reg_to_lock_depths_)) {
              // No aliases for both current and incoming, we'll lose information.
              verifier->Fail(VERIFY_ERROR_LOCKING);
              if (kDumpLockFailures) {
                VLOG(verifier) << "mismatched lock levels for register v" << idx << ": "
                               << std::hex << locked_levels << std::dec  << " != "
                               << std::hex << incoming_locked_levels << std::dec << " in "
                               << PrettyMethod(verifier->GetMethodReference().dex_method_index,
                                               *verifier->GetMethodReference().dex_file);
              }
              break;
            }
            // We found aliases, set this to zero.
            reg_to_lock_depths_.erase(idx);
          }
        }
      }
    }
  }

  // Check whether "this" was initialized in both paths.
  if (this_initialized_ && !incoming_line->this_initialized_) {
    this_initialized_ = false;
    changed = true;
  }
  return changed;
}

}  // namespace verifier
}  // namespace art
