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

#include "intern_table.h"

#include <memory>

#include "gc_root-inl.h"
#include "gc/collector/garbage_collector.h"
#include "gc/space/image_space.h"
#include "gc/weak_root_state.h"
#include "image-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "mirror/string-inl.h"
#include "thread.h"
#include "utf.h"

namespace art {

InternTable::InternTable()
    : images_added_to_intern_table_(false),
      log_new_roots_(false),
      weak_intern_condition_("New intern condition", *Locks::intern_table_lock_),
      weak_root_state_(gc::kWeakRootStateNormal) {
}

size_t InternTable::Size() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.Size() + weak_interns_.Size();
}

size_t InternTable::StrongSize() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.Size();
}

size_t InternTable::WeakSize() const {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return weak_interns_.Size();
}

void InternTable::DumpForSigQuit(std::ostream& os) const {
  os << "Intern table: " << StrongSize() << " strong; " << WeakSize() << " weak\n";
}

void InternTable::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  if ((flags & kVisitRootFlagAllRoots) != 0) {
    strong_interns_.VisitRoots(visitor);
  } else if ((flags & kVisitRootFlagNewRoots) != 0) {
    for (auto& root : new_strong_intern_roots_) {
      mirror::String* old_ref = root.Read<kWithoutReadBarrier>();
      root.VisitRoot(visitor, RootInfo(kRootInternedString));
      mirror::String* new_ref = root.Read<kWithoutReadBarrier>();
      if (new_ref != old_ref) {
        // The GC moved a root in the log. Need to search the strong interns and update the
        // corresponding object. This is slow, but luckily for us, this may only happen with a
        // concurrent moving GC.
        strong_interns_.Remove(old_ref);
        strong_interns_.Insert(new_ref);
      }
    }
  }
  if ((flags & kVisitRootFlagClearRootLog) != 0) {
    new_strong_intern_roots_.clear();
  }
  if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
    log_new_roots_ = true;
  } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
    log_new_roots_ = false;
  }
  // Note: we deliberately don't visit the weak_interns_ table and the immutable image roots.
}

mirror::String* InternTable::LookupWeak(Thread* self, mirror::String* s) {
  MutexLock mu(self, *Locks::intern_table_lock_);
  return LookupWeakLocked(s);
}

mirror::String* InternTable::LookupStrong(Thread* self, mirror::String* s) {
  MutexLock mu(self, *Locks::intern_table_lock_);
  return LookupStrongLocked(s);
}

mirror::String* InternTable::LookupStrong(Thread* self,
                                          uint32_t utf16_length,
                                          const char* utf8_data) {
  DCHECK_EQ(utf16_length, CountModifiedUtf8Chars(utf8_data));
  Utf8String string(utf16_length,
                    utf8_data,
                    ComputeUtf16HashFromModifiedUtf8(utf8_data, utf16_length));
  MutexLock mu(self, *Locks::intern_table_lock_);
  return strong_interns_.Find(string);
}

mirror::String* InternTable::LookupWeakLocked(mirror::String* s) {
  return weak_interns_.Find(s);
}

mirror::String* InternTable::LookupStrongLocked(mirror::String* s) {
  return strong_interns_.Find(s);
}

void InternTable::AddNewTable() {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  weak_interns_.AddNewTable();
  strong_interns_.AddNewTable();
}

mirror::String* InternTable::InsertStrong(mirror::String* s) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordStrongStringInsertion(s);
  }
  if (log_new_roots_) {
    new_strong_intern_roots_.push_back(GcRoot<mirror::String>(s));
  }
  strong_interns_.Insert(s);
  return s;
}

mirror::String* InternTable::InsertWeak(mirror::String* s) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordWeakStringInsertion(s);
  }
  weak_interns_.Insert(s);
  return s;
}

void InternTable::RemoveStrong(mirror::String* s) {
  strong_interns_.Remove(s);
}

void InternTable::RemoveWeak(mirror::String* s) {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    runtime->RecordWeakStringRemoval(s);
  }
  weak_interns_.Remove(s);
}

// Insert/remove methods used to undo changes made during an aborted transaction.
mirror::String* InternTable::InsertStrongFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  return InsertStrong(s);
}
mirror::String* InternTable::InsertWeakFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  return InsertWeak(s);
}
void InternTable::RemoveStrongFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  RemoveStrong(s);
}
void InternTable::RemoveWeakFromTransaction(mirror::String* s) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  RemoveWeak(s);
}

void InternTable::AddImagesStringsToTable(const std::vector<gc::space::ImageSpace*>& image_spaces) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  for (gc::space::ImageSpace* image_space : image_spaces) {
    const ImageHeader* const header = &image_space->GetImageHeader();
    // Check if we have the interned strings section.
    const ImageSection& section = header->GetImageSection(ImageHeader::kSectionInternedStrings);
    if (section.Size() > 0) {
      AddTableFromMemoryLocked(image_space->Begin() + section.Offset());
    } else {
      // TODO: Delete this logic?
      mirror::Object* root = header->GetImageRoot(ImageHeader::kDexCaches);
      mirror::ObjectArray<mirror::DexCache>* dex_caches = root->AsObjectArray<mirror::DexCache>();
      for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
        mirror::DexCache* dex_cache = dex_caches->Get(i);
        const size_t num_strings = dex_cache->NumStrings();
        for (size_t j = 0; j < num_strings; ++j) {
          mirror::String* image_string = dex_cache->GetResolvedString(j);
          if (image_string != nullptr) {
            mirror::String* found = LookupStrongLocked(image_string);
            if (found == nullptr) {
              InsertStrong(image_string);
            } else {
              DCHECK_EQ(found, image_string);
            }
          }
        }
      }
    }
  }
  images_added_to_intern_table_ = true;
}

mirror::String* InternTable::LookupStringFromImage(mirror::String* s) {
  DCHECK(!images_added_to_intern_table_);
  const std::vector<gc::space::ImageSpace*>& image_spaces =
      Runtime::Current()->GetHeap()->GetBootImageSpaces();
  if (image_spaces.empty()) {
    return nullptr;  // No image present.
  }
  const std::string utf8 = s->ToModifiedUtf8();
  for (gc::space::ImageSpace* image_space : image_spaces) {
    mirror::Object* root = image_space->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
    mirror::ObjectArray<mirror::DexCache>* dex_caches = root->AsObjectArray<mirror::DexCache>();
    for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
      mirror::DexCache* dex_cache = dex_caches->Get(i);
      const DexFile* dex_file = dex_cache->GetDexFile();
      // Binary search the dex file for the string index.
      const DexFile::StringId* string_id = dex_file->FindStringId(utf8.c_str());
      if (string_id != nullptr) {
        uint32_t string_idx = dex_file->GetIndexForStringId(*string_id);
        // GetResolvedString() contains a RB.
        mirror::String* image_string = dex_cache->GetResolvedString(string_idx);
        if (image_string != nullptr) {
          return image_string;
        }
      }
    }
  }
  return nullptr;
}

void InternTable::BroadcastForNewInterns() {
  CHECK(kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::intern_table_lock_);
  weak_intern_condition_.Broadcast(self);
}

void InternTable::WaitUntilAccessible(Thread* self) {
  Locks::intern_table_lock_->ExclusiveUnlock(self);
  {
    ScopedThreadSuspension sts(self, kWaitingWeakGcRootRead);
    MutexLock mu(self, *Locks::intern_table_lock_);
    while (weak_root_state_ == gc::kWeakRootStateNoReadsOrWrites) {
      weak_intern_condition_.Wait(self);
    }
  }
  Locks::intern_table_lock_->ExclusiveLock(self);
}

mirror::String* InternTable::Insert(mirror::String* s, bool is_strong, bool holding_locks) {
  if (s == nullptr) {
    return nullptr;
  }
  Thread* const self = Thread::Current();
  MutexLock mu(self, *Locks::intern_table_lock_);
  if (kDebugLocking && !holding_locks) {
    Locks::mutator_lock_->AssertSharedHeld(self);
    CHECK_EQ(2u, self->NumberOfHeldMutexes()) << "may only safely hold the mutator lock";
  }
  while (true) {
    if (holding_locks) {
      if (!kUseReadBarrier) {
        CHECK_EQ(weak_root_state_, gc::kWeakRootStateNormal);
      } else {
        CHECK(self->GetWeakRefAccessEnabled());
      }
    }
    // Check the strong table for a match.
    mirror::String* strong = LookupStrongLocked(s);
    if (strong != nullptr) {
      return strong;
    }
    if ((!kUseReadBarrier && weak_root_state_ != gc::kWeakRootStateNoReadsOrWrites) ||
        (kUseReadBarrier && self->GetWeakRefAccessEnabled())) {
      break;
    }
    // weak_root_state_ is set to gc::kWeakRootStateNoReadsOrWrites in the GC pause but is only
    // cleared after SweepSystemWeaks has completed. This is why we need to wait until it is
    // cleared.
    CHECK(!holding_locks);
    StackHandleScope<1> hs(self);
    auto h = hs.NewHandleWrapper(&s);
    WaitUntilAccessible(self);
  }
  if (!kUseReadBarrier) {
    CHECK_EQ(weak_root_state_, gc::kWeakRootStateNormal);
  } else {
    CHECK(self->GetWeakRefAccessEnabled());
  }
  // There is no match in the strong table, check the weak table.
  mirror::String* weak = LookupWeakLocked(s);
  if (weak != nullptr) {
    if (is_strong) {
      // A match was found in the weak table. Promote to the strong table.
      RemoveWeak(weak);
      return InsertStrong(weak);
    }
    return weak;
  }
  // Check the image for a match.
  if (!images_added_to_intern_table_) {
    mirror::String* const image_string = LookupStringFromImage(s);
    if (image_string != nullptr) {
      return is_strong ? InsertStrong(image_string) : InsertWeak(image_string);
    }
  }
  // No match in the strong table or the weak table. Insert into the strong / weak table.
  return is_strong ? InsertStrong(s) : InsertWeak(s);
}

mirror::String* InternTable::InternStrong(int32_t utf16_length, const char* utf8_data) {
  DCHECK(utf8_data != nullptr);
  return InternStrong(mirror::String::AllocFromModifiedUtf8(
      Thread::Current(), utf16_length, utf8_data));
}

mirror::String* InternTable::InternStrong(const char* utf8_data) {
  DCHECK(utf8_data != nullptr);
  return InternStrong(mirror::String::AllocFromModifiedUtf8(Thread::Current(), utf8_data));
}

mirror::String* InternTable::InternStrongImageString(mirror::String* s) {
  // May be holding the heap bitmap lock.
  return Insert(s, true, true);
}

mirror::String* InternTable::InternStrong(mirror::String* s) {
  return Insert(s, true, false);
}

mirror::String* InternTable::InternWeak(mirror::String* s) {
  return Insert(s, false, false);
}

bool InternTable::ContainsWeak(mirror::String* s) {
  return LookupWeak(Thread::Current(), s) == s;
}

void InternTable::SweepInternTableWeaks(IsMarkedVisitor* visitor) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  weak_interns_.SweepWeaks(visitor);
}

size_t InternTable::AddTableFromMemory(const uint8_t* ptr) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return AddTableFromMemoryLocked(ptr);
}

size_t InternTable::AddTableFromMemoryLocked(const uint8_t* ptr) {
  return strong_interns_.AddTableFromMemory(ptr);
}

size_t InternTable::WriteToMemory(uint8_t* ptr) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  return strong_interns_.WriteToMemory(ptr);
}

std::size_t InternTable::StringHashEquals::operator()(const GcRoot<mirror::String>& root) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  return static_cast<size_t>(root.Read()->GetHashCode());
}

bool InternTable::StringHashEquals::operator()(const GcRoot<mirror::String>& a,
                                               const GcRoot<mirror::String>& b) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  return a.Read()->Equals(b.Read());
}

bool InternTable::StringHashEquals::operator()(const GcRoot<mirror::String>& a,
                                               const Utf8String& b) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  mirror::String* a_string = a.Read();
  uint32_t a_length = static_cast<uint32_t>(a_string->GetLength());
  if (a_length != b.GetUtf16Length()) {
    return false;
  }
  const uint16_t* a_value = a_string->GetValue();
  return CompareModifiedUtf8ToUtf16AsCodePointValues(b.GetUtf8Data(), a_value, a_length) == 0;
}

size_t InternTable::Table::AddTableFromMemory(const uint8_t* ptr) {
  size_t read_count = 0;
  UnorderedSet set(ptr, /*make copy*/false, &read_count);
  if (set.Empty()) {
    // Avoid inserting empty sets.
    return read_count;
  }
  // TODO: Disable this for app images if app images have intern tables.
  static constexpr bool kCheckDuplicates = true;
  if (kCheckDuplicates) {
    for (GcRoot<mirror::String>& string : set) {
      CHECK(Find(string.Read()) == nullptr) << "Already found " << string.Read()->ToModifiedUtf8();
    }
  }
  // Insert at the front since we add new interns into the back.
  tables_.insert(tables_.begin(), std::move(set));
  return read_count;
}

size_t InternTable::Table::WriteToMemory(uint8_t* ptr) {
  if (tables_.empty()) {
    return 0;
  }
  UnorderedSet* table_to_write;
  UnorderedSet combined;
  if (tables_.size() > 1) {
    table_to_write = &combined;
    for (UnorderedSet& table : tables_) {
      for (GcRoot<mirror::String>& string : table) {
        combined.Insert(string);
      }
    }
  } else {
    table_to_write = &tables_.back();
  }
  return table_to_write->WriteToMemory(ptr);
}

void InternTable::Table::Remove(mirror::String* s) {
  for (UnorderedSet& table : tables_) {
    auto it = table.Find(GcRoot<mirror::String>(s));
    if (it != table.end()) {
      table.Erase(it);
      return;
    }
  }
  LOG(FATAL) << "Attempting to remove non-interned string " << s->ToModifiedUtf8();
}

mirror::String* InternTable::Table::Find(mirror::String* s) {
  Locks::intern_table_lock_->AssertHeld(Thread::Current());
  for (UnorderedSet& table : tables_) {
    auto it = table.Find(GcRoot<mirror::String>(s));
    if (it != table.end()) {
      return it->Read();
    }
  }
  return nullptr;
}

mirror::String* InternTable::Table::Find(const Utf8String& string) {
  Locks::intern_table_lock_->AssertHeld(Thread::Current());
  for (UnorderedSet& table : tables_) {
    auto it = table.Find(string);
    if (it != table.end()) {
      return it->Read();
    }
  }
  return nullptr;
}

void InternTable::Table::AddNewTable() {
  tables_.push_back(UnorderedSet());
}

void InternTable::Table::Insert(mirror::String* s) {
  // Always insert the last table, the image tables are before and we avoid inserting into these
  // to prevent dirty pages.
  DCHECK(!tables_.empty());
  tables_.back().Insert(GcRoot<mirror::String>(s));
}

void InternTable::Table::VisitRoots(RootVisitor* visitor) {
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(
      visitor, RootInfo(kRootInternedString));
  for (UnorderedSet& table : tables_) {
    for (auto& intern : table) {
      buffered_visitor.VisitRoot(intern);
    }
  }
}

void InternTable::Table::SweepWeaks(IsMarkedVisitor* visitor) {
  for (UnorderedSet& table : tables_) {
    SweepWeaks(&table, visitor);
  }
}

void InternTable::Table::SweepWeaks(UnorderedSet* set, IsMarkedVisitor* visitor) {
  for (auto it = set->begin(), end = set->end(); it != end;) {
    // This does not need a read barrier because this is called by GC.
    mirror::Object* object = it->Read<kWithoutReadBarrier>();
    mirror::Object* new_object = visitor->IsMarked(object);
    if (new_object == nullptr) {
      it = set->Erase(it);
    } else {
      *it = GcRoot<mirror::String>(new_object->AsString());
      ++it;
    }
  }
}

size_t InternTable::Table::Size() const {
  return std::accumulate(tables_.begin(),
                         tables_.end(),
                         0U,
                         [](size_t sum, const UnorderedSet& set) {
                           return sum + set.Size();
                         });
}

void InternTable::ChangeWeakRootState(gc::WeakRootState new_state) {
  MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
  ChangeWeakRootStateLocked(new_state);
}

void InternTable::ChangeWeakRootStateLocked(gc::WeakRootState new_state) {
  CHECK(!kUseReadBarrier);
  weak_root_state_ = new_state;
  if (new_state != gc::kWeakRootStateNoReadsOrWrites) {
    weak_intern_condition_.Broadcast(Thread::Current());
  }
}

InternTable::Table::Table() {
  Runtime* const runtime = Runtime::Current();
  // Initial table.
  tables_.push_back(UnorderedSet());
  tables_.back().SetLoadFactor(runtime->GetHashTableMinLoadFactor(),
                               runtime->GetHashTableMaxLoadFactor());
}

}  // namespace art
