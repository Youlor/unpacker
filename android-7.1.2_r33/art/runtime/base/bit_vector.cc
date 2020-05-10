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

#include "bit_vector.h"

#include <limits>
#include <sstream>

#include "allocator.h"
#include "bit_vector-inl.h"

namespace art {

BitVector::BitVector(bool expandable,
                     Allocator* allocator,
                     uint32_t storage_size,
                     uint32_t* storage)
  : storage_(storage),
    storage_size_(storage_size),
    allocator_(allocator),
    expandable_(expandable) {
  DCHECK(storage_ != nullptr);

  static_assert(sizeof(*storage_) == kWordBytes, "word bytes");
  static_assert(sizeof(*storage_) * 8u == kWordBits, "word bits");
}

BitVector::BitVector(uint32_t start_bits,
                     bool expandable,
                     Allocator* allocator)
  : BitVector(expandable,
              allocator,
              BitsToWords(start_bits),
              static_cast<uint32_t*>(allocator->Alloc(BitsToWords(start_bits) * kWordBytes))) {
}


BitVector::BitVector(const BitVector& src,
                     bool expandable,
                     Allocator* allocator)
  : BitVector(expandable,
              allocator,
              src.storage_size_,
              static_cast<uint32_t*>(allocator->Alloc(src.storage_size_ * kWordBytes))) {
  // Direct memcpy would be faster, but this should be fine too and is cleaner.
  Copy(&src);
}

BitVector::~BitVector() {
  allocator_->Free(storage_);
}

bool BitVector::SameBitsSet(const BitVector *src) const {
  int our_highest = GetHighestBitSet();
  int src_highest = src->GetHighestBitSet();

  // If the highest bit set is different, we are different.
  if (our_highest != src_highest) {
    return false;
  }

  // If the highest bit set is -1, both are cleared, we are the same.
  // If the highest bit set is 0, both have a unique bit set, we are the same.
  if (our_highest <= 0) {
    return true;
  }

  // Get the highest bit set's cell's index
  // No need of highest + 1 here because it can't be 0 so BitsToWords will work here.
  int our_highest_index = BitsToWords(our_highest);

  // This memcmp is enough: we know that the highest bit set is the same for both:
  //   - Therefore, min_size goes up to at least that, we are thus comparing at least what we need to, but not less.
  //      ie. we are comparing all storage cells that could have difference, if both vectors have cells above our_highest_index,
  //          they are automatically at 0.
  return (memcmp(storage_, src->GetRawStorage(), our_highest_index * kWordBytes) == 0);
}

bool BitVector::IsSubsetOf(const BitVector *other) const {
  int this_highest = GetHighestBitSet();
  int other_highest = other->GetHighestBitSet();

  // If the highest bit set is -1, this is empty and a trivial subset.
  if (this_highest < 0) {
    return true;
  }

  // If the highest bit set is higher, this cannot be a subset.
  if (this_highest > other_highest) {
    return false;
  }

  // Compare each 32-bit word.
  size_t this_highest_index = BitsToWords(this_highest + 1);
  for (size_t i = 0; i < this_highest_index; ++i) {
    uint32_t this_storage = storage_[i];
    uint32_t other_storage = other->storage_[i];
    if ((this_storage | other_storage) != other_storage) {
      return false;
    }
  }
  return true;
}

void BitVector::Intersect(const BitVector* src) {
  uint32_t src_storage_size = src->storage_size_;

  // Get the minimum size between us and source.
  uint32_t min_size = (storage_size_ < src_storage_size) ? storage_size_ : src_storage_size;

  uint32_t idx;
  for (idx = 0; idx < min_size; idx++) {
    storage_[idx] &= src->GetRawStorageWord(idx);
  }

  // Now, due to this being an intersection, there are two possibilities:
  //   - Either src was larger than us: we don't care, all upper bits would thus be 0.
  //   - Either we are larger than src: we don't care, all upper bits would have been 0 too.
  // So all we need to do is set all remaining bits to 0.
  for (; idx < storage_size_; idx++) {
    storage_[idx] = 0;
  }
}

bool BitVector::Union(const BitVector* src) {
  // Get the highest bit to determine how much we need to expand.
  int highest_bit = src->GetHighestBitSet();
  bool changed = false;

  // If src has no bit set, we are done: there is no need for a union with src.
  if (highest_bit == -1) {
    return changed;
  }

  // Update src_size to how many cells we actually care about: where the bit is + 1.
  uint32_t src_size = BitsToWords(highest_bit + 1);

  // Is the storage size smaller than src's?
  if (storage_size_ < src_size) {
    changed = true;

    EnsureSize(highest_bit);

    // Paranoid: storage size should be big enough to hold this bit now.
    DCHECK_LT(static_cast<uint32_t> (highest_bit), storage_size_ * kWordBits);
  }

  for (uint32_t idx = 0; idx < src_size; idx++) {
    uint32_t existing = storage_[idx];
    uint32_t update = existing | src->GetRawStorageWord(idx);
    if (existing != update) {
      changed = true;
      storage_[idx] = update;
    }
  }
  return changed;
}

bool BitVector::UnionIfNotIn(const BitVector* union_with, const BitVector* not_in) {
  // Get the highest bit to determine how much we need to expand.
  int highest_bit = union_with->GetHighestBitSet();
  bool changed = false;

  // If src has no bit set, we are done: there is no need for a union with src.
  if (highest_bit == -1) {
    return changed;
  }

  // Update union_with_size to how many cells we actually care about: where the bit is + 1.
  uint32_t union_with_size = BitsToWords(highest_bit + 1);

  // Is the storage size smaller than src's?
  if (storage_size_ < union_with_size) {
    EnsureSize(highest_bit);

    // Paranoid: storage size should be big enough to hold this bit now.
    DCHECK_LT(static_cast<uint32_t> (highest_bit), storage_size_ * kWordBits);
  }

  uint32_t not_in_size = not_in->GetStorageSize();

  uint32_t idx = 0;
  for (; idx < std::min(not_in_size, union_with_size); idx++) {
    uint32_t existing = storage_[idx];
    uint32_t update = existing |
        (union_with->GetRawStorageWord(idx) & ~not_in->GetRawStorageWord(idx));
    if (existing != update) {
      changed = true;
      storage_[idx] = update;
    }
  }

  for (; idx < union_with_size; idx++) {
    uint32_t existing = storage_[idx];
    uint32_t update = existing | union_with->GetRawStorageWord(idx);
    if (existing != update) {
      changed = true;
      storage_[idx] = update;
    }
  }
  return changed;
}

void BitVector::Subtract(const BitVector *src) {
  uint32_t src_size = src->storage_size_;

  // We only need to operate on bytes up to the smaller of the sizes of the two operands.
  unsigned int min_size = (storage_size_ > src_size) ? src_size : storage_size_;

  // Difference until max, we know both accept it:
  //   There is no need to do more:
  //     If we are bigger than src, the upper bits are unchanged.
  //     If we are smaller than src, the non-existant upper bits are 0 and thus can't get subtracted.
  for (uint32_t idx = 0; idx < min_size; idx++) {
    storage_[idx] &= (~(src->GetRawStorageWord(idx)));
  }
}

uint32_t BitVector::NumSetBits() const {
  uint32_t count = 0;
  for (uint32_t word = 0; word < storage_size_; word++) {
    count += POPCOUNT(storage_[word]);
  }
  return count;
}

uint32_t BitVector::NumSetBits(uint32_t end) const {
  DCHECK_LE(end, storage_size_ * kWordBits);
  return NumSetBits(storage_, end);
}

void BitVector::SetInitialBits(uint32_t num_bits) {
  // If num_bits is 0, clear everything.
  if (num_bits == 0) {
    ClearAllBits();
    return;
  }

  // Set the highest bit we want to set to get the BitVector allocated if need be.
  SetBit(num_bits - 1);

  uint32_t idx;
  // We can set every storage element with -1.
  for (idx = 0; idx < WordIndex(num_bits); idx++) {
    storage_[idx] = std::numeric_limits<uint32_t>::max();
  }

  // Handle the potentially last few bits.
  uint32_t rem_num_bits = num_bits & 0x1f;
  if (rem_num_bits != 0) {
    storage_[idx] = (1U << rem_num_bits) - 1;
    ++idx;
  }

  // Now set the upper ones to 0.
  for (; idx < storage_size_; idx++) {
    storage_[idx] = 0;
  }
}

int BitVector::GetHighestBitSet() const {
  unsigned int max = storage_size_;
  for (int idx = max - 1; idx >= 0; idx--) {
    // If not 0, we have more work: check the bits.
    uint32_t value = storage_[idx];

    if (value != 0) {
      // Return highest bit set in value plus bits from previous storage indexes.
      return 31 - CLZ(value) + (idx * kWordBits);
    }
  }

  // All zero, therefore return -1.
  return -1;
}

void BitVector::Copy(const BitVector *src) {
  // Get highest bit set, we only need to copy till then.
  int highest_bit = src->GetHighestBitSet();

  // If nothing is set, clear everything.
  if (highest_bit == -1) {
    ClearAllBits();
    return;
  }

  // Set upper bit to ensure right size before copy.
  SetBit(highest_bit);

  // Now set until highest bit's storage.
  uint32_t size = 1 + (highest_bit / kWordBits);
  memcpy(storage_, src->GetRawStorage(), kWordBytes * size);

  // Set upper bits to 0.
  uint32_t left = storage_size_ - size;

  if (left > 0) {
    memset(storage_ + size, 0, kWordBytes * left);
  }
}

uint32_t BitVector::NumSetBits(const uint32_t* storage, uint32_t end) {
  uint32_t word_end = WordIndex(end);
  uint32_t partial_word_bits = end & 0x1f;

  uint32_t count = 0u;
  for (uint32_t word = 0u; word < word_end; word++) {
    count += POPCOUNT(storage[word]);
  }
  if (partial_word_bits != 0u) {
    count += POPCOUNT(storage[word_end] & ~(0xffffffffu << partial_word_bits));
  }
  return count;
}

void BitVector::Dump(std::ostream& os, const char *prefix) const {
  std::ostringstream buffer;
  DumpHelper(prefix, buffer);
  os << buffer.str() << std::endl;
}

void BitVector::DumpHelper(const char* prefix, std::ostringstream& buffer) const {
  // Initialize it.
  if (prefix != nullptr) {
    buffer << prefix;
  }

  buffer << '(';
  for (size_t i = 0; i < storage_size_ * kWordBits; i++) {
    buffer << IsBitSet(i);
  }
  buffer << ')';
}

void BitVector::EnsureSize(uint32_t idx) {
  if (idx >= storage_size_ * kWordBits) {
    DCHECK(expandable_) << "Attempted to expand a non-expandable bitmap to position " << idx;

    /* Round up to word boundaries for "idx+1" bits */
    uint32_t new_size = BitsToWords(idx + 1);
    DCHECK_GT(new_size, storage_size_);
    uint32_t *new_storage =
        static_cast<uint32_t*>(allocator_->Alloc(new_size * kWordBytes));
    memcpy(new_storage, storage_, storage_size_ * kWordBytes);
    // Zero out the new storage words.
    memset(&new_storage[storage_size_], 0, (new_size - storage_size_) * kWordBytes);
    // TODO: collect stats on space wasted because of resize.

    // Free old storage.
    allocator_->Free(storage_);

    // Set fields.
    storage_ = new_storage;
    storage_size_ = new_size;
  }
}

Allocator* BitVector::GetAllocator() const {
  return allocator_;
}

}  // namespace art
