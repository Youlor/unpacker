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

#include "reg_type_cache-inl.h"

#include "base/arena_bit_vector.h"
#include "base/bit_vector-inl.h"
#include "base/casts.h"
#include "base/scoped_arena_allocator.h"
#include "base/stl_util.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "reg_type-inl.h"

namespace art {
namespace verifier {

bool RegTypeCache::primitive_initialized_ = false;
uint16_t RegTypeCache::primitive_count_ = 0;
const PreciseConstType* RegTypeCache::small_precise_constants_[kMaxSmallConstant -
                                                               kMinSmallConstant + 1];

ALWAYS_INLINE static inline bool MatchingPrecisionForClass(const RegType* entry, bool precise)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (entry->IsPreciseReference() == precise) {
    // We were or weren't looking for a precise reference and we found what we need.
    return true;
  } else {
    if (!precise && entry->GetClass()->CannotBeAssignedFromOtherTypes()) {
      // We weren't looking for a precise reference, as we're looking up based on a descriptor, but
      // we found a matching entry based on the descriptor. Return the precise entry in that case.
      return true;
    }
    return false;
  }
}

void RegTypeCache::FillPrimitiveAndSmallConstantTypes() {
  entries_.push_back(UndefinedType::GetInstance());
  entries_.push_back(ConflictType::GetInstance());
  entries_.push_back(BooleanType::GetInstance());
  entries_.push_back(ByteType::GetInstance());
  entries_.push_back(ShortType::GetInstance());
  entries_.push_back(CharType::GetInstance());
  entries_.push_back(IntegerType::GetInstance());
  entries_.push_back(LongLoType::GetInstance());
  entries_.push_back(LongHiType::GetInstance());
  entries_.push_back(FloatType::GetInstance());
  entries_.push_back(DoubleLoType::GetInstance());
  entries_.push_back(DoubleHiType::GetInstance());
  for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
    int32_t i = value - kMinSmallConstant;
    DCHECK_EQ(entries_.size(), small_precise_constants_[i]->GetId());
    entries_.push_back(small_precise_constants_[i]);
  }
  DCHECK_EQ(entries_.size(), primitive_count_);
}

const RegType& RegTypeCache::FromDescriptor(mirror::ClassLoader* loader,
                                            const char* descriptor,
                                            bool precise) {
  DCHECK(RegTypeCache::primitive_initialized_);
  if (descriptor[1] == '\0') {
    switch (descriptor[0]) {
      case 'Z':
        return Boolean();
      case 'B':
        return Byte();
      case 'S':
        return Short();
      case 'C':
        return Char();
      case 'I':
        return Integer();
      case 'J':
        return LongLo();
      case 'F':
        return Float();
      case 'D':
        return DoubleLo();
      case 'V':  // For void types, conflict types.
      default:
        return Conflict();
    }
  } else if (descriptor[0] == 'L' || descriptor[0] == '[') {
    return From(loader, descriptor, precise);
  } else {
    return Conflict();
  }
}

const RegType& RegTypeCache::RegTypeFromPrimitiveType(Primitive::Type prim_type) const {
  DCHECK(RegTypeCache::primitive_initialized_);
  switch (prim_type) {
    case Primitive::kPrimBoolean:
      return *BooleanType::GetInstance();
    case Primitive::kPrimByte:
      return *ByteType::GetInstance();
    case Primitive::kPrimShort:
      return *ShortType::GetInstance();
    case Primitive::kPrimChar:
      return *CharType::GetInstance();
    case Primitive::kPrimInt:
      return *IntegerType::GetInstance();
    case Primitive::kPrimLong:
      return *LongLoType::GetInstance();
    case Primitive::kPrimFloat:
      return *FloatType::GetInstance();
    case Primitive::kPrimDouble:
      return *DoubleLoType::GetInstance();
    case Primitive::kPrimVoid:
    default:
      return *ConflictType::GetInstance();
  }
}

bool RegTypeCache::MatchDescriptor(size_t idx, const StringPiece& descriptor, bool precise) {
  const RegType* entry = entries_[idx];
  if (descriptor != entry->descriptor_) {
    return false;
  }
  if (entry->HasClass()) {
    return MatchingPrecisionForClass(entry, precise);
  }
  // There is no notion of precise unresolved references, the precise information is just dropped
  // on the floor.
  DCHECK(entry->IsUnresolvedReference());
  return true;
}

mirror::Class* RegTypeCache::ResolveClass(const char* descriptor, mirror::ClassLoader* loader) {
  // Class was not found, must create new type.
  // Try resolving class
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(loader));
  mirror::Class* klass = nullptr;
  if (can_load_classes_) {
    klass = class_linker->FindClass(self, descriptor, class_loader);
  } else {
    klass = class_linker->LookupClass(self, descriptor, ComputeModifiedUtf8Hash(descriptor),
                                      loader);
    if (klass != nullptr && !klass->IsResolved()) {
      // We found the class but without it being loaded its not safe for use.
      klass = nullptr;
    }
  }
  return klass;
}

StringPiece RegTypeCache::AddString(const StringPiece& string_piece) {
  char* ptr = arena_.AllocArray<char>(string_piece.length());
  memcpy(ptr, string_piece.data(), string_piece.length());
  return StringPiece(ptr, string_piece.length());
}

const RegType& RegTypeCache::From(mirror::ClassLoader* loader,
                                  const char* descriptor,
                                  bool precise) {
  StringPiece sp_descriptor(descriptor);
  // Try looking up the class in the cache first. We use a StringPiece to avoid continual strlen
  // operations on the descriptor.
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    if (MatchDescriptor(i, sp_descriptor, precise)) {
      return *(entries_[i]);
    }
  }
  // Class not found in the cache, will create a new type for that.
  // Try resolving class.
  mirror::Class* klass = ResolveClass(descriptor, loader);
  if (klass != nullptr) {
    // Class resolved, first look for the class in the list of entries
    // Class was not found, must create new type.
    // To pass the verification, the type should be imprecise,
    // instantiable or an interface with the precise type set to false.
    DCHECK(!precise || klass->IsInstantiable());
    // Create a precise type if:
    // 1- Class is final and NOT an interface. a precise interface is meaningless !!
    // 2- Precise Flag passed as true.
    RegType* entry;
    // Create an imprecise type if we can't tell for a fact that it is precise.
    if (klass->CannotBeAssignedFromOtherTypes() || precise) {
      DCHECK(!(klass->IsAbstract()) || klass->IsArrayClass());
      DCHECK(!klass->IsInterface());
      entry = new (&arena_) PreciseReferenceType(klass, AddString(sp_descriptor), entries_.size());
    } else {
      entry = new (&arena_) ReferenceType(klass, AddString(sp_descriptor), entries_.size());
    }
    return AddEntry(entry);
  } else {  // Class not resolved.
    // We tried loading the class and failed, this might get an exception raised
    // so we want to clear it before we go on.
    if (can_load_classes_) {
      DCHECK(Thread::Current()->IsExceptionPending());
      Thread::Current()->ClearException();
    } else {
      DCHECK(!Thread::Current()->IsExceptionPending());
    }
    if (IsValidDescriptor(descriptor)) {
      return AddEntry(
          new (&arena_) UnresolvedReferenceType(AddString(sp_descriptor), entries_.size()));
    } else {
      // The descriptor is broken return the unknown type as there's nothing sensible that
      // could be done at runtime
      return Conflict();
    }
  }
}

const RegType* RegTypeCache::FindClass(mirror::Class* klass, bool precise) const {
  DCHECK(klass != nullptr);
  if (klass->IsPrimitive()) {
    // Note: precise isn't used for primitive classes. A char is assignable to an int. All
    // primitive classes are final.
    return &RegTypeFromPrimitiveType(klass->GetPrimitiveType());
  }
  for (auto& pair : klass_entries_) {
    mirror::Class* const reg_klass = pair.first.Read();
    if (reg_klass == klass) {
      const RegType* reg_type = pair.second;
      if (MatchingPrecisionForClass(reg_type, precise)) {
        return reg_type;
      }
    }
  }
  return nullptr;
}

const RegType* RegTypeCache::InsertClass(const StringPiece& descriptor,
                                         mirror::Class* klass,
                                         bool precise) {
  // No reference to the class was found, create new reference.
  DCHECK(FindClass(klass, precise) == nullptr);
  RegType* const reg_type = precise
      ? static_cast<RegType*>(
          new (&arena_) PreciseReferenceType(klass, descriptor, entries_.size()))
      : new (&arena_) ReferenceType(klass, descriptor, entries_.size());
  return &AddEntry(reg_type);
}

const RegType& RegTypeCache::FromClass(const char* descriptor, mirror::Class* klass, bool precise) {
  DCHECK(klass != nullptr);
  const RegType* reg_type = FindClass(klass, precise);
  if (reg_type == nullptr) {
    reg_type = InsertClass(AddString(StringPiece(descriptor)), klass, precise);
  }
  return *reg_type;
}

RegTypeCache::RegTypeCache(bool can_load_classes, ScopedArenaAllocator& arena)
    : entries_(arena.Adapter(kArenaAllocVerifier)),
      klass_entries_(arena.Adapter(kArenaAllocVerifier)),
      can_load_classes_(can_load_classes),
      arena_(arena) {
  if (kIsDebugBuild) {
    Thread::Current()->AssertThreadSuspensionIsAllowable(gAborting == 0);
  }
  // The klass_entries_ array does not have primitives or small constants.
  static constexpr size_t kNumReserveEntries = 32;
  klass_entries_.reserve(kNumReserveEntries);
  // We want to have room for additional entries after inserting primitives and small
  // constants.
  entries_.reserve(kNumReserveEntries + kNumPrimitivesAndSmallConstants);
  FillPrimitiveAndSmallConstantTypes();
}

RegTypeCache::~RegTypeCache() {
  DCHECK_LE(primitive_count_, entries_.size());
}

void RegTypeCache::ShutDown() {
  if (RegTypeCache::primitive_initialized_) {
    UndefinedType::Destroy();
    ConflictType::Destroy();
    BooleanType::Destroy();
    ByteType::Destroy();
    ShortType::Destroy();
    CharType::Destroy();
    IntegerType::Destroy();
    LongLoType::Destroy();
    LongHiType::Destroy();
    FloatType::Destroy();
    DoubleLoType::Destroy();
    DoubleHiType::Destroy();
    for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
      const PreciseConstType* type = small_precise_constants_[value - kMinSmallConstant];
      delete type;
      small_precise_constants_[value - kMinSmallConstant] = nullptr;
    }
    RegTypeCache::primitive_initialized_ = false;
    RegTypeCache::primitive_count_ = 0;
  }
}

template <class Type>
const Type* RegTypeCache::CreatePrimitiveTypeInstance(const std::string& descriptor) {
  mirror::Class* klass = nullptr;
  // Try loading the class from linker.
  if (!descriptor.empty()) {
    klass = art::Runtime::Current()->GetClassLinker()->FindSystemClass(Thread::Current(),
                                                                       descriptor.c_str());
    DCHECK(klass != nullptr);
  }
  const Type* entry = Type::CreateInstance(klass, descriptor, RegTypeCache::primitive_count_);
  RegTypeCache::primitive_count_++;
  return entry;
}

void RegTypeCache::CreatePrimitiveAndSmallConstantTypes() {
  CreatePrimitiveTypeInstance<UndefinedType>("");
  CreatePrimitiveTypeInstance<ConflictType>("");
  CreatePrimitiveTypeInstance<BooleanType>("Z");
  CreatePrimitiveTypeInstance<ByteType>("B");
  CreatePrimitiveTypeInstance<ShortType>("S");
  CreatePrimitiveTypeInstance<CharType>("C");
  CreatePrimitiveTypeInstance<IntegerType>("I");
  CreatePrimitiveTypeInstance<LongLoType>("J");
  CreatePrimitiveTypeInstance<LongHiType>("J");
  CreatePrimitiveTypeInstance<FloatType>("F");
  CreatePrimitiveTypeInstance<DoubleLoType>("D");
  CreatePrimitiveTypeInstance<DoubleHiType>("D");
  for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
    PreciseConstType* type = new PreciseConstType(value, primitive_count_);
    small_precise_constants_[value - kMinSmallConstant] = type;
    primitive_count_++;
  }
}

const RegType& RegTypeCache::FromUnresolvedMerge(const RegType& left, const RegType& right) {
  ArenaBitVector types(&arena_,
                       kDefaultArenaBitVectorBytes * kBitsPerByte,  // Allocate at least 8 bytes.
                       true);                                       // Is expandable.
  const RegType* left_resolved;
  bool left_unresolved_is_array;
  if (left.IsUnresolvedMergedReference()) {
    const UnresolvedMergedType& left_merge = *down_cast<const UnresolvedMergedType*>(&left);

    types.Copy(&left_merge.GetUnresolvedTypes());
    left_resolved = &left_merge.GetResolvedPart();
    left_unresolved_is_array = left.IsArrayTypes();
  } else if (left.IsUnresolvedTypes()) {
    types.ClearAllBits();
    types.SetBit(left.GetId());
    left_resolved = &Zero();
    left_unresolved_is_array = left.IsArrayTypes();
  } else {
    types.ClearAllBits();
    left_resolved = &left;
    left_unresolved_is_array = false;
  }

  const RegType* right_resolved;
  bool right_unresolved_is_array;
  if (right.IsUnresolvedMergedReference()) {
    const UnresolvedMergedType& right_merge = *down_cast<const UnresolvedMergedType*>(&right);

    types.Union(&right_merge.GetUnresolvedTypes());
    right_resolved = &right_merge.GetResolvedPart();
    right_unresolved_is_array = right.IsArrayTypes();
  } else if (right.IsUnresolvedTypes()) {
    types.SetBit(right.GetId());
    right_resolved = &Zero();
    right_unresolved_is_array = right.IsArrayTypes();
  } else {
    right_resolved = &right;
    right_unresolved_is_array = false;
  }

  // Merge the resolved parts. Left and right might be equal, so use SafeMerge.
  const RegType& resolved_parts_merged = left_resolved->SafeMerge(*right_resolved, this);
  // If we get a conflict here, the merge result is a conflict, not an unresolved merge type.
  if (resolved_parts_merged.IsConflict()) {
    return Conflict();
  }

  bool resolved_merged_is_array = resolved_parts_merged.IsArrayTypes();
  if (left_unresolved_is_array || right_unresolved_is_array || resolved_merged_is_array) {
    // Arrays involved, see if we need to merge to Object.

    // Is the resolved part a primitive array?
    if (resolved_merged_is_array && !resolved_parts_merged.IsObjectArrayTypes()) {
      return JavaLangObject(false /* precise */);
    }

    // Is any part not an array (but exists)?
    if ((!left_unresolved_is_array && left_resolved != &left) ||
        (!right_unresolved_is_array && right_resolved != &right) ||
        !resolved_merged_is_array) {
      return JavaLangObject(false /* precise */);
    }
  }

  // Check if entry already exists.
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsUnresolvedMergedReference()) {
      const UnresolvedMergedType* cmp_type = down_cast<const UnresolvedMergedType*>(cur_entry);
      const RegType& resolved_part = cmp_type->GetResolvedPart();
      const BitVector& unresolved_part = cmp_type->GetUnresolvedTypes();
      // Use SameBitsSet. "types" is expandable to allow merging in the components, but the
      // BitVector in the final RegType will be made non-expandable.
      if (&resolved_part == &resolved_parts_merged && types.SameBitsSet(&unresolved_part)) {
        return *cur_entry;
      }
    }
  }
  return AddEntry(new (&arena_) UnresolvedMergedType(resolved_parts_merged,
                                                     types,
                                                     this,
                                                     entries_.size()));
}

const RegType& RegTypeCache::FromUnresolvedSuperClass(const RegType& child) {
  // Check if entry already exists.
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsUnresolvedSuperClass()) {
      const UnresolvedSuperClass* tmp_entry =
          down_cast<const UnresolvedSuperClass*>(cur_entry);
      uint16_t unresolved_super_child_id =
          tmp_entry->GetUnresolvedSuperClassChildId();
      if (unresolved_super_child_id == child.GetId()) {
        return *cur_entry;
      }
    }
  }
  return AddEntry(new (&arena_) UnresolvedSuperClass(child.GetId(), this, entries_.size()));
}

const UninitializedType& RegTypeCache::Uninitialized(const RegType& type, uint32_t allocation_pc) {
  UninitializedType* entry = nullptr;
  const StringPiece& descriptor(type.GetDescriptor());
  if (type.IsUnresolvedTypes()) {
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedAndUninitializedReference() &&
          down_cast<const UnresolvedUninitializedRefType*>(cur_entry)->GetAllocationPc()
              == allocation_pc &&
          (cur_entry->GetDescriptor() == descriptor)) {
        return *down_cast<const UnresolvedUninitializedRefType*>(cur_entry);
      }
    }
    entry = new (&arena_) UnresolvedUninitializedRefType(descriptor,
                                                         allocation_pc,
                                                         entries_.size());
  } else {
    mirror::Class* klass = type.GetClass();
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUninitializedReference() &&
          down_cast<const UninitializedReferenceType*>(cur_entry)
              ->GetAllocationPc() == allocation_pc &&
          cur_entry->GetClass() == klass) {
        return *down_cast<const UninitializedReferenceType*>(cur_entry);
      }
    }
    entry = new (&arena_) UninitializedReferenceType(klass,
                                                     descriptor,
                                                     allocation_pc,
                                                     entries_.size());
  }
  return AddEntry(entry);
}

const RegType& RegTypeCache::FromUninitialized(const RegType& uninit_type) {
  RegType* entry;

  if (uninit_type.IsUnresolvedTypes()) {
    const StringPiece& descriptor(uninit_type.GetDescriptor());
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedReference() &&
          cur_entry->GetDescriptor() == descriptor) {
        return *cur_entry;
      }
    }
    entry = new (&arena_) UnresolvedReferenceType(descriptor, entries_.size());
  } else {
    mirror::Class* klass = uninit_type.GetClass();
    if (uninit_type.IsUninitializedThisReference() && !klass->IsFinal()) {
      // For uninitialized "this reference" look for reference types that are not precise.
      for (size_t i = primitive_count_; i < entries_.size(); i++) {
        const RegType* cur_entry = entries_[i];
        if (cur_entry->IsReference() && cur_entry->GetClass() == klass) {
          return *cur_entry;
        }
      }
      entry = new (&arena_) ReferenceType(klass, "", entries_.size());
    } else if (!klass->IsPrimitive()) {
      // We're uninitialized because of allocation, look or create a precise type as allocations
      // may only create objects of that type.
      // Note: we do not check whether the given klass is actually instantiable (besides being
      //       primitive), that is, we allow interfaces and abstract classes here. The reasoning is
      //       twofold:
      //       1) The "new-instance" instruction to generate the uninitialized type will already
      //          queue an instantiation error. This is a soft error that must be thrown at runtime,
      //          and could potentially change if the class is resolved differently at runtime.
      //       2) Checking whether the klass is instantiable and using conflict may produce a hard
      //          error when the value is used, which leads to a VerifyError, which is not the
      //          correct semantics.
      for (size_t i = primitive_count_; i < entries_.size(); i++) {
        const RegType* cur_entry = entries_[i];
        if (cur_entry->IsPreciseReference() && cur_entry->GetClass() == klass) {
          return *cur_entry;
        }
      }
      entry = new (&arena_) PreciseReferenceType(klass,
                                                 uninit_type.GetDescriptor(),
                                                 entries_.size());
    } else {
      return Conflict();
    }
  }
  return AddEntry(entry);
}

const UninitializedType& RegTypeCache::UninitializedThisArgument(const RegType& type) {
  UninitializedType* entry;
  const StringPiece& descriptor(type.GetDescriptor());
  if (type.IsUnresolvedTypes()) {
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUnresolvedAndUninitializedThisReference() &&
          cur_entry->GetDescriptor() == descriptor) {
        return *down_cast<const UninitializedType*>(cur_entry);
      }
    }
    entry = new (&arena_) UnresolvedUninitializedThisRefType(descriptor, entries_.size());
  } else {
    mirror::Class* klass = type.GetClass();
    for (size_t i = primitive_count_; i < entries_.size(); i++) {
      const RegType* cur_entry = entries_[i];
      if (cur_entry->IsUninitializedThisReference() && cur_entry->GetClass() == klass) {
        return *down_cast<const UninitializedType*>(cur_entry);
      }
    }
    entry = new (&arena_) UninitializedThisReferenceType(klass, descriptor, entries_.size());
  }
  return AddEntry(entry);
}

const ConstantType& RegTypeCache::FromCat1NonSmallConstant(int32_t value, bool precise) {
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->klass_.IsNull() && cur_entry->IsConstant() &&
        cur_entry->IsPreciseConstant() == precise &&
        (down_cast<const ConstantType*>(cur_entry))->ConstantValue() == value) {
      return *down_cast<const ConstantType*>(cur_entry);
    }
  }
  ConstantType* entry;
  if (precise) {
    entry = new (&arena_) PreciseConstType(value, entries_.size());
  } else {
    entry = new (&arena_) ImpreciseConstType(value, entries_.size());
  }
  return AddEntry(entry);
}

const ConstantType& RegTypeCache::FromCat2ConstLo(int32_t value, bool precise) {
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsConstantLo() && (cur_entry->IsPrecise() == precise) &&
        (down_cast<const ConstantType*>(cur_entry))->ConstantValueLo() == value) {
      return *down_cast<const ConstantType*>(cur_entry);
    }
  }
  ConstantType* entry;
  if (precise) {
    entry = new (&arena_) PreciseConstLoType(value, entries_.size());
  } else {
    entry = new (&arena_) ImpreciseConstLoType(value, entries_.size());
  }
  return AddEntry(entry);
}

const ConstantType& RegTypeCache::FromCat2ConstHi(int32_t value, bool precise) {
  for (size_t i = primitive_count_; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry->IsConstantHi() && (cur_entry->IsPrecise() == precise) &&
        (down_cast<const ConstantType*>(cur_entry))->ConstantValueHi() == value) {
      return *down_cast<const ConstantType*>(cur_entry);
    }
  }
  ConstantType* entry;
  if (precise) {
    entry = new (&arena_) PreciseConstHiType(value, entries_.size());
  } else {
    entry = new (&arena_) ImpreciseConstHiType(value, entries_.size());
  }
  return AddEntry(entry);
}

const RegType& RegTypeCache::GetComponentType(const RegType& array, mirror::ClassLoader* loader) {
  if (!array.IsArrayTypes()) {
    return Conflict();
  } else if (array.IsUnresolvedTypes()) {
    DCHECK(!array.IsUnresolvedMergedReference());  // Caller must make sure not to ask for this.
    const std::string descriptor(array.GetDescriptor().as_string());
    return FromDescriptor(loader, descriptor.c_str() + 1, false);
  } else {
    mirror::Class* klass = array.GetClass()->GetComponentType();
    std::string temp;
    const char* descriptor = klass->GetDescriptor(&temp);
    if (klass->IsErroneous()) {
      // Arrays may have erroneous component types, use unresolved in that case.
      // We assume that the primitive classes are not erroneous, so we know it is a
      // reference type.
      return FromDescriptor(loader, descriptor, false);
    } else {
      return FromClass(descriptor, klass, klass->CannotBeAssignedFromOtherTypes());
    }
  }
}

void RegTypeCache::Dump(std::ostream& os) {
  for (size_t i = 0; i < entries_.size(); i++) {
    const RegType* cur_entry = entries_[i];
    if (cur_entry != nullptr) {
      os << i << ": " << cur_entry->Dump() << "\n";
    }
  }
}

void RegTypeCache::VisitStaticRoots(RootVisitor* visitor) {
  // Visit the primitive types, this is required since if there are no active verifiers they wont
  // be in the entries array, and therefore not visited as roots.
  if (primitive_initialized_) {
    RootInfo ri(kRootUnknown);
    UndefinedType::GetInstance()->VisitRoots(visitor, ri);
    ConflictType::GetInstance()->VisitRoots(visitor, ri);
    BooleanType::GetInstance()->VisitRoots(visitor, ri);
    ByteType::GetInstance()->VisitRoots(visitor, ri);
    ShortType::GetInstance()->VisitRoots(visitor, ri);
    CharType::GetInstance()->VisitRoots(visitor, ri);
    IntegerType::GetInstance()->VisitRoots(visitor, ri);
    LongLoType::GetInstance()->VisitRoots(visitor, ri);
    LongHiType::GetInstance()->VisitRoots(visitor, ri);
    FloatType::GetInstance()->VisitRoots(visitor, ri);
    DoubleLoType::GetInstance()->VisitRoots(visitor, ri);
    DoubleHiType::GetInstance()->VisitRoots(visitor, ri);
    for (int32_t value = kMinSmallConstant; value <= kMaxSmallConstant; ++value) {
      small_precise_constants_[value - kMinSmallConstant]->VisitRoots(visitor, ri);
    }
  }
}

void RegTypeCache::VisitRoots(RootVisitor* visitor, const RootInfo& root_info) {
  // Exclude the static roots that are visited by VisitStaticRoots().
  for (size_t i = primitive_count_; i < entries_.size(); ++i) {
    entries_[i]->VisitRoots(visitor, root_info);
  }
  for (auto& pair : klass_entries_) {
    GcRoot<mirror::Class>& root = pair.first;
    root.VisitRoot(visitor, root_info);
  }
}

}  // namespace verifier
}  // namespace art
