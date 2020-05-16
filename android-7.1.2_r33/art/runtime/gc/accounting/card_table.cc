/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "card_table.h"

#include "base/logging.h"
#include "base/systrace.h"
#include "card_table-inl.h"
#include "gc/heap.h"
#include "gc/space/space.h"
#include "heap_bitmap.h"
#include "mem_map.h"
#include "runtime.h"
#include "utils.h"

namespace art {
namespace gc {
namespace accounting {

constexpr size_t CardTable::kCardShift;
constexpr size_t CardTable::kCardSize;
constexpr uint8_t CardTable::kCardClean;
constexpr uint8_t CardTable::kCardDirty;

/*
 * Maintain a card table from the write barrier. All writes of
 * non-null values to heap addresses should go through an entry in
 * WriteBarrier, and from there to here.
 *
 * The heap is divided into "cards" of GC_CARD_SIZE bytes, as
 * determined by GC_CARD_SHIFT. The card table contains one byte of
 * data per card, to be used by the GC. The value of the byte will be
 * one of GC_CARD_CLEAN or GC_CARD_DIRTY.
 *
 * After any store of a non-null object pointer into a heap object,
 * code is obliged to mark the card dirty. The setters in
 * object.h [such as SetFieldObject] do this for you. The
 * compiler also contains code to mark cards as dirty.
 *
 * The card table's base [the "biased card table"] gets set to a
 * rather strange value.  In order to keep the JIT from having to
 * fabricate or load GC_DIRTY_CARD to store into the card table,
 * biased base is within the mmap allocation at a point where its low
 * byte is equal to GC_DIRTY_CARD. See CardTable::Create for details.
 */

CardTable* CardTable::Create(const uint8_t* heap_begin, size_t heap_capacity) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  /* Set up the card table */
  size_t capacity = heap_capacity / kCardSize;
  /* Allocate an extra 256 bytes to allow fixed low-byte of base */
  std::string error_msg;
  std::unique_ptr<MemMap> mem_map(
      MemMap::MapAnonymous("card table", nullptr, capacity + 256, PROT_READ | PROT_WRITE,
                           false, false, &error_msg));
  CHECK(mem_map.get() != nullptr) << "couldn't allocate card table: " << error_msg;
  // All zeros is the correct initial value; all clean. Anonymous mmaps are initialized to zero, we
  // don't clear the card table to avoid unnecessary pages being allocated
  static_assert(kCardClean == 0, "kCardClean must be 0");

  uint8_t* cardtable_begin = mem_map->Begin();
  CHECK(cardtable_begin != nullptr);

  // We allocated up to a bytes worth of extra space to allow biased_begin's byte value to equal
  // kCardDirty, compute a offset value to make this the case
  size_t offset = 0;
  uint8_t* biased_begin = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(cardtable_begin) -
      (reinterpret_cast<uintptr_t>(heap_begin) >> kCardShift));
  uintptr_t biased_byte = reinterpret_cast<uintptr_t>(biased_begin) & 0xff;
  if (biased_byte != kCardDirty) {
    int delta = kCardDirty - biased_byte;
    offset = delta + (delta < 0 ? 0x100 : 0);
    biased_begin += offset;
  }
  CHECK_EQ(reinterpret_cast<uintptr_t>(biased_begin) & 0xff, kCardDirty);
  return new CardTable(mem_map.release(), biased_begin, offset);
}

CardTable::CardTable(MemMap* mem_map, uint8_t* biased_begin, size_t offset)
    : mem_map_(mem_map), biased_begin_(biased_begin), offset_(offset) {
}

CardTable::~CardTable() {
  // Destroys MemMap via std::unique_ptr<>.
}

void CardTable::ClearSpaceCards(space::ContinuousSpace* space) {
  // TODO: clear just the range of the table that has been modified
  uint8_t* card_start = CardFromAddr(space->Begin());
  uint8_t* card_end = CardFromAddr(space->End());  // Make sure to round up.
  memset(reinterpret_cast<void*>(card_start), kCardClean, card_end - card_start);
}

void CardTable::ClearCardTable() {
  static_assert(kCardClean == 0, "kCardClean must be 0");
  mem_map_->MadviseDontNeedAndZero();
}

void CardTable::ClearCardRange(uint8_t* start, uint8_t* end) {
  if (!kMadviseZeroes) {
    memset(start, 0, end - start);
    return;
  }
  CHECK_ALIGNED(reinterpret_cast<uintptr_t>(start), kCardSize);
  CHECK_ALIGNED(reinterpret_cast<uintptr_t>(end), kCardSize);
  static_assert(kCardClean == 0, "kCardClean must be 0");
  uint8_t* start_card = CardFromAddr(start);
  uint8_t* end_card = CardFromAddr(end);
  uint8_t* round_start = AlignUp(start_card, kPageSize);
  uint8_t* round_end = AlignDown(end_card, kPageSize);
  if (round_start < round_end) {
    madvise(round_start, round_end - round_start, MADV_DONTNEED);
  }
  // Handle unaligned regions at start / end.
  memset(start_card, 0, std::min(round_start, end_card) - start_card);
  memset(std::max(round_end, start_card), 0, end_card - std::max(round_end, start_card));
}

bool CardTable::AddrIsInCardTable(const void* addr) const {
  return IsValidCard(biased_begin_ + ((uintptr_t)addr >> kCardShift));
}

void CardTable::CheckAddrIsInCardTable(const uint8_t* addr) const {
  uint8_t* card_addr = biased_begin_ + ((uintptr_t)addr >> kCardShift);
  uint8_t* begin = mem_map_->Begin() + offset_;
  uint8_t* end = mem_map_->End();
  CHECK(AddrIsInCardTable(addr))
      << "Card table " << this
      << " begin: " << reinterpret_cast<void*>(begin)
      << " end: " << reinterpret_cast<void*>(end)
      << " card_addr: " << reinterpret_cast<void*>(card_addr)
      << " heap begin: " << AddrFromCard(begin)
      << " heap end: " << AddrFromCard(end)
      << " addr: " << reinterpret_cast<const void*>(addr);
}

void CardTable::VerifyCardTable() {
  UNIMPLEMENTED(WARNING) << "Card table verification";
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
