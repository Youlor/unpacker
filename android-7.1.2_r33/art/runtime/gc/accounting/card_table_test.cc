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

#include "card_table-inl.h"

#include <string>

#include "atomic.h"
#include "common_runtime_test.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "mirror/string-inl.h"  // Strings are easiest to allocate
#include "scoped_thread_state_change.h"
#include "thread_pool.h"
#include "utils.h"

namespace art {

namespace mirror {
  class Object;
}  // namespace mirror

namespace gc {
namespace accounting {

class CardTableTest : public CommonRuntimeTest {
 public:
  std::unique_ptr<CardTable> card_table_;

  void CommonSetup() {
    if (card_table_.get() == nullptr) {
      card_table_.reset(CardTable::Create(heap_begin_, heap_size_));
      EXPECT_TRUE(card_table_.get() != nullptr);
    } else {
      ClearCardTable();
    }
  }
  // Default values for the test, not random to avoid undeterministic behaviour.
  CardTableTest() : heap_begin_(reinterpret_cast<uint8_t*>(0x2000000)), heap_size_(2 * MB) {
  }
  void ClearCardTable() {
    card_table_->ClearCardTable();
  }
  uint8_t* HeapBegin() const {
    return heap_begin_;
  }
  uint8_t* HeapLimit() const {
    return HeapBegin() + heap_size_;
  }
  // Return a pseudo random card for an address.
  uint8_t PseudoRandomCard(const uint8_t* addr) const {
    size_t offset = RoundDown(addr - heap_begin_, CardTable::kCardSize);
    return 1 + offset % 254;
  }
  void FillRandom() {
    for (const uint8_t* addr = HeapBegin(); addr != HeapLimit(); addr += CardTable::kCardSize) {
      EXPECT_TRUE(card_table_->AddrIsInCardTable(addr));
      uint8_t* card = card_table_->CardFromAddr(addr);
      *card = PseudoRandomCard(addr);
    }
  }

 private:
  uint8_t* const heap_begin_;
  const size_t heap_size_;
};

TEST_F(CardTableTest, TestMarkCard) {
  CommonSetup();
  for (const uint8_t* addr = HeapBegin(); addr < HeapLimit(); addr += kObjectAlignment) {
    auto obj = reinterpret_cast<const mirror::Object*>(addr);
    EXPECT_EQ(card_table_->GetCard(obj), CardTable::kCardClean);
    EXPECT_TRUE(!card_table_->IsDirty(obj));
    card_table_->MarkCard(addr);
    EXPECT_TRUE(card_table_->IsDirty(obj));
    EXPECT_EQ(card_table_->GetCard(obj), CardTable::kCardDirty);
    uint8_t* card_addr = card_table_->CardFromAddr(addr);
    EXPECT_EQ(*card_addr, CardTable::kCardDirty);
    *card_addr = CardTable::kCardClean;
    EXPECT_EQ(*card_addr, CardTable::kCardClean);
  }
}

class UpdateVisitor {
 public:
  uint8_t operator()(uint8_t c) const {
    return c * 93 + 123;
  }
  void operator()(uint8_t* /*card*/, uint8_t /*expected_value*/, uint8_t /*new_value*/) const {
  }
};

TEST_F(CardTableTest, TestModifyCardsAtomic) {
  CommonSetup();
  FillRandom();
  const size_t delta = std::min(static_cast<size_t>(HeapLimit() - HeapBegin()),
                                8U * CardTable::kCardSize);
  UpdateVisitor visitor;
  size_t start_offset = 0;
  for (uint8_t* cstart = HeapBegin(); cstart < HeapBegin() + delta; cstart += CardTable::kCardSize) {
    start_offset = (start_offset + kObjectAlignment) % CardTable::kCardSize;
    size_t end_offset = 0;
    for (uint8_t* cend = HeapLimit() - delta; cend < HeapLimit(); cend += CardTable::kCardSize) {
      // Don't always start at a card boundary.
      uint8_t* start = cstart + start_offset;
      uint8_t* end = cend - end_offset;
      end_offset = (end_offset + kObjectAlignment) % CardTable::kCardSize;
      // Modify cards.
      card_table_->ModifyCardsAtomic(start, end, visitor, visitor);
      // Check adjacent cards not modified.
      for (uint8_t* cur = start - CardTable::kCardSize; cur >= HeapBegin();
          cur -= CardTable::kCardSize) {
        EXPECT_EQ(card_table_->GetCard(reinterpret_cast<mirror::Object*>(cur)),
                  PseudoRandomCard(cur));
      }
      for (uint8_t* cur = end + CardTable::kCardSize; cur < HeapLimit();
          cur += CardTable::kCardSize) {
        EXPECT_EQ(card_table_->GetCard(reinterpret_cast<mirror::Object*>(cur)),
                  PseudoRandomCard(cur));
      }
      // Verify Range.
      for (uint8_t* cur = start; cur < AlignUp(end, CardTable::kCardSize);
          cur += CardTable::kCardSize) {
        uint8_t* card = card_table_->CardFromAddr(cur);
        uint8_t value = PseudoRandomCard(cur);
        EXPECT_EQ(visitor(value), *card);
        // Restore for next iteration.
        *card = value;
      }
    }
  }
}

// TODO: Add test for CardTable::Scan.
}  // namespace accounting
}  // namespace gc
}  // namespace art
