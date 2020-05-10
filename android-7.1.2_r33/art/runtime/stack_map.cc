/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "stack_map.h"

#include <stdint.h>

#include "indenter.h"
#include "invoke_type.h"

namespace art {

constexpr size_t DexRegisterLocationCatalog::kNoLocationEntryIndex;
constexpr uint32_t StackMap::kNoDexRegisterMap;
constexpr uint32_t StackMap::kNoInlineInfo;

std::ostream& operator<<(std::ostream& stream, const DexRegisterLocation::Kind& kind) {
  using Kind = DexRegisterLocation::Kind;
  switch (kind) {
    case Kind::kNone:
      return stream << "none";
    case Kind::kInStack:
      return stream << "in stack";
    case Kind::kInRegister:
      return stream << "in register";
    case Kind::kInRegisterHigh:
      return stream << "in register high";
    case Kind::kInFpuRegister:
      return stream << "in fpu register";
    case Kind::kInFpuRegisterHigh:
      return stream << "in fpu register high";
    case Kind::kConstant:
      return stream << "as constant";
    case Kind::kInStackLargeOffset:
      return stream << "in stack (large offset)";
    case Kind::kConstantLargeValue:
      return stream << "as constant (large value)";
  }
  return stream << "Kind<" << static_cast<uint32_t>(kind) << ">";
}

DexRegisterLocation::Kind DexRegisterMap::GetLocationInternalKind(
    uint16_t dex_register_number,
    uint16_t number_of_dex_registers,
    const CodeInfo& code_info,
    const CodeInfoEncoding& enc) const {
  DexRegisterLocationCatalog dex_register_location_catalog =
      code_info.GetDexRegisterLocationCatalog(enc);
  size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
      dex_register_number,
      number_of_dex_registers,
      code_info.GetNumberOfLocationCatalogEntries(enc));
  return dex_register_location_catalog.GetLocationInternalKind(location_catalog_entry_index);
}

DexRegisterLocation DexRegisterMap::GetDexRegisterLocation(uint16_t dex_register_number,
                                                           uint16_t number_of_dex_registers,
                                                           const CodeInfo& code_info,
                                                           const CodeInfoEncoding& enc) const {
  DexRegisterLocationCatalog dex_register_location_catalog =
      code_info.GetDexRegisterLocationCatalog(enc);
  size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
      dex_register_number,
      number_of_dex_registers,
      code_info.GetNumberOfLocationCatalogEntries(enc));
  return dex_register_location_catalog.GetDexRegisterLocation(location_catalog_entry_index);
}

static void DumpRegisterMapping(std::ostream& os,
                                size_t dex_register_num,
                                DexRegisterLocation location,
                                const std::string& prefix = "v",
                                const std::string& suffix = "") {
  os << prefix << dex_register_num << ": "
     << location.GetInternalKind()
     << " (" << location.GetValue() << ")" << suffix << '\n';
}

void StackMapEncoding::Dump(VariableIndentationOutputStream* vios) const {
  vios->Stream()
      << "StackMapEncoding"
      << " (native_pc_bit_offset=" << static_cast<uint32_t>(kNativePcBitOffset)
      << ", dex_pc_bit_offset=" << static_cast<uint32_t>(dex_pc_bit_offset_)
      << ", dex_register_map_bit_offset=" << static_cast<uint32_t>(dex_register_map_bit_offset_)
      << ", inline_info_bit_offset=" << static_cast<uint32_t>(inline_info_bit_offset_)
      << ", register_mask_bit_offset=" << static_cast<uint32_t>(register_mask_bit_offset_)
      << ", stack_mask_bit_offset=" << static_cast<uint32_t>(stack_mask_bit_offset_)
      << ")\n";
}

void InlineInfoEncoding::Dump(VariableIndentationOutputStream* vios) const {
  vios->Stream()
      << "InlineInfoEncoding"
      << " (method_index_bit_offset=" << static_cast<uint32_t>(kMethodIndexBitOffset)
      << ", dex_pc_bit_offset=" << static_cast<uint32_t>(dex_pc_bit_offset_)
      << ", invoke_type_bit_offset=" << static_cast<uint32_t>(invoke_type_bit_offset_)
      << ", dex_register_map_bit_offset=" << static_cast<uint32_t>(dex_register_map_bit_offset_)
      << ", total_bit_size=" << static_cast<uint32_t>(total_bit_size_)
      << ")\n";
}

void CodeInfo::Dump(VariableIndentationOutputStream* vios,
                    uint32_t code_offset,
                    uint16_t number_of_dex_registers,
                    bool dump_stack_maps) const {
  CodeInfoEncoding encoding = ExtractEncoding();
  size_t number_of_stack_maps = GetNumberOfStackMaps(encoding);
  vios->Stream()
      << "Optimized CodeInfo (number_of_dex_registers=" << number_of_dex_registers
      << ", number_of_stack_maps=" << number_of_stack_maps
      << ")\n";
  ScopedIndentation indent1(vios);
  encoding.stack_map_encoding.Dump(vios);
  if (HasInlineInfo(encoding)) {
    encoding.inline_info_encoding.Dump(vios);
  }
  // Display the Dex register location catalog.
  GetDexRegisterLocationCatalog(encoding).Dump(vios, *this);
  // Display stack maps along with (live) Dex register maps.
  if (dump_stack_maps) {
    for (size_t i = 0; i < number_of_stack_maps; ++i) {
      StackMap stack_map = GetStackMapAt(i, encoding);
      stack_map.Dump(vios,
                     *this,
                     encoding,
                     code_offset,
                     number_of_dex_registers,
                     " " + std::to_string(i));
    }
  }
  // TODO: Dump the stack map's inline information? We need to know more from the caller:
  //       we need to know the number of dex registers for each inlined method.
}

void DexRegisterLocationCatalog::Dump(VariableIndentationOutputStream* vios,
                                      const CodeInfo& code_info) {
  CodeInfoEncoding encoding = code_info.ExtractEncoding();
  size_t number_of_location_catalog_entries = code_info.GetNumberOfLocationCatalogEntries(encoding);
  size_t location_catalog_size_in_bytes = code_info.GetDexRegisterLocationCatalogSize(encoding);
  vios->Stream()
      << "DexRegisterLocationCatalog (number_of_entries=" << number_of_location_catalog_entries
      << ", size_in_bytes=" << location_catalog_size_in_bytes << ")\n";
  for (size_t i = 0; i < number_of_location_catalog_entries; ++i) {
    DexRegisterLocation location = GetDexRegisterLocation(i);
    ScopedIndentation indent1(vios);
    DumpRegisterMapping(vios->Stream(), i, location, "entry ");
  }
}

void DexRegisterMap::Dump(VariableIndentationOutputStream* vios,
                          const CodeInfo& code_info,
                          uint16_t number_of_dex_registers) const {
  CodeInfoEncoding encoding = code_info.ExtractEncoding();
  size_t number_of_location_catalog_entries = code_info.GetNumberOfLocationCatalogEntries(encoding);
  // TODO: Display the bit mask of live Dex registers.
  for (size_t j = 0; j < number_of_dex_registers; ++j) {
    if (IsDexRegisterLive(j)) {
      size_t location_catalog_entry_index = GetLocationCatalogEntryIndex(
          j, number_of_dex_registers, number_of_location_catalog_entries);
      DexRegisterLocation location = GetDexRegisterLocation(j,
                                                            number_of_dex_registers,
                                                            code_info,
                                                            encoding);
      ScopedIndentation indent1(vios);
      DumpRegisterMapping(
          vios->Stream(), j, location, "v",
          "\t[entry " + std::to_string(static_cast<int>(location_catalog_entry_index)) + "]");
    }
  }
}

void StackMap::Dump(VariableIndentationOutputStream* vios,
                    const CodeInfo& code_info,
                    const CodeInfoEncoding& encoding,
                    uint32_t code_offset,
                    uint16_t number_of_dex_registers,
                    const std::string& header_suffix) const {
  StackMapEncoding stack_map_encoding = encoding.stack_map_encoding;
  vios->Stream()
      << "StackMap" << header_suffix
      << std::hex
      << " [native_pc=0x" << code_offset + GetNativePcOffset(stack_map_encoding) << "]"
      << " (dex_pc=0x" << GetDexPc(stack_map_encoding)
      << ", native_pc_offset=0x" << GetNativePcOffset(stack_map_encoding)
      << ", dex_register_map_offset=0x" << GetDexRegisterMapOffset(stack_map_encoding)
      << ", inline_info_offset=0x" << GetInlineDescriptorOffset(stack_map_encoding)
      << ", register_mask=0x" << GetRegisterMask(stack_map_encoding)
      << std::dec
      << ", stack_mask=0b";
  for (size_t i = 0, e = GetNumberOfStackMaskBits(stack_map_encoding); i < e; ++i) {
    vios->Stream() << GetStackMaskBit(stack_map_encoding, e - i - 1);
  }
  vios->Stream() << ")\n";
  if (HasDexRegisterMap(stack_map_encoding)) {
    DexRegisterMap dex_register_map = code_info.GetDexRegisterMapOf(
        *this, encoding, number_of_dex_registers);
    dex_register_map.Dump(vios, code_info, number_of_dex_registers);
  }
  if (HasInlineInfo(stack_map_encoding)) {
    InlineInfo inline_info = code_info.GetInlineInfoOf(*this, encoding);
    // We do not know the length of the dex register maps of inlined frames
    // at this level, so we just pass null to `InlineInfo::Dump` to tell
    // it not to look at these maps.
    inline_info.Dump(vios, code_info, nullptr);
  }
}

void InlineInfo::Dump(VariableIndentationOutputStream* vios,
                      const CodeInfo& code_info,
                      uint16_t number_of_dex_registers[]) const {
  InlineInfoEncoding inline_info_encoding = code_info.ExtractEncoding().inline_info_encoding;
  vios->Stream() << "InlineInfo with depth "
                 << static_cast<uint32_t>(GetDepth(inline_info_encoding))
                 << "\n";

  for (size_t i = 0; i < GetDepth(inline_info_encoding); ++i) {
    vios->Stream()
        << " At depth " << i
        << std::hex
        << " (dex_pc=0x" << GetDexPcAtDepth(inline_info_encoding, i)
        << std::dec
        << ", method_index=" << GetMethodIndexAtDepth(inline_info_encoding, i)
        << ", invoke_type=" << static_cast<InvokeType>(GetInvokeTypeAtDepth(inline_info_encoding,
                                                                            i))
        << ")\n";
    if (HasDexRegisterMapAtDepth(inline_info_encoding, i) && (number_of_dex_registers != nullptr)) {
      CodeInfoEncoding encoding = code_info.ExtractEncoding();
      DexRegisterMap dex_register_map =
          code_info.GetDexRegisterMapAtDepth(i, *this, encoding, number_of_dex_registers[i]);
      ScopedIndentation indent1(vios);
      dex_register_map.Dump(vios, code_info, number_of_dex_registers[i]);
    }
  }
}

}  // namespace art
