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

#include "bitmap-inl.h"

#include "base/bit_utils.h"
#include "card_table.h"
#include "jit/jit_code_cache.h"
#include "mem_map.h"

namespace art {
namespace gc {
namespace accounting {

Bitmap* Bitmap::CreateFromMemMap(MemMap* mem_map, size_t num_bits) {
  CHECK(mem_map != nullptr);
  return new Bitmap(mem_map, num_bits);
}

Bitmap::Bitmap(MemMap* mem_map, size_t bitmap_size)
    : mem_map_(mem_map), bitmap_begin_(reinterpret_cast<uintptr_t*>(mem_map->Begin())),
      bitmap_size_(bitmap_size) {
  CHECK(bitmap_begin_ != nullptr);
  CHECK_NE(bitmap_size, 0U);
}

Bitmap::~Bitmap() {
  // Destroys MemMap via std::unique_ptr<>.
}

MemMap* Bitmap::AllocateMemMap(const std::string& name, size_t num_bits) {
  const size_t bitmap_size = RoundUp(
      RoundUp(num_bits, kBitsPerBitmapWord) / kBitsPerBitmapWord * sizeof(uintptr_t), kPageSize);
  std::string error_msg;
  std::unique_ptr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), nullptr, bitmap_size,
                                                       PROT_READ | PROT_WRITE, false, false,
                                                       &error_msg));
  if (UNLIKELY(mem_map.get() == nullptr)) {
    LOG(ERROR) << "Failed to allocate bitmap " << name << ": " << error_msg;
    return nullptr;
  }
  return mem_map.release();
}

Bitmap* Bitmap::Create(const std::string& name, size_t num_bits) {
  auto* const mem_map = AllocateMemMap(name, num_bits);
  if (mem_map == nullptr) {
    return nullptr;
  }
  return CreateFromMemMap(mem_map, num_bits);
}

void Bitmap::Clear() {
  if (bitmap_begin_ != nullptr) {
    mem_map_->MadviseDontNeedAndZero();
  }
}

void Bitmap::CopyFrom(Bitmap* source_bitmap) {
  DCHECK_EQ(BitmapSize(), source_bitmap->BitmapSize());
  std::copy(source_bitmap->Begin(),
            source_bitmap->Begin() + BitmapSize() / kBitsPerBitmapWord, Begin());
}

template<size_t kAlignment>
MemoryRangeBitmap<kAlignment>* MemoryRangeBitmap<kAlignment>::Create(
    const std::string& name, uintptr_t cover_begin, uintptr_t cover_end) {
  CHECK_ALIGNED(cover_begin, kAlignment);
  CHECK_ALIGNED(cover_end, kAlignment);
  const size_t num_bits = (cover_end - cover_begin) / kAlignment;
  auto* const mem_map = Bitmap::AllocateMemMap(name, num_bits);
  return CreateFromMemMap(mem_map, cover_begin, num_bits);
}

template<size_t kAlignment>
MemoryRangeBitmap<kAlignment>* MemoryRangeBitmap<kAlignment>::CreateFromMemMap(
    MemMap* mem_map, uintptr_t begin, size_t num_bits) {
  return new MemoryRangeBitmap(mem_map, begin, num_bits);
}

template class MemoryRangeBitmap<CardTable::kCardSize>;
template class MemoryRangeBitmap<jit::kJitCodeAlignment>;

}  // namespace accounting
}  // namespace gc
}  // namespace art

