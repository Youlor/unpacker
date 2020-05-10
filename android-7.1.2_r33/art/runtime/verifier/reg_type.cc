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

#include "reg_type-inl.h"

#include "base/arena_bit_vector.h"
#include "base/bit_vector-inl.h"
#include "base/casts.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "reg_type_cache-inl.h"
#include "scoped_thread_state_change.h"

#include <limits>
#include <sstream>

namespace art {
namespace verifier {

const UndefinedType* UndefinedType::instance_ = nullptr;
const ConflictType* ConflictType::instance_ = nullptr;
const BooleanType* BooleanType::instance_ = nullptr;
const ByteType* ByteType::instance_ = nullptr;
const ShortType* ShortType::instance_ = nullptr;
const CharType* CharType::instance_ = nullptr;
const FloatType* FloatType::instance_ = nullptr;
const LongLoType* LongLoType::instance_ = nullptr;
const LongHiType* LongHiType::instance_ = nullptr;
const DoubleLoType* DoubleLoType::instance_ = nullptr;
const DoubleHiType* DoubleHiType::instance_ = nullptr;
const IntegerType* IntegerType::instance_ = nullptr;

PrimitiveType::PrimitiveType(mirror::Class* klass, const StringPiece& descriptor, uint16_t cache_id)
    : RegType(klass, descriptor, cache_id) {
  CHECK(klass != nullptr);
  CHECK(!descriptor.empty());
}

Cat1Type::Cat1Type(mirror::Class* klass, const StringPiece& descriptor, uint16_t cache_id)
    : PrimitiveType(klass, descriptor, cache_id) {
}

Cat2Type::Cat2Type(mirror::Class* klass, const StringPiece& descriptor, uint16_t cache_id)
    : PrimitiveType(klass, descriptor, cache_id) {
}

std::string PreciseConstType::Dump() const {
  std::stringstream result;
  uint32_t val = ConstantValue();
  if (val == 0) {
    CHECK(IsPreciseConstant());
    result << "Zero/null";
  } else {
    result << "Precise ";
    if (IsConstantShort()) {
      result << StringPrintf("Constant: %d", val);
    } else {
      result << StringPrintf("Constant: 0x%x", val);
    }
  }
  return result.str();
}

std::string BooleanType::Dump() const {
  return "Boolean";
}

std::string ConflictType::Dump() const {
    return "Conflict";
}

std::string ByteType::Dump() const {
  return "Byte";
}

std::string ShortType::Dump() const {
  return "Short";
}

std::string CharType::Dump() const {
  return "Char";
}

std::string FloatType::Dump() const {
  return "Float";
}

std::string LongLoType::Dump() const {
  return "Long (Low Half)";
}

std::string LongHiType::Dump() const {
  return "Long (High Half)";
}

std::string DoubleLoType::Dump() const {
  return "Double (Low Half)";
}

std::string DoubleHiType::Dump() const {
  return "Double (High Half)";
}

std::string IntegerType::Dump() const {
  return "Integer";
}

const DoubleHiType* DoubleHiType::CreateInstance(mirror::Class* klass,
                                                 const StringPiece& descriptor,
                                                 uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new DoubleHiType(klass, descriptor, cache_id);
  return instance_;
}

void DoubleHiType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const DoubleLoType* DoubleLoType::CreateInstance(mirror::Class* klass,
                                                 const StringPiece& descriptor,
                                                 uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new DoubleLoType(klass, descriptor, cache_id);
  return instance_;
}

void DoubleLoType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const LongLoType* LongLoType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                             uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new LongLoType(klass, descriptor, cache_id);
  return instance_;
}

const LongHiType* LongHiType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                             uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new LongHiType(klass, descriptor, cache_id);
  return instance_;
}

void LongHiType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

void LongLoType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const FloatType* FloatType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                           uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new FloatType(klass, descriptor, cache_id);
  return instance_;
}

void FloatType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const CharType* CharType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                         uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new CharType(klass, descriptor, cache_id);
  return instance_;
}

void CharType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const ShortType* ShortType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                           uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new ShortType(klass, descriptor, cache_id);
  return instance_;
}

void ShortType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const ByteType* ByteType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                         uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new ByteType(klass, descriptor, cache_id);
  return instance_;
}

void ByteType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const IntegerType* IntegerType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                               uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new IntegerType(klass, descriptor, cache_id);
  return instance_;
}

void IntegerType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const ConflictType* ConflictType::CreateInstance(mirror::Class* klass,
                                                 const StringPiece& descriptor,
                                                 uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new ConflictType(klass, descriptor, cache_id);
  return instance_;
}

void ConflictType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

const BooleanType* BooleanType::CreateInstance(mirror::Class* klass, const StringPiece& descriptor,
                                         uint16_t cache_id) {
  CHECK(BooleanType::instance_ == nullptr);
  instance_ = new BooleanType(klass, descriptor, cache_id);
  return BooleanType::instance_;
}

void BooleanType::Destroy() {
  if (BooleanType::instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

std::string UndefinedType::Dump() const SHARED_REQUIRES(Locks::mutator_lock_) {
  return "Undefined";
}

const UndefinedType* UndefinedType::CreateInstance(mirror::Class* klass,
                                                   const StringPiece& descriptor,
                                                   uint16_t cache_id) {
  CHECK(instance_ == nullptr);
  instance_ = new UndefinedType(klass, descriptor, cache_id);
  return instance_;
}

void UndefinedType::Destroy() {
  if (instance_ != nullptr) {
    delete instance_;
    instance_ = nullptr;
  }
}

PreciseReferenceType::PreciseReferenceType(mirror::Class* klass, const StringPiece& descriptor,
                                           uint16_t cache_id)
    : RegType(klass, descriptor, cache_id) {
  // Note: no check for IsInstantiable() here. We may produce this in case an InstantiationError
  //       would be thrown at runtime, but we need to continue verification and *not* create a
  //       hard failure or abort.
}

std::string UnresolvedMergedType::Dump() const {
  std::stringstream result;
  result << "UnresolvedMergedReferences(" << GetResolvedPart().Dump() << " | ";
  const BitVector& types = GetUnresolvedTypes();

  bool first = true;
  for (uint32_t idx : types.Indexes()) {
    if (!first) {
      result << ", ";
    } else {
      first = false;
    }
    result << reg_type_cache_->GetFromId(idx).Dump();
  }
  result << ")";
  return result.str();
}

std::string UnresolvedSuperClass::Dump() const {
  std::stringstream result;
  uint16_t super_type_id = GetUnresolvedSuperClassChildId();
  result << "UnresolvedSuperClass(" << reg_type_cache_->GetFromId(super_type_id).Dump() << ")";
  return result.str();
}

std::string UnresolvedReferenceType::Dump() const {
  std::stringstream result;
  result << "Unresolved Reference" << ": " << PrettyDescriptor(GetDescriptor().as_string().c_str());
  return result.str();
}

std::string UnresolvedUninitializedRefType::Dump() const {
  std::stringstream result;
  result << "Unresolved And Uninitialized Reference" << ": "
      << PrettyDescriptor(GetDescriptor().as_string().c_str())
      << " Allocation PC: " << GetAllocationPc();
  return result.str();
}

std::string UnresolvedUninitializedThisRefType::Dump() const {
  std::stringstream result;
  result << "Unresolved And Uninitialized This Reference"
      << PrettyDescriptor(GetDescriptor().as_string().c_str());
  return result.str();
}

std::string ReferenceType::Dump() const {
  std::stringstream result;
  result << "Reference" << ": " << PrettyDescriptor(GetClass());
  return result.str();
}

std::string PreciseReferenceType::Dump() const {
  std::stringstream result;
  result << "Precise Reference" << ": "<< PrettyDescriptor(GetClass());
  return result.str();
}

std::string UninitializedReferenceType::Dump() const {
  std::stringstream result;
  result << "Uninitialized Reference" << ": " << PrettyDescriptor(GetClass());
  result << " Allocation PC: " << GetAllocationPc();
  return result.str();
}

std::string UninitializedThisReferenceType::Dump() const {
  std::stringstream result;
  result << "Uninitialized This Reference" << ": " << PrettyDescriptor(GetClass());
  result << "Allocation PC: " << GetAllocationPc();
  return result.str();
}

std::string ImpreciseConstType::Dump() const {
  std::stringstream result;
  uint32_t val = ConstantValue();
  if (val == 0) {
    result << "Zero/null";
  } else {
    result << "Imprecise ";
    if (IsConstantShort()) {
      result << StringPrintf("Constant: %d", val);
    } else {
      result << StringPrintf("Constant: 0x%x", val);
    }
  }
  return result.str();
}
std::string PreciseConstLoType::Dump() const {
  std::stringstream result;

  int32_t val = ConstantValueLo();
  result << "Precise ";
  if (val >= std::numeric_limits<jshort>::min() &&
      val <= std::numeric_limits<jshort>::max()) {
    result << StringPrintf("Low-half Constant: %d", val);
  } else {
    result << StringPrintf("Low-half Constant: 0x%x", val);
  }
  return result.str();
}

std::string ImpreciseConstLoType::Dump() const {
  std::stringstream result;

  int32_t val = ConstantValueLo();
  result << "Imprecise ";
  if (val >= std::numeric_limits<jshort>::min() &&
      val <= std::numeric_limits<jshort>::max()) {
    result << StringPrintf("Low-half Constant: %d", val);
  } else {
    result << StringPrintf("Low-half Constant: 0x%x", val);
  }
  return result.str();
}

std::string PreciseConstHiType::Dump() const {
  std::stringstream result;
  int32_t val = ConstantValueHi();
  result << "Precise ";
  if (val >= std::numeric_limits<jshort>::min() &&
      val <= std::numeric_limits<jshort>::max()) {
    result << StringPrintf("High-half Constant: %d", val);
  } else {
    result << StringPrintf("High-half Constant: 0x%x", val);
  }
  return result.str();
}

std::string ImpreciseConstHiType::Dump() const {
  std::stringstream result;
  int32_t val = ConstantValueHi();
  result << "Imprecise ";
  if (val >= std::numeric_limits<jshort>::min() &&
      val <= std::numeric_limits<jshort>::max()) {
    result << StringPrintf("High-half Constant: %d", val);
  } else {
    result << StringPrintf("High-half Constant: 0x%x", val);
  }
  return result.str();
}

const RegType& RegType::HighHalf(RegTypeCache* cache) const {
  DCHECK(IsLowHalf());
  if (IsLongLo()) {
    return cache->LongHi();
  } else if (IsDoubleLo()) {
    return cache->DoubleHi();
  } else {
    DCHECK(IsImpreciseConstantLo());
    const ConstantType* const_val = down_cast<const ConstantType*>(this);
    return cache->FromCat2ConstHi(const_val->ConstantValue(), false);
  }
}

Primitive::Type RegType::GetPrimitiveType() const {
  if (IsNonZeroReferenceTypes()) {
    return Primitive::kPrimNot;
  } else if (IsBooleanTypes()) {
    return Primitive::kPrimBoolean;
  } else if (IsByteTypes()) {
    return Primitive::kPrimByte;
  } else if (IsShortTypes()) {
    return Primitive::kPrimShort;
  } else if (IsCharTypes()) {
    return Primitive::kPrimChar;
  } else if (IsFloat()) {
    return Primitive::kPrimFloat;
  } else if (IsIntegralTypes()) {
    return Primitive::kPrimInt;
  } else if (IsDoubleLo()) {
    return Primitive::kPrimDouble;
  } else {
    DCHECK(IsLongTypes());
    return Primitive::kPrimLong;
  }
}

bool UninitializedType::IsUninitializedTypes() const {
  return true;
}

bool UninitializedType::IsNonZeroReferenceTypes() const {
  return true;
}

bool UnresolvedType::IsNonZeroReferenceTypes() const {
  return true;
}

const RegType& RegType::GetSuperClass(RegTypeCache* cache) const {
  if (!IsUnresolvedTypes()) {
    mirror::Class* super_klass = GetClass()->GetSuperClass();
    if (super_klass != nullptr) {
      // A super class of a precise type isn't precise as a precise type indicates the register
      // holds exactly that type.
      std::string temp;
      return cache->FromClass(super_klass->GetDescriptor(&temp), super_klass, false);
    } else {
      return cache->Zero();
    }
  } else {
    if (!IsUnresolvedMergedReference() && !IsUnresolvedSuperClass() &&
        GetDescriptor()[0] == '[') {
      // Super class of all arrays is Object.
      return cache->JavaLangObject(true);
    } else {
      return cache->FromUnresolvedSuperClass(*this);
    }
  }
}

bool RegType::IsJavaLangObject() const SHARED_REQUIRES(Locks::mutator_lock_) {
  return IsReference() && GetClass()->IsObjectClass();
}

bool RegType::IsObjectArrayTypes() const SHARED_REQUIRES(Locks::mutator_lock_) {
  if (IsUnresolvedTypes()) {
    DCHECK(!IsUnresolvedMergedReference());

    if (IsUnresolvedSuperClass()) {
      // Cannot be an array, as the superclass of arrays is java.lang.Object (which cannot be
      // unresolved).
      return false;
    }

    // Primitive arrays will always resolve.
    DCHECK(descriptor_[1] == 'L' || descriptor_[1] == '[');
    return descriptor_[0] == '[';
  } else if (HasClass()) {
    mirror::Class* type = GetClass();
    return type->IsArrayClass() && !type->GetComponentType()->IsPrimitive();
  } else {
    return false;
  }
}

bool RegType::IsArrayTypes() const SHARED_REQUIRES(Locks::mutator_lock_) {
  if (IsUnresolvedTypes()) {
    DCHECK(!IsUnresolvedMergedReference());

    if (IsUnresolvedSuperClass()) {
      // Cannot be an array, as the superclass of arrays is java.lang.Object (which cannot be
      // unresolved).
      return false;
    }
    return descriptor_[0] == '[';
  } else if (HasClass()) {
    return GetClass()->IsArrayClass();
  } else {
    return false;
  }
}

bool RegType::IsJavaLangObjectArray() const {
  if (HasClass()) {
    mirror::Class* type = GetClass();
    return type->IsArrayClass() && type->GetComponentType()->IsObjectClass();
  }
  return false;
}

bool RegType::IsInstantiableTypes() const {
  return IsUnresolvedTypes() || (IsNonZeroReferenceTypes() && GetClass()->IsInstantiable());
}

static const RegType& SelectNonConstant(const RegType& a, const RegType& b) {
  return a.IsConstantTypes() ? b : a;
}

const RegType& RegType::Merge(const RegType& incoming_type, RegTypeCache* reg_types) const {
  DCHECK(!Equals(incoming_type));  // Trivial equality handled by caller
  // Perform pointer equality tests for undefined and conflict to avoid virtual method dispatch.
  const UndefinedType& undefined = reg_types->Undefined();
  const ConflictType& conflict = reg_types->Conflict();
  DCHECK_EQ(this == &undefined, IsUndefined());
  DCHECK_EQ(&incoming_type == &undefined, incoming_type.IsUndefined());
  DCHECK_EQ(this == &conflict, IsConflict());
  DCHECK_EQ(&incoming_type == &conflict, incoming_type.IsConflict());
  if (this == &undefined || &incoming_type == &undefined) {
    // There is a difference between undefined and conflict. Conflicts may be copied around, but
    // not used. Undefined registers must not be copied. So any merge with undefined should return
    // undefined.
    return undefined;
  } else if (this == &conflict || &incoming_type == &conflict) {
    return conflict;  // (Conflict MERGE *) or (* MERGE Conflict) => Conflict
  } else if (IsConstant() && incoming_type.IsConstant()) {
    const ConstantType& type1 = *down_cast<const ConstantType*>(this);
    const ConstantType& type2 = *down_cast<const ConstantType*>(&incoming_type);
    int32_t val1 = type1.ConstantValue();
    int32_t val2 = type2.ConstantValue();
    if (val1 >= 0 && val2 >= 0) {
      // +ve1 MERGE +ve2 => MAX(+ve1, +ve2)
      if (val1 >= val2) {
        if (!type1.IsPreciseConstant()) {
          return *this;
        } else {
          return reg_types->FromCat1Const(val1, false);
        }
      } else {
        if (!type2.IsPreciseConstant()) {
          return type2;
        } else {
          return reg_types->FromCat1Const(val2, false);
        }
      }
    } else if (val1 < 0 && val2 < 0) {
      // -ve1 MERGE -ve2 => MIN(-ve1, -ve2)
      if (val1 <= val2) {
        if (!type1.IsPreciseConstant()) {
          return *this;
        } else {
          return reg_types->FromCat1Const(val1, false);
        }
      } else {
        if (!type2.IsPreciseConstant()) {
          return type2;
        } else {
          return reg_types->FromCat1Const(val2, false);
        }
      }
    } else {
      // Values are +ve and -ve, choose smallest signed type in which they both fit
      if (type1.IsConstantByte()) {
        if (type2.IsConstantByte()) {
          return reg_types->ByteConstant();
        } else if (type2.IsConstantShort()) {
          return reg_types->ShortConstant();
        } else {
          return reg_types->IntConstant();
        }
      } else if (type1.IsConstantShort()) {
        if (type2.IsConstantShort()) {
          return reg_types->ShortConstant();
        } else {
          return reg_types->IntConstant();
        }
      } else {
        return reg_types->IntConstant();
      }
    }
  } else if (IsConstantLo() && incoming_type.IsConstantLo()) {
    const ConstantType& type1 = *down_cast<const ConstantType*>(this);
    const ConstantType& type2 = *down_cast<const ConstantType*>(&incoming_type);
    int32_t val1 = type1.ConstantValueLo();
    int32_t val2 = type2.ConstantValueLo();
    return reg_types->FromCat2ConstLo(val1 | val2, false);
  } else if (IsConstantHi() && incoming_type.IsConstantHi()) {
    const ConstantType& type1 = *down_cast<const ConstantType*>(this);
    const ConstantType& type2 = *down_cast<const ConstantType*>(&incoming_type);
    int32_t val1 = type1.ConstantValueHi();
    int32_t val2 = type2.ConstantValueHi();
    return reg_types->FromCat2ConstHi(val1 | val2, false);
  } else if (IsIntegralTypes() && incoming_type.IsIntegralTypes()) {
    if (IsBooleanTypes() && incoming_type.IsBooleanTypes()) {
      return reg_types->Boolean();  // boolean MERGE boolean => boolean
    }
    if (IsByteTypes() && incoming_type.IsByteTypes()) {
      return reg_types->Byte();  // byte MERGE byte => byte
    }
    if (IsShortTypes() && incoming_type.IsShortTypes()) {
      return reg_types->Short();  // short MERGE short => short
    }
    if (IsCharTypes() && incoming_type.IsCharTypes()) {
      return reg_types->Char();  // char MERGE char => char
    }
    return reg_types->Integer();  // int MERGE * => int
  } else if ((IsFloatTypes() && incoming_type.IsFloatTypes()) ||
             (IsLongTypes() && incoming_type.IsLongTypes()) ||
             (IsLongHighTypes() && incoming_type.IsLongHighTypes()) ||
             (IsDoubleTypes() && incoming_type.IsDoubleTypes()) ||
             (IsDoubleHighTypes() && incoming_type.IsDoubleHighTypes())) {
    // check constant case was handled prior to entry
    DCHECK(!IsConstant() || !incoming_type.IsConstant());
    // float/long/double MERGE float/long/double_constant => float/long/double
    return SelectNonConstant(*this, incoming_type);
  } else if (IsReferenceTypes() && incoming_type.IsReferenceTypes()) {
    if (IsUninitializedTypes() || incoming_type.IsUninitializedTypes()) {
      // Something that is uninitialized hasn't had its constructor called. Unitialized types are
      // special. They may only ever be merged with themselves (must be taken care of by the
      // caller of Merge(), see the DCHECK on entry). So mark any other merge as conflicting here.
      return conflict;
    } else if (IsZero() || incoming_type.IsZero()) {
      return SelectNonConstant(*this, incoming_type);  // 0 MERGE ref => ref
    } else if (IsJavaLangObject() || incoming_type.IsJavaLangObject()) {
      return reg_types->JavaLangObject(false);  // Object MERGE ref => Object
    } else if (IsUnresolvedTypes() || incoming_type.IsUnresolvedTypes()) {
      // We know how to merge an unresolved type with itself, 0 or Object. In this case we
      // have two sub-classes and don't know how to merge. Create a new string-based unresolved
      // type that reflects our lack of knowledge and that allows the rest of the unresolved
      // mechanics to continue.
      return reg_types->FromUnresolvedMerge(*this, incoming_type);
    } else {  // Two reference types, compute Join
      mirror::Class* c1 = GetClass();
      mirror::Class* c2 = incoming_type.GetClass();
      DCHECK(c1 != nullptr && !c1->IsPrimitive());
      DCHECK(c2 != nullptr && !c2->IsPrimitive());
      mirror::Class* join_class = ClassJoin(c1, c2);
      if (c1 == join_class && !IsPreciseReference()) {
        return *this;
      } else if (c2 == join_class && !incoming_type.IsPreciseReference()) {
        return incoming_type;
      } else {
        std::string temp;
        return reg_types->FromClass(join_class->GetDescriptor(&temp), join_class, false);
      }
    }
  } else {
    return conflict;  // Unexpected types => Conflict
  }
}

// See comment in reg_type.h
mirror::Class* RegType::ClassJoin(mirror::Class* s, mirror::Class* t) {
  DCHECK(!s->IsPrimitive()) << PrettyClass(s);
  DCHECK(!t->IsPrimitive()) << PrettyClass(t);
  if (s == t) {
    return s;
  } else if (s->IsAssignableFrom(t)) {
    return s;
  } else if (t->IsAssignableFrom(s)) {
    return t;
  } else if (s->IsArrayClass() && t->IsArrayClass()) {
    mirror::Class* s_ct = s->GetComponentType();
    mirror::Class* t_ct = t->GetComponentType();
    if (s_ct->IsPrimitive() || t_ct->IsPrimitive()) {
      // Given the types aren't the same, if either array is of primitive types then the only
      // common parent is java.lang.Object
      mirror::Class* result = s->GetSuperClass();  // short-cut to java.lang.Object
      DCHECK(result->IsObjectClass());
      return result;
    }
    mirror::Class* common_elem = ClassJoin(s_ct, t_ct);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    mirror::Class* array_class = class_linker->FindArrayClass(Thread::Current(), &common_elem);
    DCHECK(array_class != nullptr);
    return array_class;
  } else {
    size_t s_depth = s->Depth();
    size_t t_depth = t->Depth();
    // Get s and t to the same depth in the hierarchy
    if (s_depth > t_depth) {
      while (s_depth > t_depth) {
        s = s->GetSuperClass();
        s_depth--;
      }
    } else {
      while (t_depth > s_depth) {
        t = t->GetSuperClass();
        t_depth--;
      }
    }
    // Go up the hierarchy until we get to the common parent
    while (s != t) {
      s = s->GetSuperClass();
      t = t->GetSuperClass();
    }
    return s;
  }
}

void RegType::CheckInvariants() const {
  if (IsConstant() || IsConstantLo() || IsConstantHi()) {
    CHECK(descriptor_.empty()) << *this;
    CHECK(klass_.IsNull()) << *this;
  }
  if (!klass_.IsNull()) {
    CHECK(!descriptor_.empty()) << *this;
  }
}

void RegType::VisitRoots(RootVisitor* visitor, const RootInfo& root_info) const {
  klass_.VisitRootIfNonNull(visitor, root_info);
}

void UninitializedThisReferenceType::CheckInvariants() const {
  CHECK_EQ(GetAllocationPc(), 0U) << *this;
}

void UnresolvedUninitializedThisRefType::CheckInvariants() const {
  CHECK_EQ(GetAllocationPc(), 0U) << *this;
  CHECK(!descriptor_.empty()) << *this;
  CHECK(klass_.IsNull()) << *this;
}

void UnresolvedUninitializedRefType::CheckInvariants() const {
  CHECK(!descriptor_.empty()) << *this;
  CHECK(klass_.IsNull()) << *this;
}

UnresolvedMergedType::UnresolvedMergedType(const RegType& resolved,
                                           const BitVector& unresolved,
                                           const RegTypeCache* reg_type_cache,
                                           uint16_t cache_id)
    : UnresolvedType("", cache_id),
      reg_type_cache_(reg_type_cache),
      resolved_part_(resolved),
      unresolved_types_(unresolved, false, unresolved.GetAllocator()) {
  if (kIsDebugBuild) {
    CheckInvariants();
  }
}
void UnresolvedMergedType::CheckInvariants() const {
  CHECK(reg_type_cache_ != nullptr);

  // Unresolved merged types: merged types should be defined.
  CHECK(descriptor_.empty()) << *this;
  CHECK(klass_.IsNull()) << *this;

  CHECK(!resolved_part_.IsConflict());
  CHECK(resolved_part_.IsReferenceTypes());
  CHECK(!resolved_part_.IsUnresolvedTypes());

  CHECK(resolved_part_.IsZero() ||
        !(resolved_part_.IsArrayTypes() && !resolved_part_.IsObjectArrayTypes()));

  CHECK_GT(unresolved_types_.NumSetBits(), 0U);
  bool unresolved_is_array =
      reg_type_cache_->GetFromId(unresolved_types_.GetHighestBitSet()).IsArrayTypes();
  for (uint32_t idx : unresolved_types_.Indexes()) {
    const RegType& t = reg_type_cache_->GetFromId(idx);
    CHECK_EQ(unresolved_is_array, t.IsArrayTypes());
  }

  if (!resolved_part_.IsZero()) {
    CHECK_EQ(resolved_part_.IsArrayTypes(), unresolved_is_array);
  }
}

bool UnresolvedMergedType::IsArrayTypes() const {
  // For a merge to be an array, both the resolved and the unresolved part need to be object
  // arrays.
  // (Note: we encode a missing resolved part [which doesn't need to be an array] as zero.)

  if (!resolved_part_.IsZero() && !resolved_part_.IsArrayTypes()) {
    return false;
  }

  // It is enough to check just one of the merged types. Otherwise the merge should have been
  // collapsed (checked in CheckInvariants on construction).
  uint32_t idx = unresolved_types_.GetHighestBitSet();
  const RegType& unresolved = reg_type_cache_->GetFromId(idx);
  return unresolved.IsArrayTypes();
}
bool UnresolvedMergedType::IsObjectArrayTypes() const {
  // Same as IsArrayTypes, as primitive arrays are always resolved.
  return IsArrayTypes();
}

void UnresolvedReferenceType::CheckInvariants() const {
  CHECK(!descriptor_.empty()) << *this;
  CHECK(klass_.IsNull()) << *this;
}

void UnresolvedSuperClass::CheckInvariants() const {
  // Unresolved merged types: merged types should be defined.
  CHECK(descriptor_.empty()) << *this;
  CHECK(klass_.IsNull()) << *this;
  CHECK_NE(unresolved_child_id_, 0U) << *this;
}

std::ostream& operator<<(std::ostream& os, const RegType& rhs) {
  os << rhs.Dump();
  return os;
}

bool RegType::CanAssignArray(const RegType& src, RegTypeCache& reg_types,
                             Handle<mirror::ClassLoader> class_loader, bool* soft_error) const {
  if (!IsArrayTypes() || !src.IsArrayTypes()) {
    *soft_error = false;
    return false;
  }

  if (IsUnresolvedMergedReference() || src.IsUnresolvedMergedReference()) {
    // An unresolved array type means that it's an array of some reference type. Reference arrays
    // can never be assigned to primitive-type arrays, and vice versa. So it is a soft error if
    // both arrays are reference arrays, otherwise a hard error.
    *soft_error = IsObjectArrayTypes() && src.IsObjectArrayTypes();
    return false;
  }

  const RegType& cmp1 = reg_types.GetComponentType(*this, class_loader.Get());
  const RegType& cmp2 = reg_types.GetComponentType(src, class_loader.Get());

  if (cmp1.IsAssignableFrom(cmp2)) {
    return true;
  }
  if (cmp1.IsUnresolvedTypes()) {
    if (cmp2.IsIntegralTypes() || cmp2.IsFloatTypes() || cmp2.IsArrayTypes()) {
      *soft_error = false;
      return false;
    }
    *soft_error = true;
    return false;
  }
  if (cmp2.IsUnresolvedTypes()) {
    if (cmp1.IsIntegralTypes() || cmp1.IsFloatTypes() || cmp1.IsArrayTypes()) {
      *soft_error = false;
      return false;
    }
    *soft_error = true;
    return false;
  }
  if (!cmp1.IsArrayTypes() || !cmp2.IsArrayTypes()) {
    *soft_error = false;
    return false;
  }
  return cmp1.CanAssignArray(cmp2, reg_types, class_loader, soft_error);
}


}  // namespace verifier
}  // namespace art
