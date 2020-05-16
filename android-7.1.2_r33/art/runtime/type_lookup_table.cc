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

#include "type_lookup_table.h"

#include "base/bit_utils.h"
#include "dex_file-inl.h"
#include "utf-inl.h"
#include "utils.h"

#include <memory>
#include <cstring>

namespace art {

static uint16_t MakeData(uint16_t class_def_idx, uint32_t hash, uint32_t mask) {
  uint16_t hash_mask = static_cast<uint16_t>(~mask);
  return (static_cast<uint16_t>(hash) & hash_mask) | class_def_idx;
}

TypeLookupTable::~TypeLookupTable() {
  if (!owns_entries_) {
    // We don't actually own the entries, don't let the unique_ptr release them.
    entries_.release();
  }
}

uint32_t TypeLookupTable::RawDataLength() const {
  return RawDataLength(dex_file_);
}

uint32_t TypeLookupTable::RawDataLength(const DexFile& dex_file) {
  return RawDataLength(dex_file.NumClassDefs());
}

uint32_t TypeLookupTable::RawDataLength(uint32_t num_class_defs) {
  return SupportedSize(num_class_defs) ? RoundUpToPowerOfTwo(num_class_defs) * sizeof(Entry) : 0u;
}

uint32_t TypeLookupTable::CalculateMask(uint32_t num_class_defs) {
  return SupportedSize(num_class_defs) ? RoundUpToPowerOfTwo(num_class_defs) - 1u : 0u;
}

bool TypeLookupTable::SupportedSize(uint32_t num_class_defs) {
  return num_class_defs != 0u && num_class_defs <= std::numeric_limits<uint16_t>::max();
}

TypeLookupTable* TypeLookupTable::Create(const DexFile& dex_file, uint8_t* storage) {
  const uint32_t num_class_defs = dex_file.NumClassDefs();
  return SupportedSize(num_class_defs)
      ? new TypeLookupTable(dex_file, storage)
      : nullptr;
}

TypeLookupTable* TypeLookupTable::Open(const uint8_t* raw_data, const DexFile& dex_file) {
  return new TypeLookupTable(raw_data, dex_file);
}

TypeLookupTable::TypeLookupTable(const DexFile& dex_file, uint8_t* storage)
    : dex_file_(dex_file),
      mask_(CalculateMask(dex_file.NumClassDefs())),
      entries_(storage != nullptr ? reinterpret_cast<Entry*>(storage) : new Entry[mask_ + 1]),
      owns_entries_(storage == nullptr) {
  static_assert(alignof(Entry) == 4u, "Expecting Entry to be 4-byte aligned.");
  DCHECK_ALIGNED(storage, alignof(Entry));
  std::vector<uint16_t> conflict_class_defs;
  // The first stage. Put elements on their initial positions. If an initial position is already
  // occupied then delay the insertion of the element to the second stage to reduce probing
  // distance.
  for (size_t i = 0; i < dex_file.NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
    const DexFile::TypeId& type_id = dex_file.GetTypeId(class_def.class_idx_);
    const DexFile::StringId& str_id = dex_file.GetStringId(type_id.descriptor_idx_);
    const uint32_t hash = ComputeModifiedUtf8Hash(dex_file.GetStringData(str_id));
    Entry entry;
    entry.str_offset = str_id.string_data_off_;
    entry.data = MakeData(i, hash, GetSizeMask());
    if (!SetOnInitialPos(entry, hash)) {
      conflict_class_defs.push_back(i);
    }
  }
  // The second stage. The initial position of these elements had a collision. Put these elements
  // into the nearest free cells and link them together by updating next_pos_delta.
  for (uint16_t class_def_idx : conflict_class_defs) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_idx);
    const DexFile::TypeId& type_id = dex_file.GetTypeId(class_def.class_idx_);
    const DexFile::StringId& str_id = dex_file.GetStringId(type_id.descriptor_idx_);
    const uint32_t hash = ComputeModifiedUtf8Hash(dex_file.GetStringData(str_id));
    Entry entry;
    entry.str_offset = str_id.string_data_off_;
    entry.data = MakeData(class_def_idx, hash, GetSizeMask());
    Insert(entry, hash);
  }
}

TypeLookupTable::TypeLookupTable(const uint8_t* raw_data, const DexFile& dex_file)
    : dex_file_(dex_file),
      mask_(CalculateMask(dex_file.NumClassDefs())),
      entries_(reinterpret_cast<Entry*>(const_cast<uint8_t*>(raw_data))),
      owns_entries_(false) {}

bool TypeLookupTable::SetOnInitialPos(const Entry& entry, uint32_t hash) {
  const uint32_t pos = hash & GetSizeMask();
  if (!entries_[pos].IsEmpty()) {
    return false;
  }
  entries_[pos] = entry;
  entries_[pos].next_pos_delta = 0;
  return true;
}

void TypeLookupTable::Insert(const Entry& entry, uint32_t hash) {
  uint32_t pos = FindLastEntryInBucket(hash & GetSizeMask());
  uint32_t next_pos = (pos + 1) & GetSizeMask();
  while (!entries_[next_pos].IsEmpty()) {
    next_pos = (next_pos + 1) & GetSizeMask();
  }
  const uint32_t delta = (next_pos >= pos) ? (next_pos - pos) : (next_pos + Size() - pos);
  entries_[pos].next_pos_delta = delta;
  entries_[next_pos] = entry;
  entries_[next_pos].next_pos_delta = 0;
}

uint32_t TypeLookupTable::FindLastEntryInBucket(uint32_t pos) const {
  const Entry* entry = &entries_[pos];
  while (!entry->IsLast()) {
    pos = (pos + entry->next_pos_delta) & GetSizeMask();
    entry = &entries_[pos];
  }
  return pos;
}

}  // namespace art
