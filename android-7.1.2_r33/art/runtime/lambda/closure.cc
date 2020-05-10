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

#include "lambda/closure.h"

#include "base/logging.h"
#include "lambda/art_lambda_method.h"
#include "runtime/mirror/object_reference.h"

static constexpr const bool kClosureSupportsReferences = false;
static constexpr const bool kClosureSupportsGarbageCollection = false;

namespace art {
namespace lambda {

template <typename T>
// TODO: can I return T __attribute__((__aligned__(1)))* here instead?
const uint8_t* Closure::GetUnsafeAtOffset(size_t offset) const {
  // Do not DCHECK here with existing helpers since most of them will call into this function.
  return reinterpret_cast<const uint8_t*>(captured_) + offset;
}

size_t Closure::GetCapturedVariableSize(ShortyFieldType variable_type, size_t offset) const {
  switch (variable_type) {
    case ShortyFieldType::kLambda:
    {
      return GetClosureSize(GetUnsafeAtOffset<Closure>(offset));
    }
    default:
      DCHECK(variable_type.IsStaticSize());
      return variable_type.GetStaticSize();
  }
}

// Templatize the flags to give the compiler a fighting chance to eliminate
// any unnecessary code through different uses of this function.
template <Closure::VariableInfo::Flags flags>
inline Closure::VariableInfo Closure::ParseTypeDescriptor(const char* type_descriptor,
                                                          size_t upto_index) const {
  DCHECK(type_descriptor != nullptr);

  VariableInfo result;

  ShortyFieldType last_type;
  size_t offset = (flags & VariableInfo::kOffset) ? GetStartingOffset() : 0;
  size_t prev_offset = 0;
  size_t count = 0;

  while ((type_descriptor =
      ShortyFieldType::ParseFromFieldTypeDescriptor(type_descriptor, &last_type)) != nullptr) {
    count++;

    if (flags & VariableInfo::kOffset) {
      // Accumulate the sizes of all preceding captured variables as the current offset only.
      offset += prev_offset;
      prev_offset = GetCapturedVariableSize(last_type, offset);
    }

    if ((count > upto_index)) {
      break;
    }
  }

  if (flags & VariableInfo::kVariableType) {
    result.variable_type_ = last_type;
  }

  if (flags & VariableInfo::kIndex) {
    result.index_ = count;
  }

  if (flags & VariableInfo::kCount) {
    result.count_ = count;
  }

  if (flags & VariableInfo::kOffset) {
    result.offset_ = offset;
  }

  // TODO: We should probably store the result of this in the ArtLambdaMethod,
  // to avoid re-computing the data every single time for static closures.
  return result;
}

size_t Closure::GetCapturedVariablesSize() const {
  const size_t captured_variable_offset = offsetof(Closure, captured_);
  DCHECK_GE(GetSize(), captured_variable_offset);  // Prevent underflows.
  return GetSize() - captured_variable_offset;
}

size_t Closure::GetSize() const {
  const size_t static_closure_size = lambda_info_->GetStaticClosureSize();
  if (LIKELY(lambda_info_->IsStaticSize())) {
    return static_closure_size;
  }

  DCHECK_GE(static_closure_size, sizeof(captured_[0].dynamic_.size_));
  const size_t dynamic_closure_size = captured_[0].dynamic_.size_;
  // The dynamic size better be at least as big as the static size.
  DCHECK_GE(dynamic_closure_size, static_closure_size);

  return dynamic_closure_size;
}

void Closure::CopyTo(void* target, size_t target_size) const {
  DCHECK_GE(target_size, GetSize());

  // TODO: using memcpy is unsafe with read barriers, fix this once we add reference support
  static_assert(kClosureSupportsReferences == false,
                "Do not use memcpy with readbarrier references");
  memcpy(target, this, GetSize());
}

ArtMethod* Closure::GetTargetMethod() const {
  return const_cast<ArtMethod*>(lambda_info_->GetArtMethod());
}

uint32_t Closure::GetHashCode() const {
  // Start with a non-zero constant, a prime number.
  uint32_t result = 17;

  // Include the hash with the ArtMethod.
  {
    uintptr_t method = reinterpret_cast<uintptr_t>(GetTargetMethod());
    result = 31 * result + Low32Bits(method);
    if (sizeof(method) == sizeof(uint64_t)) {
      result = 31 * result + High32Bits(method);
    }
  }

  // Include a hash for each captured variable.
  for (size_t i = 0; i < GetCapturedVariablesSize(); ++i) {
    // TODO: not safe for GC-able values since the address can move and the hash code would change.
    uint8_t captured_variable_raw_value;
    CopyUnsafeAtOffset<uint8_t>(i, /*out*/&captured_variable_raw_value);  // NOLINT: [whitespace/comma] [3]

    result = 31 * result + captured_variable_raw_value;
  }

  // TODO: Fix above loop to work for objects and lambdas.
  static_assert(kClosureSupportsGarbageCollection == false,
               "Need to update above loop to read the hash code from the "
                "objects and lambdas recursively");

  return result;
}

bool Closure::ReferenceEquals(const Closure* other) const {
  DCHECK(other != nullptr);

  // TODO: Need rework to use read barriers once closures have references inside of them that can
  // move. Until then, it's safe to just compare the data inside of it directly.
  static_assert(kClosureSupportsReferences == false,
                "Unsafe to use memcmp in read barrier collector");

  if (GetSize() != other->GetSize()) {
    return false;
  }

  return memcmp(this, other, GetSize());
}

size_t Closure::GetNumberOfCapturedVariables() const {
  // TODO: refactor into art_lambda_method.h. Parsing should only be required here as a DCHECK.
  VariableInfo variable_info =
      ParseTypeDescriptor<VariableInfo::kCount>(GetCapturedVariablesTypeDescriptor(),
                                                VariableInfo::kUpToIndexMax);
  size_t count = variable_info.count_;
  // Assuming each variable was 1 byte, the size should always be greater or equal than the count.
  DCHECK_LE(count, GetCapturedVariablesSize());
  return count;
}

const char* Closure::GetCapturedVariablesTypeDescriptor() const {
  return lambda_info_->GetCapturedVariablesTypeDescriptor();
}

ShortyFieldType Closure::GetCapturedShortyType(size_t index) const {
  DCHECK_LT(index, GetNumberOfCapturedVariables());

  VariableInfo variable_info =
      ParseTypeDescriptor<VariableInfo::kVariableType>(GetCapturedVariablesTypeDescriptor(),
                                                       index);

  return variable_info.variable_type_;
}

uint32_t Closure::GetCapturedPrimitiveNarrow(size_t index) const {
  DCHECK(GetCapturedShortyType(index).IsPrimitiveNarrow());

  ShortyFieldType variable_type;
  size_t offset;
  GetCapturedVariableTypeAndOffset(index, &variable_type, &offset);

  // TODO: Restructure to use template specialization, e.g. GetCapturedPrimitive<T>
  // so that we can avoid this nonsense regarding memcpy always overflowing.
  // Plus, this additional switching seems redundant since the interpreter
  // would've done it already, and knows the exact type.
  uint32_t result = 0;
  static_assert(ShortyFieldTypeTraits::IsPrimitiveNarrowType<decltype(result)>(),
                "result must be a primitive narrow type");
  switch (variable_type) {
    case ShortyFieldType::kBoolean:
      CopyUnsafeAtOffset<bool>(offset, &result);
      break;
    case ShortyFieldType::kByte:
      CopyUnsafeAtOffset<uint8_t>(offset, &result);
      break;
    case ShortyFieldType::kChar:
      CopyUnsafeAtOffset<uint16_t>(offset, &result);
      break;
    case ShortyFieldType::kShort:
      CopyUnsafeAtOffset<int16_t>(offset, &result);
      break;
    case ShortyFieldType::kInt:
      CopyUnsafeAtOffset<int32_t>(offset, &result);
      break;
    case ShortyFieldType::kFloat:
      // XX: Maybe there should just be a GetCapturedPrimitive<T> to avoid this shuffle?
      // The interpreter's invoke seems to only special case references and wides,
      // everything else is treated as a generic 32-bit pattern.
      CopyUnsafeAtOffset<float>(offset, &result);
      break;
    default:
      LOG(FATAL)
          << "expected a valid narrow primitive shorty type but got "
          << static_cast<char>(variable_type);
      UNREACHABLE();
  }

  return result;
}

uint64_t Closure::GetCapturedPrimitiveWide(size_t index) const {
  DCHECK(GetCapturedShortyType(index).IsPrimitiveWide());

  ShortyFieldType variable_type;
  size_t offset;
  GetCapturedVariableTypeAndOffset(index, &variable_type, &offset);

  // TODO: Restructure to use template specialization, e.g. GetCapturedPrimitive<T>
  // so that we can avoid this nonsense regarding memcpy always overflowing.
  // Plus, this additional switching seems redundant since the interpreter
  // would've done it already, and knows the exact type.
  uint64_t result = 0;
  static_assert(ShortyFieldTypeTraits::IsPrimitiveWideType<decltype(result)>(),
                "result must be a primitive wide type");
  switch (variable_type) {
    case ShortyFieldType::kLong:
      CopyUnsafeAtOffset<int64_t>(offset, &result);
      break;
    case ShortyFieldType::kDouble:
      CopyUnsafeAtOffset<double>(offset, &result);
      break;
    default:
      LOG(FATAL)
          << "expected a valid primitive wide shorty type but got "
          << static_cast<char>(variable_type);
      UNREACHABLE();
  }

  return result;
}

mirror::Object* Closure::GetCapturedObject(size_t index) const {
  DCHECK(GetCapturedShortyType(index).IsObject());

  ShortyFieldType variable_type;
  size_t offset;
  GetCapturedVariableTypeAndOffset(index, &variable_type, &offset);

  // TODO: Restructure to use template specialization, e.g. GetCapturedPrimitive<T>
  // so that we can avoid this nonsense regarding memcpy always overflowing.
  // Plus, this additional switching seems redundant since the interpreter
  // would've done it already, and knows the exact type.
  mirror::Object* result = nullptr;
  static_assert(ShortyFieldTypeTraits::IsObjectType<decltype(result)>(),
                "result must be an object type");
  switch (variable_type) {
    case ShortyFieldType::kObject:
      // TODO: This seems unsafe. This may need to use gcroots.
      static_assert(kClosureSupportsGarbageCollection == false,
                    "May need GcRoots and definitely need mutator locks");
      {
        mirror::CompressedReference<mirror::Object> compressed_result;
        CopyUnsafeAtOffset<uint32_t>(offset, &compressed_result);
        result = compressed_result.AsMirrorPtr();
      }
      break;
    default:
      CHECK(false)
          << "expected a valid shorty type but got " << static_cast<char>(variable_type);
      UNREACHABLE();
  }

  return result;
}

size_t Closure::GetCapturedClosureSize(size_t index) const {
  DCHECK(GetCapturedShortyType(index).IsLambda());
  size_t offset = GetCapturedVariableOffset(index);

  auto* captured_ptr = reinterpret_cast<const uint8_t*>(&captured_);
  size_t closure_size = GetClosureSize(captured_ptr + offset);

  return closure_size;
}

void Closure::CopyCapturedClosure(size_t index, void* destination, size_t destination_room) const {
  DCHECK(GetCapturedShortyType(index).IsLambda());
  size_t offset = GetCapturedVariableOffset(index);

  auto* captured_ptr = reinterpret_cast<const uint8_t*>(&captured_);
  size_t closure_size = GetClosureSize(captured_ptr + offset);

  static_assert(ShortyFieldTypeTraits::IsLambdaType<Closure*>(),
                "result must be a lambda type");

  CopyUnsafeAtOffset<Closure>(offset, destination, closure_size, destination_room);
}

size_t Closure::GetCapturedVariableOffset(size_t index) const {
  VariableInfo variable_info =
      ParseTypeDescriptor<VariableInfo::kOffset>(GetCapturedVariablesTypeDescriptor(),
                                                 index);

  size_t offset = variable_info.offset_;

  return offset;
}

void Closure::GetCapturedVariableTypeAndOffset(size_t index,
                                               ShortyFieldType* out_type,
                                               size_t* out_offset) const {
  DCHECK(out_type != nullptr);
  DCHECK(out_offset != nullptr);

  static constexpr const VariableInfo::Flags kVariableTypeAndOffset =
      static_cast<VariableInfo::Flags>(VariableInfo::kVariableType | VariableInfo::kOffset);
  VariableInfo variable_info =
      ParseTypeDescriptor<kVariableTypeAndOffset>(GetCapturedVariablesTypeDescriptor(),
                                                  index);

  ShortyFieldType variable_type = variable_info.variable_type_;
  size_t offset = variable_info.offset_;

  *out_type = variable_type;
  *out_offset = offset;
}

template <typename T>
void Closure::CopyUnsafeAtOffset(size_t offset,
                                 void* destination,
                                 size_t src_size,
                                 size_t destination_room) const {
  DCHECK_GE(destination_room, src_size);
  const uint8_t* data_ptr = GetUnsafeAtOffset<T>(offset);
  memcpy(destination, data_ptr, sizeof(T));
}

// TODO: This is kind of ugly. I would prefer an unaligned_ptr<Closure> here.
// Unfortunately C++ doesn't let you lower the alignment (i.e. alignas(1) Closure*) is not legal.
size_t Closure::GetClosureSize(const uint8_t* closure) {
  DCHECK(closure != nullptr);

  static_assert(!std::is_base_of<mirror::Object, Closure>::value,
                "It might be unsafe to call memcpy on a managed object");

  // Safe as long as it's not a mirror Object.
  // TODO: Should probably wrap this in like MemCpyNative or some such which statically asserts
  // we aren't trying to copy mirror::Object data around.
  ArtLambdaMethod* closure_info;
  memcpy(&closure_info, closure + offsetof(Closure, lambda_info_), sizeof(closure_info));

  if (LIKELY(closure_info->IsStaticSize())) {
    return closure_info->GetStaticClosureSize();
  }

  // The size is dynamic, so we need to read it from captured_variables_ portion.
  size_t dynamic_size;
  memcpy(&dynamic_size,
         closure + offsetof(Closure, captured_[0].dynamic_.size_),
         sizeof(dynamic_size));
  static_assert(sizeof(dynamic_size) == sizeof(captured_[0].dynamic_.size_),
                "Dynamic size type must match the structural type of the size");

  DCHECK_GE(dynamic_size, closure_info->GetStaticClosureSize());
  return dynamic_size;
}

size_t Closure::GetStartingOffset() const {
  static constexpr const size_t captured_offset = offsetof(Closure, captured_);
  if (LIKELY(lambda_info_->IsStaticSize())) {
    return offsetof(Closure, captured_[0].static_variables_) - captured_offset;
  } else {
    return offsetof(Closure, captured_[0].dynamic_.variables_) - captured_offset;
  }
}

}  // namespace lambda
}  // namespace art
