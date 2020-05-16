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

#include "mod_union_table.h"

#include <memory>

#include "base/stl_util.h"
#include "bitmap-inl.h"
#include "card_table-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "gc/space/space.h"
#include "mirror/object-inl.h"
#include "space_bitmap-inl.h"
#include "thread-inl.h"

namespace art {
namespace gc {
namespace accounting {

class ModUnionAddToCardSetVisitor {
 public:
  explicit ModUnionAddToCardSetVisitor(ModUnionTable::CardSet* const cleared_cards)
      : cleared_cards_(cleared_cards) {}

  inline void operator()(uint8_t* card,
                         uint8_t expected_value,
                         uint8_t new_value ATTRIBUTE_UNUSED) const {
    if (expected_value == CardTable::kCardDirty) {
      cleared_cards_->insert(card);
    }
  }

 private:
  ModUnionTable::CardSet* const cleared_cards_;
};

class ModUnionAddToCardBitmapVisitor {
 public:
  ModUnionAddToCardBitmapVisitor(ModUnionTable::CardBitmap* bitmap, CardTable* card_table)
      : bitmap_(bitmap), card_table_(card_table) {}

  inline void operator()(uint8_t* card,
                         uint8_t expected_value,
                         uint8_t new_value ATTRIBUTE_UNUSED) const {
    if (expected_value == CardTable::kCardDirty) {
      // We want the address the card represents, not the address of the card.
      bitmap_->Set(reinterpret_cast<uintptr_t>(card_table_->AddrFromCard(card)));
    }
  }

 private:
  ModUnionTable::CardBitmap* const bitmap_;
  CardTable* const card_table_;
};

class ModUnionAddToCardVectorVisitor {
 public:
  explicit ModUnionAddToCardVectorVisitor(std::vector<uint8_t*>* cleared_cards)
      : cleared_cards_(cleared_cards) {
  }

  void operator()(uint8_t* card, uint8_t expected_card, uint8_t new_card ATTRIBUTE_UNUSED) const {
    if (expected_card == CardTable::kCardDirty) {
      cleared_cards_->push_back(card);
    }
  }

 private:
  std::vector<uint8_t*>* const cleared_cards_;
};

class ModUnionUpdateObjectReferencesVisitor {
 public:
  ModUnionUpdateObjectReferencesVisitor(MarkObjectVisitor* visitor,
                                        space::ContinuousSpace* from_space,
                                        space::ContinuousSpace* immune_space,
                                        bool* contains_reference_to_other_space)
    : visitor_(visitor),
      from_space_(from_space),
      immune_space_(immune_space),
      contains_reference_to_other_space_(contains_reference_to_other_space) {}

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    MarkReference(obj->GetFieldObjectReferenceAddr(offset));
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    VisitRoot(root);
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    MarkReference(root);
  }

 private:
  template<bool kPoisonReferences>
  void MarkReference(mirror::ObjectReference<kPoisonReferences, mirror::Object>* obj_ptr) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    // Only add the reference if it is non null and fits our criteria.
    mirror::Object* ref = obj_ptr->AsMirrorPtr();
    if (ref != nullptr && !from_space_->HasAddress(ref) && !immune_space_->HasAddress(ref)) {
      *contains_reference_to_other_space_ = true;
      mirror::Object* new_object = visitor_->MarkObject(ref);
      if (ref != new_object) {
        obj_ptr->Assign(new_object);
      }
    }
  }

  MarkObjectVisitor* const visitor_;
  // Space which we are scanning
  space::ContinuousSpace* const from_space_;
  space::ContinuousSpace* const immune_space_;
  // Set if we have any references to another space.
  bool* const contains_reference_to_other_space_;
};

class ModUnionScanImageRootVisitor {
 public:
  // Immune space is any other space which we don't care about references to. Currently this is
  // the image space in the case of the zygote mod union table.
  ModUnionScanImageRootVisitor(MarkObjectVisitor* visitor,
                               space::ContinuousSpace* from_space,
                               space::ContinuousSpace* immune_space,
                               bool* contains_reference_to_other_space)
      : visitor_(visitor),
        from_space_(from_space),
        immune_space_(immune_space),
        contains_reference_to_other_space_(contains_reference_to_other_space) {}

  void operator()(mirror::Object* root) const
      REQUIRES(Locks::heap_bitmap_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(root != nullptr);
    ModUnionUpdateObjectReferencesVisitor ref_visitor(visitor_,
                                                      from_space_,
                                                      immune_space_,
                                                      contains_reference_to_other_space_);
    root->VisitReferences(ref_visitor, VoidFunctor());
  }

 private:
  MarkObjectVisitor* const visitor_;
  // Space which we are scanning
  space::ContinuousSpace* const from_space_;
  space::ContinuousSpace* const immune_space_;
  // Set if we have any references to another space.
  bool* const contains_reference_to_other_space_;
};

void ModUnionTableReferenceCache::ClearCards() {
  CardTable* card_table = GetHeap()->GetCardTable();
  ModUnionAddToCardSetVisitor visitor(&cleared_cards_);
  // Clear dirty cards in the this space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space_->Begin(), space_->End(), AgeCardVisitor(), visitor);
}

class AddToReferenceArrayVisitor {
 public:
  AddToReferenceArrayVisitor(ModUnionTableReferenceCache* mod_union_table,
                             MarkObjectVisitor* visitor,
                             std::vector<mirror::HeapReference<mirror::Object>*>* references,
                             bool* has_target_reference)
      : mod_union_table_(mod_union_table),
        visitor_(visitor),
        references_(references),
        has_target_reference_(has_target_reference) {}

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    mirror::HeapReference<mirror::Object>* ref_ptr = obj->GetFieldObjectReferenceAddr(offset);
    mirror::Object* ref = ref_ptr->AsMirrorPtr();
    // Only add the reference if it is non null and fits our criteria.
    if (ref != nullptr && mod_union_table_->ShouldAddReference(ref)) {
      // Push the adddress of the reference.
      references_->push_back(ref_ptr);
    }
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (mod_union_table_->ShouldAddReference(root->AsMirrorPtr())) {
      *has_target_reference_ = true;
      // TODO: Add MarkCompressedReference callback here.
      mirror::Object* old_ref = root->AsMirrorPtr();
      mirror::Object* new_ref = visitor_->MarkObject(old_ref);
      if (old_ref != new_ref) {
        root->Assign(new_ref);
      }
    }
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  MarkObjectVisitor* const visitor_;
  std::vector<mirror::HeapReference<mirror::Object>*>* const references_;
  bool* const has_target_reference_;
};

class ModUnionReferenceVisitor {
 public:
  ModUnionReferenceVisitor(ModUnionTableReferenceCache* const mod_union_table,
                           MarkObjectVisitor* visitor,
                           std::vector<mirror::HeapReference<mirror::Object>*>* references,
                           bool* has_target_reference)
      : mod_union_table_(mod_union_table),
        visitor_(visitor),
        references_(references),
        has_target_reference_(has_target_reference) {}

  void operator()(mirror::Object* obj) const
      SHARED_REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    // We don't have an early exit since we use the visitor pattern, an early
    // exit should significantly speed this up.
    AddToReferenceArrayVisitor visitor(mod_union_table_,
                                       visitor_,
                                       references_,
                                       has_target_reference_);
    obj->VisitReferences(visitor, VoidFunctor());
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  MarkObjectVisitor* const visitor_;
  std::vector<mirror::HeapReference<mirror::Object>*>* const references_;
  bool* const has_target_reference_;
};

class CheckReferenceVisitor {
 public:
  CheckReferenceVisitor(ModUnionTableReferenceCache* mod_union_table,
                        const std::set<mirror::Object*>& references)
      : mod_union_table_(mod_union_table),
        references_(references) {}

  // Extra parameters are required since we use this same visitor signature for checking objects.
  void operator()(mirror::Object* obj, MemberOffset offset, bool is_static ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
    mirror::Object* ref = obj->GetFieldObject<mirror::Object>(offset);
    if (ref != nullptr &&
        mod_union_table_->ShouldAddReference(ref) &&
        references_.find(ref) == references_.end()) {
      Heap* heap = mod_union_table_->GetHeap();
      space::ContinuousSpace* from_space = heap->FindContinuousSpaceFromObject(obj, false);
      space::ContinuousSpace* to_space = heap->FindContinuousSpaceFromObject(ref, false);
      LOG(INFO) << "Object " << reinterpret_cast<const void*>(obj) << "(" << PrettyTypeOf(obj)
          << ")" << "References " << reinterpret_cast<const void*>(ref) << "(" << PrettyTypeOf(ref)
          << ") without being in mod-union table";
      LOG(INFO) << "FromSpace " << from_space->GetName() << " type "
          << from_space->GetGcRetentionPolicy();
      LOG(INFO) << "ToSpace " << to_space->GetName() << " type "
          << to_space->GetGcRetentionPolicy();
      heap->DumpSpaces(LOG(INFO));
      LOG(FATAL) << "FATAL ERROR";
    }
  }

  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (kIsDebugBuild && !root->IsNull()) {
      VisitRoot(root);
    }
  }

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(!mod_union_table_->ShouldAddReference(root->AsMirrorPtr()));
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  const std::set<mirror::Object*>& references_;
};

class ModUnionCheckReferences {
 public:
  ModUnionCheckReferences(ModUnionTableReferenceCache* mod_union_table,
                          const std::set<mirror::Object*>& references)
      REQUIRES(Locks::heap_bitmap_lock_)
      : mod_union_table_(mod_union_table), references_(references) {}

  void operator()(mirror::Object* obj) const NO_THREAD_SAFETY_ANALYSIS {
    Locks::heap_bitmap_lock_->AssertSharedHeld(Thread::Current());
    CheckReferenceVisitor visitor(mod_union_table_, references_);
    obj->VisitReferences(visitor, VoidFunctor());
  }

 private:
  ModUnionTableReferenceCache* const mod_union_table_;
  const std::set<mirror::Object*>& references_;
};

void ModUnionTableReferenceCache::Verify() {
  // Start by checking that everything in the mod union table is marked.
  for (const auto& ref_pair : references_) {
    for (mirror::HeapReference<mirror::Object>* ref : ref_pair.second) {
      CHECK(heap_->IsLiveObjectLocked(ref->AsMirrorPtr()));
    }
  }

  // Check the references of each clean card which is also in the mod union table.
  CardTable* card_table = heap_->GetCardTable();
  ContinuousSpaceBitmap* live_bitmap = space_->GetLiveBitmap();
  for (const auto& ref_pair : references_) {
    const uint8_t* card = ref_pair.first;
    if (*card == CardTable::kCardClean) {
      std::set<mirror::Object*> reference_set;
      for (mirror::HeapReference<mirror::Object>* obj_ptr : ref_pair.second) {
        reference_set.insert(obj_ptr->AsMirrorPtr());
      }
      ModUnionCheckReferences visitor(this, reference_set);
      uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
      live_bitmap->VisitMarkedRange(start, start + CardTable::kCardSize, visitor);
    }
  }
}

void ModUnionTableReferenceCache::Dump(std::ostream& os) {
  CardTable* card_table = heap_->GetCardTable();
  os << "ModUnionTable cleared cards: [";
  for (uint8_t* card_addr : cleared_cards_) {
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    uintptr_t end = start + CardTable::kCardSize;
    os << reinterpret_cast<void*>(start) << "-" << reinterpret_cast<void*>(end) << ",";
  }
  os << "]\nModUnionTable references: [";
  for (const auto& ref_pair : references_) {
    const uint8_t* card_addr = ref_pair.first;
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card_addr));
    uintptr_t end = start + CardTable::kCardSize;
    os << reinterpret_cast<void*>(start) << "-" << reinterpret_cast<void*>(end) << "->{";
    for (mirror::HeapReference<mirror::Object>* ref : ref_pair.second) {
      os << reinterpret_cast<const void*>(ref->AsMirrorPtr()) << ",";
    }
    os << "},";
  }
}

void ModUnionTableReferenceCache::UpdateAndMarkReferences(MarkObjectVisitor* visitor) {
  CardTable* const card_table = heap_->GetCardTable();
  std::vector<mirror::HeapReference<mirror::Object>*> cards_references;
  // If has_target_reference is true then there was a GcRoot compressed reference which wasn't
  // added. In this case we need to keep the card dirty.
  // We don't know if the GcRoot addresses will remain constant, for example, classloaders have a
  // hash set of GcRoot which may be resized or modified.
  bool has_target_reference;
  ModUnionReferenceVisitor add_visitor(this, visitor, &cards_references, &has_target_reference);
  CardSet new_cleared_cards;
  for (uint8_t* card : cleared_cards_) {
    // Clear and re-compute alloc space references associated with this card.
    cards_references.clear();
    has_target_reference = false;
    uintptr_t start = reinterpret_cast<uintptr_t>(card_table->AddrFromCard(card));
    uintptr_t end = start + CardTable::kCardSize;
    space::ContinuousSpace* space =
        heap_->FindContinuousSpaceFromObject(reinterpret_cast<mirror::Object*>(start), false);
    DCHECK(space != nullptr);
    ContinuousSpaceBitmap* live_bitmap = space->GetLiveBitmap();
    live_bitmap->VisitMarkedRange(start, end, add_visitor);
    // Update the corresponding references for the card.
    auto found = references_.find(card);
    if (found == references_.end()) {
      // Don't add card for an empty reference array.
      if (!cards_references.empty()) {
        references_.Put(card, cards_references);
      }
    } else {
      if (cards_references.empty()) {
        references_.erase(found);
      } else {
        found->second = cards_references;
      }
    }
    if (has_target_reference) {
      // Keep this card for next time since it contains a GcRoot which matches the
      // ShouldAddReference criteria. This usually occurs for class loaders.
      new_cleared_cards.insert(card);
    }
  }
  cleared_cards_ = std::move(new_cleared_cards);
  size_t count = 0;
  for (auto it = references_.begin(); it != references_.end();) {
    std::vector<mirror::HeapReference<mirror::Object>*>& references = it->second;
    // Since there is no card mark for setting a reference to null, we check each reference.
    // If all of the references of a card are null then we can remove that card. This is racy
    // with the mutators, but handled by rescanning dirty cards.
    bool all_null = true;
    for (mirror::HeapReference<mirror::Object>* obj_ptr : references) {
      if (obj_ptr->AsMirrorPtr() != nullptr) {
        all_null = false;
        visitor->MarkHeapReference(obj_ptr);
      }
    }
    count += references.size();
    if (!all_null) {
      ++it;
    } else {
      // All null references, erase the array from the set.
      it = references_.erase(it);
    }
  }
  if (VLOG_IS_ON(heap)) {
    VLOG(gc) << "Marked " << count << " references in mod union table";
  }
}

ModUnionTableCardCache::ModUnionTableCardCache(const std::string& name,
                                               Heap* heap,
                                               space::ContinuousSpace* space)
    : ModUnionTable(name, heap, space) {
  // Normally here we could use End() instead of Limit(), but for testing we may want to have a
  // mod-union table for a space which can still grow.
  if (!space->IsImageSpace()) {
    CHECK_ALIGNED(reinterpret_cast<uintptr_t>(space->Limit()), CardTable::kCardSize);
  }
  card_bitmap_.reset(CardBitmap::Create(
      "mod union bitmap", reinterpret_cast<uintptr_t>(space->Begin()),
      RoundUp(reinterpret_cast<uintptr_t>(space->Limit()), CardTable::kCardSize)));
}

class CardBitVisitor {
 public:
  CardBitVisitor(MarkObjectVisitor* visitor,
                 space::ContinuousSpace* space,
                 space::ContinuousSpace* immune_space,
                 ModUnionTable::CardBitmap* card_bitmap)
      : visitor_(visitor),
        space_(space),
        immune_space_(immune_space),
        bitmap_(space->GetLiveBitmap()),
        card_bitmap_(card_bitmap) {
    DCHECK(immune_space_ != nullptr);
  }

  void operator()(size_t bit_index) const {
    const uintptr_t start = card_bitmap_->AddrFromBitIndex(bit_index);
    DCHECK(space_->HasAddress(reinterpret_cast<mirror::Object*>(start)))
        << start << " " << *space_;
    bool reference_to_other_space = false;
    ModUnionScanImageRootVisitor scan_visitor(visitor_, space_, immune_space_,
                                              &reference_to_other_space);
    bitmap_->VisitMarkedRange(start, start + CardTable::kCardSize, scan_visitor);
    if (!reference_to_other_space) {
      // No non null reference to another space, clear the bit.
      card_bitmap_->ClearBit(bit_index);
    }
  }

 private:
  MarkObjectVisitor* const visitor_;
  space::ContinuousSpace* const space_;
  space::ContinuousSpace* const immune_space_;
  ContinuousSpaceBitmap* const bitmap_;
  ModUnionTable::CardBitmap* const card_bitmap_;
};

void ModUnionTableCardCache::ClearCards() {
  CardTable* const card_table = GetHeap()->GetCardTable();
  ModUnionAddToCardBitmapVisitor visitor(card_bitmap_.get(), card_table);
  // Clear dirty cards in the this space and update the corresponding mod-union bits.
  card_table->ModifyCardsAtomic(space_->Begin(), space_->End(), AgeCardVisitor(), visitor);
}

// Mark all references to the alloc space(s).
void ModUnionTableCardCache::UpdateAndMarkReferences(MarkObjectVisitor* visitor) {
  // TODO: Needs better support for multi-images? b/26317072
  space::ImageSpace* image_space =
      heap_->GetBootImageSpaces().empty() ? nullptr : heap_->GetBootImageSpaces()[0];
  // If we don't have an image space, just pass in space_ as the immune space. Pass in the same
  // space_ instead of image_space to avoid a null check in ModUnionUpdateObjectReferencesVisitor.
  CardBitVisitor bit_visitor(visitor, space_, image_space != nullptr ? image_space : space_,
      card_bitmap_.get());
  card_bitmap_->VisitSetBits(
      0, RoundUp(space_->Size(), CardTable::kCardSize) / CardTable::kCardSize, bit_visitor);
}

void ModUnionTableCardCache::Dump(std::ostream& os) {
  os << "ModUnionTable dirty cards: [";
  // TODO: Find cleaner way of doing this.
  for (uint8_t* addr = space_->Begin(); addr < AlignUp(space_->End(), CardTable::kCardSize);
      addr += CardTable::kCardSize) {
    if (card_bitmap_->Test(reinterpret_cast<uintptr_t>(addr))) {
      os << reinterpret_cast<void*>(addr) << "-"
         << reinterpret_cast<void*>(addr + CardTable::kCardSize) << "\n";
    }
  }
  os << "]";
}

void ModUnionTableCardCache::SetCards() {
  // Only clean up to the end since there cannot be any objects past the End() of the space.
  for (uint8_t* addr = space_->Begin(); addr < AlignUp(space_->End(), CardTable::kCardSize);
       addr += CardTable::kCardSize) {
    card_bitmap_->Set(reinterpret_cast<uintptr_t>(addr));
  }
}

bool ModUnionTableCardCache::ContainsCardFor(uintptr_t addr) {
  return card_bitmap_->Test(addr);
}

void ModUnionTableReferenceCache::SetCards() {
  for (uint8_t* addr = space_->Begin(); addr < AlignUp(space_->End(), CardTable::kCardSize);
       addr += CardTable::kCardSize) {
    cleared_cards_.insert(heap_->GetCardTable()->CardFromAddr(reinterpret_cast<void*>(addr)));
  }
}

bool ModUnionTableReferenceCache::ContainsCardFor(uintptr_t addr) {
  auto* card_ptr = heap_->GetCardTable()->CardFromAddr(reinterpret_cast<void*>(addr));
  return cleared_cards_.find(card_ptr) != cleared_cards_.end() ||
      references_.find(card_ptr) != references_.end();
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
