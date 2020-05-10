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

#include "mod_union_table-inl.h"

#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "gc/space/space-inl.h"
#include "mirror/array-inl.h"
#include "space_bitmap-inl.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {
namespace gc {
namespace accounting {

class ModUnionTableFactory {
 public:
  enum TableType {
    kTableTypeCardCache,
    kTableTypeReferenceCache,
    kTableTypeCount,  // Number of values in the enum.
  };

  // Target space is ignored for the card cache implementation.
  static ModUnionTable* Create(
      TableType type, space::ContinuousSpace* space, space::ContinuousSpace* target_space);
};

class ModUnionTableTest : public CommonRuntimeTest {
 public:
  ModUnionTableTest() : java_lang_object_array_(nullptr) {
  }
  mirror::ObjectArray<mirror::Object>* AllocObjectArray(
      Thread* self, space::ContinuousMemMapAllocSpace* space, size_t component_count)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    auto* klass = GetObjectArrayClass(self, space);
    const size_t size = mirror::ComputeArraySize(component_count, 2);
    size_t bytes_allocated = 0, bytes_tl_bulk_allocated;
    auto* obj = down_cast<mirror::ObjectArray<mirror::Object>*>(
        space->Alloc(self, size, &bytes_allocated, nullptr, &bytes_tl_bulk_allocated));
    if (obj != nullptr) {
      obj->SetClass(klass);
      obj->SetLength(static_cast<int32_t>(component_count));
      space->GetLiveBitmap()->Set(obj);
      EXPECT_GE(bytes_allocated, size);
    }
    return obj;
  }
  void ResetClass() {
    java_lang_object_array_ = nullptr;
  }
  void RunTest(ModUnionTableFactory::TableType type);

 private:
  mirror::Class* GetObjectArrayClass(Thread* self, space::ContinuousMemMapAllocSpace* space)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (java_lang_object_array_ == nullptr) {
      java_lang_object_array_ =
          Runtime::Current()->GetClassLinker()->GetClassRoot(ClassLinker::kObjectArrayClass);
      // Since the test doesn't have an image, the class of the object array keeps cards live
      // inside the card cache mod-union table and causes the check
      // ASSERT_FALSE(table->ContainsCardFor(reinterpret_cast<uintptr_t>(obj3)));
      // to fail since the class ends up keeping the card dirty. To get around this, we make a fake
      // copy of the class in the same space that we are allocating in.
      DCHECK(java_lang_object_array_ != nullptr);
      const size_t class_size = java_lang_object_array_->GetClassSize();
      size_t bytes_allocated = 0, bytes_tl_bulk_allocated;
      auto* klass = down_cast<mirror::Class*>(space->Alloc(self, class_size, &bytes_allocated,
                                                           nullptr,
                                                           &bytes_tl_bulk_allocated));
      DCHECK(klass != nullptr);
      memcpy(klass, java_lang_object_array_, class_size);
      Runtime::Current()->GetHeap()->GetCardTable()->MarkCard(klass);
      java_lang_object_array_ = klass;
    }
    return java_lang_object_array_;
  }
  mirror::Class* java_lang_object_array_;
};

// Collect visited objects into container.
class CollectVisitedVisitor : public MarkObjectVisitor {
 public:
  explicit CollectVisitedVisitor(std::set<mirror::Object*>* out) : out_(out) {}
  virtual void MarkHeapReference(mirror::HeapReference<mirror::Object>* ref) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(ref != nullptr);
    MarkObject(ref->AsMirrorPtr());
  }
  virtual mirror::Object* MarkObject(mirror::Object* obj) OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(obj != nullptr);
    out_->insert(obj);
    return obj;
  }

 private:
  std::set<mirror::Object*>* const out_;
};

// A mod union table that only holds references to a specified target space.
class ModUnionTableRefCacheToSpace : public ModUnionTableReferenceCache {
 public:
  explicit ModUnionTableRefCacheToSpace(
      const std::string& name, Heap* heap, space::ContinuousSpace* space,
      space::ContinuousSpace* target_space)
      : ModUnionTableReferenceCache(name, heap, space), target_space_(target_space) {}

  bool ShouldAddReference(const mirror::Object* ref) const OVERRIDE {
    return target_space_->HasAddress(ref);
  }

 private:
  space::ContinuousSpace* const target_space_;
};

std::ostream& operator<<(std::ostream& oss, ModUnionTableFactory::TableType type) {
  switch (type) {
    case ModUnionTableFactory::kTableTypeCardCache: {
      oss << "CardCache";
      break;
    }
    case ModUnionTableFactory::kTableTypeReferenceCache: {
      oss << "ReferenceCache";
      break;
    }
    default: {
      UNIMPLEMENTED(FATAL) << static_cast<size_t>(type);
    }
  }
  return oss;
}

ModUnionTable* ModUnionTableFactory::Create(
    TableType type, space::ContinuousSpace* space, space::ContinuousSpace* target_space) {
  std::ostringstream name;
  name << "Mod union table: " << type;
  switch (type) {
    case kTableTypeCardCache: {
      return new ModUnionTableCardCache(name.str(), Runtime::Current()->GetHeap(), space);
    }
    case kTableTypeReferenceCache: {
      return new ModUnionTableRefCacheToSpace(name.str(), Runtime::Current()->GetHeap(), space,
                                              target_space);
    }
    default: {
      UNIMPLEMENTED(FATAL) << "Invalid type " << type;
    }
  }
  return nullptr;
}

TEST_F(ModUnionTableTest, TestCardCache) {
  RunTest(ModUnionTableFactory::kTableTypeCardCache);
}

TEST_F(ModUnionTableTest, TestReferenceCache) {
  RunTest(ModUnionTableFactory::kTableTypeReferenceCache);
}

void ModUnionTableTest::RunTest(ModUnionTableFactory::TableType type) {
  Thread* const self = Thread::Current();
  ScopedObjectAccess soa(self);
  Runtime* const runtime = Runtime::Current();
  gc::Heap* const heap = runtime->GetHeap();
  // Use non moving space since moving GC don't necessarily have a primary free list space.
  auto* space = heap->GetNonMovingSpace();
  ResetClass();
  // Create another space that we can put references in.
  std::unique_ptr<space::DlMallocSpace> other_space(space::DlMallocSpace::Create(
      "other space", 128 * KB, 4 * MB, 4 * MB, nullptr, false));
  ASSERT_TRUE(other_space.get() != nullptr);
  {
    ScopedThreadSuspension sts(self, kSuspended);
    ScopedSuspendAll ssa("Add image space");
    heap->AddSpace(other_space.get());
  }
  std::unique_ptr<ModUnionTable> table(ModUnionTableFactory::Create(
      type, space, other_space.get()));
  ASSERT_TRUE(table.get() != nullptr);
  // Create some fake objects and put the main space and dirty cards in the non moving space.
  auto* obj1 = AllocObjectArray(self, space, CardTable::kCardSize);
  ASSERT_TRUE(obj1 != nullptr);
  auto* obj2 = AllocObjectArray(self, space, CardTable::kCardSize);
  ASSERT_TRUE(obj2 != nullptr);
  auto* obj3 = AllocObjectArray(self, space, CardTable::kCardSize);
  ASSERT_TRUE(obj3 != nullptr);
  auto* obj4 = AllocObjectArray(self, space, CardTable::kCardSize);
  ASSERT_TRUE(obj4 != nullptr);
  // Dirty some cards.
  obj1->Set(0, obj2);
  obj2->Set(0, obj3);
  obj3->Set(0, obj4);
  obj4->Set(0, obj1);
  // Dirty some more cards to objects in another space.
  auto* other_space_ref1 = AllocObjectArray(self, other_space.get(), CardTable::kCardSize);
  ASSERT_TRUE(other_space_ref1 != nullptr);
  auto* other_space_ref2 = AllocObjectArray(self, other_space.get(), CardTable::kCardSize);
  ASSERT_TRUE(other_space_ref2 != nullptr);
  obj1->Set(1, other_space_ref1);
  obj2->Set(3, other_space_ref2);
  table->ClearCards();
  std::set<mirror::Object*> visited_before;
  CollectVisitedVisitor collector_before(&visited_before);
  table->UpdateAndMarkReferences(&collector_before);
  // Check that we visited all the references in other spaces only.
  ASSERT_GE(visited_before.size(), 2u);
  ASSERT_TRUE(visited_before.find(other_space_ref1) != visited_before.end());
  ASSERT_TRUE(visited_before.find(other_space_ref2) != visited_before.end());
  // Verify that all the other references were visited.
  // obj1, obj2 cards should still be in mod union table since they have references to other
  // spaces.
  ASSERT_TRUE(table->ContainsCardFor(reinterpret_cast<uintptr_t>(obj1)));
  ASSERT_TRUE(table->ContainsCardFor(reinterpret_cast<uintptr_t>(obj2)));
  // obj3, obj4 don't have a reference to any object in the other space, their cards should have
  // been removed from the mod union table during UpdateAndMarkReferences.
  ASSERT_FALSE(table->ContainsCardFor(reinterpret_cast<uintptr_t>(obj3)));
  ASSERT_FALSE(table->ContainsCardFor(reinterpret_cast<uintptr_t>(obj4)));
  {
    // Currently no-op, make sure it still works however.
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    table->Verify();
  }
  // Verify that dump doesn't crash.
  std::ostringstream oss;
  table->Dump(oss);
  // Set all the cards, then verify.
  table->SetCards();
  // TODO: Check that the cards are actually set.
  for (auto* ptr = space->Begin(); ptr < AlignUp(space->End(), CardTable::kCardSize);
      ptr += CardTable::kCardSize) {
    ASSERT_TRUE(table->ContainsCardFor(reinterpret_cast<uintptr_t>(ptr)));
  }
  // Visit again and make sure the cards got cleared back to their sane state.
  std::set<mirror::Object*> visited_after;
  CollectVisitedVisitor collector_after(&visited_after);
  table->UpdateAndMarkReferences(&collector_after);
  // Check that we visited a superset after.
  for (auto* obj : visited_before) {
    ASSERT_TRUE(visited_after.find(obj) != visited_after.end()) << obj;
  }
  // Verify that the dump still works.
  std::ostringstream oss2;
  table->Dump(oss2);
  // Remove the space we added so it doesn't persist to the next test.
  ScopedThreadSuspension sts(self, kSuspended);
  ScopedSuspendAll ssa("Add image space");
  heap->RemoveSpace(other_space.get());
}

}  // namespace accounting
}  // namespace gc
}  // namespace art
