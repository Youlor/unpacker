/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "dex_instruction-inl.h"

#include <inttypes.h>

#include <iomanip>
#include <sstream>

#include "base/stringprintf.h"
#include "dex_file-inl.h"
#include "utils.h"

namespace art {

const char* const Instruction::kInstructionNames[] = {
#define INSTRUCTION_NAME(o, c, pname, f, r, i, a, v) pname,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_NAME)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_NAME
};

Instruction::Format const Instruction::kInstructionFormats[] = {
#define INSTRUCTION_FORMAT(o, c, p, format, r, i, a, v) format,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_FORMAT)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FORMAT
};

Instruction::IndexType const Instruction::kInstructionIndexTypes[] = {
#define INSTRUCTION_INDEX_TYPE(o, c, p, f, r, index, a, v) index,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_INDEX_TYPE)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FLAGS
};

int const Instruction::kInstructionFlags[] = {
#define INSTRUCTION_FLAGS(o, c, p, f, r, i, flags, v) flags,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_FLAGS)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_FLAGS
};

int const Instruction::kInstructionVerifyFlags[] = {
#define INSTRUCTION_VERIFY_FLAGS(o, c, p, f, r, i, a, vflags) vflags,
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_VERIFY_FLAGS)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_VERIFY_FLAGS
};

int const Instruction::kInstructionSizeInCodeUnits[] = {
#define INSTRUCTION_SIZE(opcode, c, p, format, r, i, a, v) \
    ((opcode == NOP)                        ? -1 : \
     ((format >= k10x) && (format <= k10t)) ?  1 : \
     ((format >= k20t) && (format <= k25x)) ?  2 : \
     ((format >= k32x) && (format <= k3rc)) ?  3 : \
      (format == k51l)                      ?  5 : -1),
#include "dex_instruction_list.h"
  DEX_INSTRUCTION_LIST(INSTRUCTION_SIZE)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_SIZE
};

int32_t Instruction::GetTargetOffset() const {
  switch (FormatOf(Opcode())) {
    // Cases for conditional branches follow.
    case k22t: return VRegC_22t();
    case k21t: return VRegB_21t();
    // Cases for unconditional branches follow.
    case k10t: return VRegA_10t();
    case k20t: return VRegA_20t();
    case k30t: return VRegA_30t();
    default: LOG(FATAL) << "Tried to access the branch offset of an instruction " << Name() <<
        " which does not have a target operand.";
  }
  return 0;
}

bool Instruction::CanFlowThrough() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  uint16_t insn = *insns;
  Code opcode = static_cast<Code>(insn & 0xFF);
  return  FlagsOf(opcode) & Instruction::kContinue;
}

size_t Instruction::SizeInCodeUnitsComplexOpcode() const {
  const uint16_t* insns = reinterpret_cast<const uint16_t*>(this);
  // Handle special NOP encoded variable length sequences.
  switch (*insns) {
    case kPackedSwitchSignature:
      return (4 + insns[1] * 2);
    case kSparseSwitchSignature:
      return (2 + insns[1] * 4);
    case kArrayDataSignature: {
      uint16_t element_size = insns[1];
      uint32_t length = insns[2] | (((uint32_t)insns[3]) << 16);
      // The plus 1 is to round up for odd size and width.
      return (4 + (element_size * length + 1) / 2);
    }
    default:
      if ((*insns & 0xFF) == 0) {
        return 1;  // NOP.
      } else {
        LOG(FATAL) << "Unreachable: " << DumpString(nullptr);
        UNREACHABLE();
      }
  }
}

std::string Instruction::DumpHex(size_t code_units) const {
  size_t inst_length = SizeInCodeUnits();
  if (inst_length > code_units) {
    inst_length = code_units;
  }
  std::ostringstream os;
  const uint16_t* insn = reinterpret_cast<const uint16_t*>(this);
  for (size_t i = 0; i < inst_length; i++) {
    os << StringPrintf("0x%04x", insn[i]) << " ";
  }
  for (size_t i = inst_length; i < code_units; i++) {
    os << "       ";
  }
  return os.str();
}

std::string Instruction::DumpHexLE(size_t instr_code_units) const {
  size_t inst_length = SizeInCodeUnits();
  if (inst_length > instr_code_units) {
    inst_length = instr_code_units;
  }
  std::ostringstream os;
  const uint16_t* insn = reinterpret_cast<const uint16_t*>(this);
  for (size_t i = 0; i < inst_length; i++) {
    os << StringPrintf("%02x%02x", static_cast<uint8_t>(insn[i] & 0x00FF),
                       static_cast<uint8_t>((insn[i] & 0xFF00) >> 8)) << " ";
  }
  for (size_t i = inst_length; i < instr_code_units; i++) {
    os << "     ";
  }
  return os.str();
}

std::string Instruction::DumpString(const DexFile* file) const {
  std::ostringstream os;
  const char* opcode = kInstructionNames[Opcode()];
  switch (FormatOf(Opcode())) {
    case k10x:  os << opcode; break;
    case k12x:  os << StringPrintf("%s v%d, v%d", opcode, VRegA_12x(), VRegB_12x()); break;
    case k11n:  os << StringPrintf("%s v%d, #%+d", opcode, VRegA_11n(), VRegB_11n()); break;
    case k11x:  os << StringPrintf("%s v%d", opcode, VRegA_11x()); break;
    case k10t:  os << StringPrintf("%s %+d", opcode, VRegA_10t()); break;
    case k20t:  os << StringPrintf("%s %+d", opcode, VRegA_20t()); break;
    case k22x:  os << StringPrintf("%s v%d, v%d", opcode, VRegA_22x(), VRegB_22x()); break;
    case k21t:  os << StringPrintf("%s v%d, %+d", opcode, VRegA_21t(), VRegB_21t()); break;
    case k21s:  os << StringPrintf("%s v%d, #%+d", opcode, VRegA_21s(), VRegB_21s()); break;
    case k21h: {
        // op vAA, #+BBBB0000[00000000]
        if (Opcode() == CONST_HIGH16) {
          uint32_t value = VRegB_21h() << 16;
          os << StringPrintf("%s v%d, #int %+d // 0x%x", opcode, VRegA_21h(), value, value);
        } else {
          uint64_t value = static_cast<uint64_t>(VRegB_21h()) << 48;
          os << StringPrintf("%s v%d, #long %+" PRId64 " // 0x%" PRIx64, opcode, VRegA_21h(),
                             value, value);
        }
      }
      break;
    case k21c: {
      switch (Opcode()) {
        case CONST_STRING:
          if (file != nullptr) {
            uint32_t string_idx = VRegB_21c();
            if (string_idx < file->NumStringIds()) {
              os << StringPrintf("const-string v%d, %s // string@%d",
                                 VRegA_21c(),
                                 PrintableString(file->StringDataByIdx(string_idx)).c_str(),
                                 string_idx);
            } else {
              os << StringPrintf("const-string v%d, <<invalid-string-idx-%d>> // string@%d",
                                 VRegA_21c(),
                                 string_idx,
                                 string_idx);
            }
            break;
          }
          FALLTHROUGH_INTENDED;
        case CHECK_CAST:
        case CONST_CLASS:
        case NEW_INSTANCE:
          if (file != nullptr) {
            uint32_t type_idx = VRegB_21c();
            os << opcode << " v" << static_cast<int>(VRegA_21c()) << ", " << PrettyType(type_idx, *file)
               << " // type@" << type_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case SGET:
        case SGET_WIDE:
        case SGET_OBJECT:
        case SGET_BOOLEAN:
        case SGET_BYTE:
        case SGET_CHAR:
        case SGET_SHORT:
          if (file != nullptr) {
            uint32_t field_idx = VRegB_21c();
            os << opcode << "  v" << static_cast<int>(VRegA_21c()) << ", " << PrettyField(field_idx, *file, true)
               << " // field@" << field_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case SPUT:
        case SPUT_WIDE:
        case SPUT_OBJECT:
        case SPUT_BOOLEAN:
        case SPUT_BYTE:
        case SPUT_CHAR:
        case SPUT_SHORT:
          if (file != nullptr) {
            uint32_t field_idx = VRegB_21c();
            os << opcode << " v" << static_cast<int>(VRegA_21c()) << ", " << PrettyField(field_idx, *file, true)
               << " // field@" << field_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case CREATE_LAMBDA:
          if (file != nullptr) {
            uint32_t method_idx = VRegB_21c();
            os << opcode << " v" << static_cast<int>(VRegA_21c()) << ", " << PrettyMethod(method_idx, *file, true)
               << " // method@" << method_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        default:
          os << StringPrintf("%s v%d, thing@%d", opcode, VRegA_21c(), VRegB_21c());
          break;
      }
      break;
    }
    case k23x:  os << StringPrintf("%s v%d, v%d, v%d", opcode, VRegA_23x(), VRegB_23x(), VRegC_23x()); break;
    case k22b:  os << StringPrintf("%s v%d, v%d, #%+d", opcode, VRegA_22b(), VRegB_22b(), VRegC_22b()); break;
    case k22t:  os << StringPrintf("%s v%d, v%d, %+d", opcode, VRegA_22t(), VRegB_22t(), VRegC_22t()); break;
    case k22s:  os << StringPrintf("%s v%d, v%d, #%+d", opcode, VRegA_22s(), VRegB_22s(), VRegC_22s()); break;
    case k22c: {
      switch (Opcode()) {
        case IGET:
        case IGET_WIDE:
        case IGET_OBJECT:
        case IGET_BOOLEAN:
        case IGET_BYTE:
        case IGET_CHAR:
        case IGET_SHORT:
          if (file != nullptr) {
            uint32_t field_idx = VRegC_22c();
            os << opcode << " v" << static_cast<int>(VRegA_22c()) << ", v" << static_cast<int>(VRegB_22c()) << ", "
               << PrettyField(field_idx, *file, true) << " // field@" << field_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case IGET_QUICK:
        case IGET_OBJECT_QUICK:
          if (file != nullptr) {
            uint32_t field_idx = VRegC_22c();
            os << opcode << " v" << static_cast<int>(VRegA_22c()) << ", v" << static_cast<int>(VRegB_22c()) << ", "
               << "// offset@" << field_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case IPUT:
        case IPUT_WIDE:
        case IPUT_OBJECT:
        case IPUT_BOOLEAN:
        case IPUT_BYTE:
        case IPUT_CHAR:
        case IPUT_SHORT:
          if (file != nullptr) {
            uint32_t field_idx = VRegC_22c();
            os << opcode << " v" << static_cast<int>(VRegA_22c()) << ", v" << static_cast<int>(VRegB_22c()) << ", "
               << PrettyField(field_idx, *file, true) << " // field@" << field_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case IPUT_QUICK:
        case IPUT_OBJECT_QUICK:
          if (file != nullptr) {
            uint32_t field_idx = VRegC_22c();
            os << opcode << " v" << static_cast<int>(VRegA_22c()) << ", v" << static_cast<int>(VRegB_22c()) << ", "
               << "// offset@" << field_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case INSTANCE_OF:
          if (file != nullptr) {
            uint32_t type_idx = VRegC_22c();
            os << opcode << " v" << static_cast<int>(VRegA_22c()) << ", v" << static_cast<int>(VRegB_22c()) << ", "
               << PrettyType(type_idx, *file) << " // type@" << type_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case NEW_ARRAY:
          if (file != nullptr) {
            uint32_t type_idx = VRegC_22c();
            os << opcode << " v" << static_cast<int>(VRegA_22c()) << ", v" << static_cast<int>(VRegB_22c()) << ", "
               << PrettyType(type_idx, *file) << " // type@" << type_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        default:
          os << StringPrintf("%s v%d, v%d, thing@%d", opcode, VRegA_22c(), VRegB_22c(), VRegC_22c());
          break;
      }
      break;
    }
    case k25x: {
      if (Opcode() == INVOKE_LAMBDA) {
        uint32_t arg[kMaxVarArgRegs25x];
        GetAllArgs25x(arg);
        const size_t num_extra_var_args = VRegB_25x();
        DCHECK_LE(num_extra_var_args + 2, arraysize(arg));

        // invoke-lambda vC, {vD, vE, vF, vG}
        os << opcode << " v" << arg[0] << ", {";
        for (size_t i = 0; i < num_extra_var_args; ++i) {
          if (i != 0) {
            os << ", ";
          }
          os << "v" << arg[i + 2];  // Don't print the pair of vC registers. Pair is implicit.
        }
        os << "}";
        break;
      }
      FALLTHROUGH_INTENDED;
    }
    case k32x:  os << StringPrintf("%s v%d, v%d", opcode, VRegA_32x(), VRegB_32x()); break;
    case k30t:  os << StringPrintf("%s %+d", opcode, VRegA_30t()); break;
    case k31t:  os << StringPrintf("%s v%d, %+d", opcode, VRegA_31t(), VRegB_31t()); break;
    case k31i:  os << StringPrintf("%s v%d, #%+d", opcode, VRegA_31i(), VRegB_31i()); break;
    case k31c:
      if (Opcode() == CONST_STRING_JUMBO) {
        uint32_t string_idx = VRegB_31c();
        if (file != nullptr) {
          if (string_idx < file->NumStringIds()) {
            os << StringPrintf("%s v%d, %s // string@%d",
                               opcode,
                               VRegA_31c(),
                               PrintableString(file->StringDataByIdx(string_idx)).c_str(),
                               string_idx);
          } else {
            os << StringPrintf("%s v%d, <<invalid-string-idx-%d>> // string@%d",
                               opcode,
                               VRegA_31c(),
                               string_idx,
                               string_idx);
          }
        } else {
          os << StringPrintf("%s v%d, string@%d", opcode, VRegA_31c(), string_idx);
        }
      } else {
        os << StringPrintf("%s v%d, thing@%d", opcode, VRegA_31c(), VRegB_31c()); break;
      }
      break;
    case k35c: {
      uint32_t arg[5];
      GetVarArgs(arg);
      switch (Opcode()) {
        case FILLED_NEW_ARRAY:
        {
          const int32_t a = VRegA_35c();
          os << opcode << " {";
          for (int i = 0; i < a; ++i) {
            if (i > 0) {
              os << ", ";
            }
            os << "v" << arg[i];
          }
          os << "}, type@" << VRegB_35c();
        }
        break;

        case INVOKE_VIRTUAL:
        case INVOKE_SUPER:
        case INVOKE_DIRECT:
        case INVOKE_STATIC:
        case INVOKE_INTERFACE:
          if (file != nullptr) {
            os << opcode << " {";
            uint32_t method_idx = VRegB_35c();
            for (size_t i = 0; i < VRegA_35c(); ++i) {
              if (i != 0) {
                os << ", ";
              }
              os << "v" << arg[i];
            }
            os << "}, " << PrettyMethod(method_idx, *file) << " // method@" << method_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case INVOKE_VIRTUAL_QUICK:
          if (file != nullptr) {
            os << opcode << " {";
            uint32_t method_idx = VRegB_35c();
            for (size_t i = 0; i < VRegA_35c(); ++i) {
              if (i != 0) {
                os << ", ";
              }
              os << "v" << arg[i];
            }
            os << "},  // vtable@" << method_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        default:
          os << opcode << " {v" << arg[0] << ", v" << arg[1] << ", v" << arg[2]
                       << ", v" << arg[3] << ", v" << arg[4] << "}, thing@" << VRegB_35c();
          break;
      }
      break;
    }
    case k3rc: {
      switch (Opcode()) {
        case INVOKE_VIRTUAL_RANGE:
        case INVOKE_SUPER_RANGE:
        case INVOKE_DIRECT_RANGE:
        case INVOKE_STATIC_RANGE:
        case INVOKE_INTERFACE_RANGE:
          if (file != nullptr) {
            uint32_t method_idx = VRegB_3rc();
            os << StringPrintf("%s, {v%d .. v%d}, ", opcode, VRegC_3rc(), (VRegC_3rc() + VRegA_3rc() - 1))
               << PrettyMethod(method_idx, *file) << " // method@" << method_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        case INVOKE_VIRTUAL_RANGE_QUICK:
          if (file != nullptr) {
            uint32_t method_idx = VRegB_3rc();
            os << StringPrintf("%s, {v%d .. v%d}, ", opcode, VRegC_3rc(), (VRegC_3rc() + VRegA_3rc() - 1))
               << "// vtable@" << method_idx;
            break;
          }
          FALLTHROUGH_INTENDED;
        default:
          os << StringPrintf("%s, {v%d .. v%d}, thing@%d", opcode, VRegC_3rc(),
                             (VRegC_3rc() + VRegA_3rc() - 1), VRegB_3rc());
          break;
      }
      break;
    }
    case k51l: os << StringPrintf("%s v%d, #%+" PRId64, opcode, VRegA_51l(), VRegB_51l()); break;
    default: os << " unknown format (" << DumpHex(5) << ")"; break;
  }
  return os.str();
}

std::ostream& operator<<(std::ostream& os, const Instruction::Code& code) {
  return os << Instruction::Name(code);
}

}  // namespace art
