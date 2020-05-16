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
#include "lambda/closure_builder.h"

#include "base/macros.h"
#include "base/value_object.h"
#include "lambda/art_lambda_method.h"
#include "lambda/closure.h"
#include "lambda/shorty_field_type.h"
#include "runtime/mirror/object_reference.h"

#include <stdint.h>
#include <vector>

namespace art {
namespace lambda {

/*
 * GC support TODOs:
 * (Although there's some code for storing objects, it is UNIMPLEMENTED(FATAL) because it is
 * incomplete).
 *
 * 1) GC needs to be able to traverse the Closure and visit any references.
 *    It might be possible to get away with global roots in the short term.
 *
 * 2) Add brooks read barrier support. We can store the black/gray/white bits
 *    in the lower 2 bits of the lambda art method pointer. Whenever a closure is copied
 *    [to the stack] we'd need to add a cold path to turn it black.
 *    (since there's only 3 colors, I can use the 4th value to indicate no-refs).
 *    e.g. 0x0 = gray, 0x1 = white, 0x2 = black, 0x3 = no-nested-references
 *    - Alternatively the GC can mark reference-less closures as always-black,
 *      although it would need extra work to check for references.
 */

void ClosureBuilder::CaptureVariableObject(mirror::Object* object) {
  auto compressed_reference = mirror::CompressedReference<mirror::Object>::FromMirrorPtr(object);
  ShortyFieldTypeTraits::MaxType storage = 0;

  static_assert(sizeof(storage) >= sizeof(compressed_reference),
                "not enough room to store a compressed reference");
  memcpy(&storage, &compressed_reference, sizeof(compressed_reference));

  values_.push_back(storage);
  size_ += kObjectReferenceSize;

  static_assert(kObjectReferenceSize == sizeof(compressed_reference), "reference size mismatch");

  // TODO: needs more work to support concurrent GC
  if (kIsDebugBuild) {
    if (kUseReadBarrier) {
      UNIMPLEMENTED(FATAL) << "can't yet safely capture objects with read barrier";
    }
  }

  shorty_types_ += ShortyFieldType::kObject;
}

void ClosureBuilder::CaptureVariableLambda(Closure* closure) {
  DCHECK(closure != nullptr);  // null closures not allowed, target method must be null instead.
  values_.push_back(reinterpret_cast<ShortyFieldTypeTraits::MaxType>(closure));

  if (LIKELY(is_dynamic_size_ == false)) {
    // Write in the extra bytes to store the dynamic size the first time.
    is_dynamic_size_ = true;
    size_ += sizeof(Closure::captured_[0].dynamic_.size_);
  }

  // A closure may be sized dynamically, so always query it for the true size.
  size_ += closure->GetSize();

  shorty_types_ += ShortyFieldType::kLambda;
}

size_t ClosureBuilder::GetSize() const {
  return size_;
}

size_t ClosureBuilder::GetCaptureCount() const {
  DCHECK_EQ(values_.size(), shorty_types_.size());
  return values_.size();
}

const std::string& ClosureBuilder::GetCapturedVariableShortyTypes() const {
  DCHECK_EQ(values_.size(), shorty_types_.size());
  return shorty_types_;
}

Closure* ClosureBuilder::CreateInPlace(void* memory, ArtLambdaMethod* target_method) const {
  DCHECK(memory != nullptr);
  DCHECK(target_method != nullptr);
  DCHECK_EQ(is_dynamic_size_, target_method->IsDynamicSize());

  CHECK_EQ(target_method->GetNumberOfCapturedVariables(), values_.size())
    << "number of variables captured at runtime does not match "
    << "number of variables captured at compile time";

  Closure* closure = new (memory) Closure;
  closure->lambda_info_ = target_method;

  static_assert(offsetof(Closure, captured_) == kInitialSize, "wrong initial size");

  size_t written_size;
  if (UNLIKELY(is_dynamic_size_)) {
    // The closure size must be set dynamically (i.e. nested lambdas).
    closure->captured_[0].dynamic_.size_ = GetSize();
    size_t header_size = offsetof(Closure, captured_[0].dynamic_.variables_);
    DCHECK_LE(header_size, GetSize());
    size_t variables_size = GetSize() - header_size;
    written_size =
        WriteValues(target_method,
                    closure->captured_[0].dynamic_.variables_,
                    header_size,
                    variables_size);
  } else {
    // The closure size is known statically (i.e. no nested lambdas).
    DCHECK(GetSize() == target_method->GetStaticClosureSize());
    size_t header_size = offsetof(Closure, captured_[0].static_variables_);
    DCHECK_LE(header_size, GetSize());
    size_t variables_size = GetSize() - header_size;
    written_size =
        WriteValues(target_method,
                    closure->captured_[0].static_variables_,
                    header_size,
                    variables_size);
  }

  DCHECK_EQ(written_size, closure->GetSize());

  return closure;
}

size_t ClosureBuilder::WriteValues(ArtLambdaMethod* target_method,
                                   uint8_t variables[],
                                   size_t header_size,
                                   size_t variables_size) const {
  size_t total_size = header_size;
  const char* shorty_types = target_method->GetCapturedVariablesShortyTypeDescriptor();
  DCHECK_STREQ(shorty_types, shorty_types_.c_str());

  size_t variables_offset = 0;
  size_t remaining_size = variables_size;

  const size_t shorty_count = target_method->GetNumberOfCapturedVariables();
  DCHECK_EQ(shorty_count, GetCaptureCount());

  for (size_t i = 0; i < shorty_count; ++i) {
    ShortyFieldType shorty{shorty_types[i]};  // NOLINT [readability/braces] [4]

    size_t var_size;
    if (LIKELY(shorty.IsStaticSize())) {
      // TODO: needs more work to support concurrent GC, e.g. read barriers
      if (kUseReadBarrier == false) {
        if (UNLIKELY(shorty.IsObject())) {
          UNIMPLEMENTED(FATAL) << "can't yet safely write objects with read barrier";
        }
      } else {
        if (UNLIKELY(shorty.IsObject())) {
          UNIMPLEMENTED(FATAL) << "writing objects not yet supported, no GC support";
        }
      }

      var_size = shorty.GetStaticSize();
      DCHECK_LE(var_size, sizeof(values_[i]));

      // Safe even for objects (non-read barrier case) if we never suspend
      // while the ClosureBuilder is live.
      // FIXME: Need to add GC support for references in a closure.
      memcpy(&variables[variables_offset], &values_[i], var_size);
    } else {
      DCHECK(shorty.IsLambda())
          << " don't support writing dynamically sized types other than lambda";

      ShortyFieldTypeTraits::MaxType closure_raw = values_[i];
      Closure* nested_closure = reinterpret_cast<Closure*>(closure_raw);

      DCHECK(nested_closure != nullptr);
      nested_closure->CopyTo(&variables[variables_offset], remaining_size);

      var_size = nested_closure->GetSize();
    }

    total_size += var_size;
    DCHECK_GE(remaining_size, var_size);
    remaining_size -= var_size;

    variables_offset += var_size;
  }

  DCHECK_EQ('\0', shorty_types[shorty_count]);
  DCHECK_EQ(variables_offset, variables_size);

  return total_size;
}


}  // namespace lambda
}  // namespace art
