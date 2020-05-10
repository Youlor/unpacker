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

#include <ctime>

#include "object.h"

#include "art_field.h"
#include "art_field-inl.h"
#include "array-inl.h"
#include "class.h"
#include "class-inl.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "iftable-inl.h"
#include "monitor.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "runtime.h"
#include "handle_scope-inl.h"
#include "throwable.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

Atomic<uint32_t> Object::hash_code_seed(987654321U + std::time(nullptr));

class CopyReferenceFieldsWithReadBarrierVisitor {
 public:
  explicit CopyReferenceFieldsWithReadBarrierVisitor(Object* dest_obj)
      : dest_obj_(dest_obj) {}

  void operator()(Object* obj, MemberOffset offset, bool /* is_static */) const
      ALWAYS_INLINE SHARED_REQUIRES(Locks::mutator_lock_) {
    // GetFieldObject() contains a RB.
    Object* ref = obj->GetFieldObject<Object>(offset);
    // No WB here as a large object space does not have a card table
    // coverage. Instead, cards will be marked separately.
    dest_obj_->SetFieldObjectWithoutWriteBarrier<false, false>(offset, ref);
  }

  void operator()(mirror::Class* klass, mirror::Reference* ref) const
      ALWAYS_INLINE SHARED_REQUIRES(Locks::mutator_lock_) {
    // Copy java.lang.ref.Reference.referent which isn't visited in
    // Object::VisitReferences().
    DCHECK(klass->IsTypeOfReferenceClass());
    this->operator()(ref, mirror::Reference::ReferentOffset(), false);
  }

  // Unused since we don't copy class native roots.
  void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED)
      const {}
  void VisitRoot(mirror::CompressedReference<mirror::Object>* root ATTRIBUTE_UNUSED) const {}

 private:
  Object* const dest_obj_;
};

Object* Object::CopyObject(Thread* self, mirror::Object* dest, mirror::Object* src,
                           size_t num_bytes) {
  // Copy instance data.  We assume memcpy copies by words.
  // TODO: expose and use move32.
  uint8_t* src_bytes = reinterpret_cast<uint8_t*>(src);
  uint8_t* dst_bytes = reinterpret_cast<uint8_t*>(dest);
  size_t offset = sizeof(Object);
  memcpy(dst_bytes + offset, src_bytes + offset, num_bytes - offset);
  if (kUseReadBarrier) {
    // We need a RB here. After the memcpy that covers the whole
    // object above, copy references fields one by one again with a
    // RB. TODO: Optimize this later?
    CopyReferenceFieldsWithReadBarrierVisitor visitor(dest);
    src->VisitReferences(visitor, visitor);
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // Perform write barriers on copied object references.
  Class* c = src->GetClass();
  if (c->IsArrayClass()) {
    if (!c->GetComponentType()->IsPrimitive()) {
      ObjectArray<Object>* array = dest->AsObjectArray<Object>();
      heap->WriteBarrierArray(dest, 0, array->GetLength());
    }
  } else {
    heap->WriteBarrierEveryFieldOf(dest);
  }
  if (c->IsFinalizable()) {
    heap->AddFinalizerReference(self, &dest);
  }
  return dest;
}

// An allocation pre-fence visitor that copies the object.
class CopyObjectVisitor {
 public:
  CopyObjectVisitor(Thread* self, Handle<Object>* orig, size_t num_bytes)
      : self_(self), orig_(orig), num_bytes_(num_bytes) {
  }

  void operator()(Object* obj, size_t usable_size ATTRIBUTE_UNUSED) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Object::CopyObject(self_, obj, orig_->Get(), num_bytes_);
  }

 private:
  Thread* const self_;
  Handle<Object>* const orig_;
  const size_t num_bytes_;
  DISALLOW_COPY_AND_ASSIGN(CopyObjectVisitor);
};

Object* Object::Clone(Thread* self) {
  CHECK(!IsClass()) << "Can't clone classes.";
  // Object::SizeOf gets the right size even if we're an array. Using c->AllocObject() here would
  // be wrong.
  gc::Heap* heap = Runtime::Current()->GetHeap();
  size_t num_bytes = SizeOf();
  StackHandleScope<1> hs(self);
  Handle<Object> this_object(hs.NewHandle(this));
  Object* copy;
  CopyObjectVisitor visitor(self, &this_object, num_bytes);
  if (heap->IsMovableObject(this)) {
    copy = heap->AllocObject<true>(self, GetClass(), num_bytes, visitor);
  } else {
    copy = heap->AllocNonMovableObject<true>(self, GetClass(), num_bytes, visitor);
  }
  return copy;
}

uint32_t Object::GenerateIdentityHashCode() {
  uint32_t expected_value, new_value;
  do {
    expected_value = hash_code_seed.LoadRelaxed();
    new_value = expected_value * 1103515245 + 12345;
  } while (!hash_code_seed.CompareExchangeWeakRelaxed(expected_value, new_value) ||
      (expected_value & LockWord::kHashMask) == 0);
  return expected_value & LockWord::kHashMask;
}

void Object::SetHashCodeSeed(uint32_t new_seed) {
  hash_code_seed.StoreRelaxed(new_seed);
}

int32_t Object::IdentityHashCode() const {
  mirror::Object* current_this = const_cast<mirror::Object*>(this);
  while (true) {
    LockWord lw = current_this->GetLockWord(false);
    switch (lw.GetState()) {
      case LockWord::kUnlocked: {
        // Try to compare and swap in a new hash, if we succeed we will return the hash on the next
        // loop iteration.
        LockWord hash_word = LockWord::FromHashCode(GenerateIdentityHashCode(),
                                                    lw.ReadBarrierState());
        DCHECK_EQ(hash_word.GetState(), LockWord::kHashCode);
        if (const_cast<Object*>(this)->CasLockWordWeakRelaxed(lw, hash_word)) {
          return hash_word.GetHashCode();
        }
        break;
      }
      case LockWord::kThinLocked: {
        // Inflate the thin lock to a monitor and stick the hash code inside of the monitor. May
        // fail spuriously.
        Thread* self = Thread::Current();
        StackHandleScope<1> hs(self);
        Handle<mirror::Object> h_this(hs.NewHandle(current_this));
        Monitor::InflateThinLocked(self, h_this, lw, GenerateIdentityHashCode());
        // A GC may have occurred when we switched to kBlocked.
        current_this = h_this.Get();
        break;
      }
      case LockWord::kFatLocked: {
        // Already inflated, return the hash stored in the monitor.
        Monitor* monitor = lw.FatLockMonitor();
        DCHECK(monitor != nullptr);
        return monitor->GetHashCode();
      }
      case LockWord::kHashCode: {
        return lw.GetHashCode();
      }
      default: {
        LOG(FATAL) << "Invalid state during hashcode " << lw.GetState();
        break;
      }
    }
  }
  UNREACHABLE();
}

void Object::CheckFieldAssignmentImpl(MemberOffset field_offset, Object* new_value) {
  Class* c = GetClass();
  Runtime* runtime = Runtime::Current();
  if (runtime->GetClassLinker() == nullptr || !runtime->IsStarted() ||
      !runtime->GetHeap()->IsObjectValidationEnabled() || !c->IsResolved()) {
    return;
  }
  for (Class* cur = c; cur != nullptr; cur = cur->GetSuperClass()) {
    for (ArtField& field : cur->GetIFields()) {
      StackHandleScope<1> hs(Thread::Current());
      Handle<Object> h_object(hs.NewHandle(new_value));
      if (field.GetOffset().Int32Value() == field_offset.Int32Value()) {
        CHECK_NE(field.GetTypeAsPrimitiveType(), Primitive::kPrimNot);
        // TODO: resolve the field type for moving GC.
        mirror::Class* field_type = field.GetType<!kMovingCollector>();
        if (field_type != nullptr) {
          CHECK(field_type->IsAssignableFrom(new_value->GetClass()));
        }
        return;
      }
    }
  }
  if (c->IsArrayClass()) {
    // Bounds and assign-ability done in the array setter.
    return;
  }
  if (IsClass()) {
    for (ArtField& field : AsClass()->GetSFields()) {
      if (field.GetOffset().Int32Value() == field_offset.Int32Value()) {
        CHECK_NE(field.GetTypeAsPrimitiveType(), Primitive::kPrimNot);
        // TODO: resolve the field type for moving GC.
        mirror::Class* field_type = field.GetType<!kMovingCollector>();
        if (field_type != nullptr) {
          CHECK(field_type->IsAssignableFrom(new_value->GetClass()));
        }
        return;
      }
    }
  }
  LOG(FATAL) << "Failed to find field for assignment to " << reinterpret_cast<void*>(this)
      << " of type " << PrettyDescriptor(c) << " at offset " << field_offset;
  UNREACHABLE();
}

ArtField* Object::FindFieldByOffset(MemberOffset offset) {
  return IsClass() ? ArtField::FindStaticFieldWithOffset(AsClass(), offset.Uint32Value())
      : ArtField::FindInstanceFieldWithOffset(GetClass(), offset.Uint32Value());
}

}  // namespace mirror
}  // namespace art
