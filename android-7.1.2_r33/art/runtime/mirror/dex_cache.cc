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

#include "dex_cache-inl.h"

#include "art_method-inl.h"
#include "base/logging.h"
#include "class_linker.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "globals.h"
#include "object.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "runtime.h"
#include "string.h"

namespace art {
namespace mirror {

void DexCache::Init(const DexFile* dex_file,
                    String* location,
                    GcRoot<String>* strings,
                    uint32_t num_strings,
                    GcRoot<Class>* resolved_types,
                    uint32_t num_resolved_types,
                    ArtMethod** resolved_methods,
                    uint32_t num_resolved_methods,
                    ArtField** resolved_fields,
                    uint32_t num_resolved_fields,
                    size_t pointer_size) {
  CHECK(dex_file != nullptr);
  CHECK(location != nullptr);
  CHECK_EQ(num_strings != 0u, strings != nullptr);
  CHECK_EQ(num_resolved_types != 0u, resolved_types != nullptr);
  CHECK_EQ(num_resolved_methods != 0u, resolved_methods != nullptr);
  CHECK_EQ(num_resolved_fields != 0u, resolved_fields != nullptr);

  SetDexFile(dex_file);
  SetLocation(location);
  SetStrings(strings);
  SetResolvedTypes(resolved_types);
  SetResolvedMethods(resolved_methods);
  SetResolvedFields(resolved_fields);
  SetField32<false>(NumStringsOffset(), num_strings);
  SetField32<false>(NumResolvedTypesOffset(), num_resolved_types);
  SetField32<false>(NumResolvedMethodsOffset(), num_resolved_methods);
  SetField32<false>(NumResolvedFieldsOffset(), num_resolved_fields);

  Runtime* const runtime = Runtime::Current();
  if (runtime->HasResolutionMethod()) {
    // Initialize the resolve methods array to contain trampolines for resolution.
    Fixup(runtime->GetResolutionMethod(), pointer_size);
  }
}

void DexCache::Fixup(ArtMethod* trampoline, size_t pointer_size) {
  // Fixup the resolve methods array to contain trampoline for resolution.
  CHECK(trampoline != nullptr);
  CHECK(trampoline->IsRuntimeMethod());
  auto* resolved_methods = GetResolvedMethods();
  for (size_t i = 0, length = NumResolvedMethods(); i < length; i++) {
    if (GetElementPtrSize<ArtMethod*>(resolved_methods, i, pointer_size) == nullptr) {
      SetElementPtrSize(resolved_methods, i, trampoline, pointer_size);
    }
  }
}

void DexCache::SetLocation(mirror::String* location) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(DexCache, location_), location);
}

}  // namespace mirror
}  // namespace art
