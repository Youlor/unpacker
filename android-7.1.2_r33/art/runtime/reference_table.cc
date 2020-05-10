/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "reference_table.h"

#include "base/mutex.h"
#include "indirect_reference_table.h"
#include "mirror/array.h"
#include "mirror/array-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/string-inl.h"
#include "runtime-inl.h"
#include "thread.h"
#include "utils.h"

namespace art {

ReferenceTable::ReferenceTable(const char* name, size_t initial_size, size_t max_size)
    : name_(name), max_size_(max_size) {
  CHECK_LE(initial_size, max_size);
  entries_.reserve(initial_size);
}

ReferenceTable::~ReferenceTable() {
}

void ReferenceTable::Add(mirror::Object* obj) {
  DCHECK(obj != nullptr);
  VerifyObject(obj);
  if (entries_.size() >= max_size_) {
    LOG(FATAL) << "ReferenceTable '" << name_ << "' "
               << "overflowed (" << max_size_ << " entries)";
  }
  entries_.push_back(GcRoot<mirror::Object>(obj));
}

void ReferenceTable::Remove(mirror::Object* obj) {
  // We iterate backwards on the assumption that references are LIFO.
  for (int i = entries_.size() - 1; i >= 0; --i) {
    mirror::Object* entry = entries_[i].Read();
    if (entry == obj) {
      entries_.erase(entries_.begin() + i);
      return;
    }
  }
}

// If "obj" is an array, return the number of elements in the array.
// Otherwise, return zero.
static size_t GetElementCount(mirror::Object* obj) SHARED_REQUIRES(Locks::mutator_lock_) {
  // We assume the special cleared value isn't an array in the if statement below.
  DCHECK(!Runtime::Current()->GetClearedJniWeakGlobal()->IsArrayInstance());
  if (obj == nullptr || !obj->IsArrayInstance()) {
    return 0;
  }
  return obj->AsArray()->GetLength();
}

// Log an object with some additional info.
//
// Pass in the number of elements in the array (or 0 if this is not an
// array object), and the number of additional objects that are identical
// or equivalent to the original.
static void DumpSummaryLine(std::ostream& os, mirror::Object* obj, size_t element_count,
                            int identical, int equiv)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (obj == nullptr) {
    os << "    null reference (count=" << equiv << ")\n";
    return;
  }
  if (Runtime::Current()->IsClearedJniWeakGlobal(obj)) {
    os << "    cleared jweak (count=" << equiv << ")\n";
    return;
  }

  std::string className(PrettyTypeOf(obj));
  if (obj->IsClass()) {
    // We're summarizing multiple instances, so using the exemplar
    // Class' type parameter here would be misleading.
    className = "java.lang.Class";
  }
  if (element_count != 0) {
    StringAppendF(&className, " (%zd elements)", element_count);
  }

  size_t total = identical + equiv + 1;
  std::string msg(StringPrintf("%5zd of %s", total, className.c_str()));
  if (identical + equiv != 0) {
    StringAppendF(&msg, " (%d unique instances)", equiv + 1);
  }
  os << "    " << msg << "\n";
}

size_t ReferenceTable::Size() const {
  return entries_.size();
}

void ReferenceTable::Dump(std::ostream& os) {
  os << name_ << " reference table dump:\n";
  Dump(os, entries_);
}

void ReferenceTable::Dump(std::ostream& os, Table& entries) {
  // Compare GC roots, first by class, then size, then address.
  struct GcRootComparator {
    bool operator()(GcRoot<mirror::Object> root1, GcRoot<mirror::Object> root2) const
      // TODO: enable analysis when analysis can work with the STL.
        NO_THREAD_SAFETY_ANALYSIS {
      Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
      // These GC roots are already forwarded in ReferenceTable::Dump. We sort by class since there
      // are no suspend points which can happen during the sorting process. This works since
      // we are guaranteed that the addresses of obj1, obj2, obj1->GetClass, obj2->GetClass wont
      // change during the sorting process. The classes are forwarded by ref->GetClass().
      mirror::Object* obj1 = root1.Read<kWithoutReadBarrier>();
      mirror::Object* obj2 = root2.Read<kWithoutReadBarrier>();
      DCHECK(obj1 != nullptr);
      DCHECK(obj2 != nullptr);
      Runtime* runtime = Runtime::Current();
      DCHECK(!runtime->IsClearedJniWeakGlobal(obj1));
      DCHECK(!runtime->IsClearedJniWeakGlobal(obj2));
      // Sort by class...
      if (obj1->GetClass() != obj2->GetClass()) {
        return obj1->GetClass() < obj2->GetClass();
      }
      // ...then by size...
      const size_t size1 = obj1->SizeOf();
      const size_t size2 = obj2->SizeOf();
      if (size1 != size2) {
        return size1 < size2;
      }
      // ...and finally by address.
      return obj1 < obj2;
    }
  };

  if (entries.empty()) {
    os << "  (empty)\n";
    return;
  }

  // Dump the most recent N entries.
  const size_t kLast = 10;
  size_t count = entries.size();
  int first = count - kLast;
  if (first < 0) {
    first = 0;
  }
  os << "  Last " << (count - first) << " entries (of " << count << "):\n";
  Runtime* runtime = Runtime::Current();
  for (int idx = count - 1; idx >= first; --idx) {
    mirror::Object* ref = entries[idx].Read();
    if (ref == nullptr) {
      continue;
    }
    if (runtime->IsClearedJniWeakGlobal(ref)) {
      os << StringPrintf("    %5d: cleared jweak\n", idx);
      continue;
    }
    if (ref->GetClass() == nullptr) {
      // should only be possible right after a plain dvmMalloc().
      size_t size = ref->SizeOf();
      os << StringPrintf("    %5d: %p (raw) (%zd bytes)\n", idx, ref, size);
      continue;
    }

    std::string className(PrettyTypeOf(ref));

    std::string extras;
    size_t element_count = GetElementCount(ref);
    if (element_count != 0) {
      StringAppendF(&extras, " (%zd elements)", element_count);
    } else if (ref->GetClass()->IsStringClass()) {
      mirror::String* s = ref->AsString();
      std::string utf8(s->ToModifiedUtf8());
      if (s->GetLength() <= 16) {
        StringAppendF(&extras, " \"%s\"", utf8.c_str());
      } else {
        StringAppendF(&extras, " \"%.16s... (%d chars)", utf8.c_str(), s->GetLength());
      }
    } else if (ref->IsReferenceInstance()) {
      mirror::Object* referent = ref->AsReference()->GetReferent();
      if (referent == nullptr) {
        extras = " (storing null)";
      } else {
        extras = StringPrintf(" (storing a %s)", PrettyTypeOf(referent).c_str());
      }
    }
    os << StringPrintf("    %5d: ", idx) << ref << " " << className << extras << "\n";
  }

  // Make a copy of the table and sort it, only adding non null and not cleared elements.
  Table sorted_entries;
  for (GcRoot<mirror::Object>& root : entries) {
    if (!root.IsNull() && !runtime->IsClearedJniWeakGlobal(root.Read())) {
      sorted_entries.push_back(root);
    }
  }
  if (sorted_entries.empty()) {
    return;
  }
  std::sort(sorted_entries.begin(), sorted_entries.end(), GcRootComparator());

  class SummaryElement {
   public:
    GcRoot<mirror::Object> root;
    size_t equiv;
    size_t identical;

    SummaryElement() : equiv(0), identical(0) {}
    SummaryElement(SummaryElement&& ref) {
      root = ref.root;
      equiv = ref.equiv;
      identical = ref.identical;
    }
    SummaryElement(const SummaryElement&) = default;
    SummaryElement& operator=(SummaryElement&&) = default;

    void Reset(GcRoot<mirror::Object>& _root) {
      root = _root;
      equiv = 0;
      identical = 0;
    }
  };
  std::vector<SummaryElement> sorted_summaries;
  {
    SummaryElement prev;

    for (GcRoot<mirror::Object>& root : sorted_entries) {
      mirror::Object* current = root.Read<kWithoutReadBarrier>();

      if (UNLIKELY(prev.root.IsNull())) {
        prev.Reset(root);
        continue;
      }

      mirror::Object* prevObj = prev.root.Read<kWithoutReadBarrier>();
      if (current == prevObj) {
        // Same reference, added more than once.
        ++prev.identical;
      } else if (current->GetClass() == prevObj->GetClass() &&
          GetElementCount(current) == GetElementCount(prevObj)) {
        // Same class / element count, different object.
        ++prev.equiv;
      } else {
        sorted_summaries.push_back(prev);
        prev.Reset(root);
      }
      prev.root = root;
    }
    sorted_summaries.push_back(prev);

    // Compare summary elements, first by combined count, then by identical (indicating leaks),
    // then by class (and size and address).
    struct SummaryElementComparator {
      GcRootComparator gc_root_cmp;

      bool operator()(SummaryElement& elem1, SummaryElement& elem2) const
          NO_THREAD_SAFETY_ANALYSIS {
        Locks::mutator_lock_->AssertSharedHeld(Thread::Current());

        size_t count1 = elem1.equiv + elem1.identical;
        size_t count2 = elem2.equiv + elem2.identical;
        if (count1 != count2) {
          return count1 > count2;
        }

        if (elem1.identical != elem2.identical) {
          return elem1.identical > elem2.identical;
        }

        // Otherwise, compare the GC roots as before.
        return gc_root_cmp(elem1.root, elem2.root);
      }
    };
    std::sort(sorted_summaries.begin(), sorted_summaries.end(), SummaryElementComparator());
  }

  // Dump a summary of the whole table.
  os << "  Summary:\n";
  for (SummaryElement& elem : sorted_summaries) {
    mirror::Object* elemObj = elem.root.Read<kWithoutReadBarrier>();
    DumpSummaryLine(os, elemObj, GetElementCount(elemObj), elem.identical, elem.equiv);
  }
}

void ReferenceTable::VisitRoots(RootVisitor* visitor, const RootInfo& root_info) {
  BufferedRootVisitor<kDefaultBufferedRootCount> buffered_visitor(visitor, root_info);
  for (GcRoot<mirror::Object>& root : entries_) {
    buffered_visitor.VisitRoot(root);
  }
}

}  // namespace art
