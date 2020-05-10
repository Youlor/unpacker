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

#include "art_field.h"

#include "art_field-inl.h"
#include "class_linker-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "handle_scope.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {

ArtField::ArtField() : access_flags_(0), field_dex_idx_(0), offset_(0) {
  declaring_class_ = GcRoot<mirror::Class>(nullptr);
}

void ArtField::SetOffset(MemberOffset num_bytes) {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  if (kIsDebugBuild && Runtime::Current()->IsAotCompiler() &&
      Runtime::Current()->IsCompilingBootImage()) {
    Primitive::Type type = GetTypeAsPrimitiveType();
    if (type == Primitive::kPrimDouble || type == Primitive::kPrimLong) {
      DCHECK_ALIGNED(num_bytes.Uint32Value(), 8);
    }
  }
  // Not called within a transaction.
  offset_ = num_bytes.Uint32Value();
}

mirror::Class* ArtField::ProxyFindSystemClass(const char* descriptor) {
  DCHECK(GetDeclaringClass()->IsProxyClass());
  return Runtime::Current()->GetClassLinker()->FindSystemClass(Thread::Current(), descriptor);
}

mirror::Class* ArtField::ResolveGetType(uint32_t type_idx) {
  return Runtime::Current()->GetClassLinker()->ResolveType(type_idx, this);
}

mirror::String* ArtField::ResolveGetStringName(Thread* self, const DexFile& dex_file,
                                               uint32_t string_idx, mirror::DexCache* dex_cache) {
  StackHandleScope<1> hs(self);
  return Runtime::Current()->GetClassLinker()->ResolveString(
      dex_file, string_idx, hs.NewHandle(dex_cache));
}

}  // namespace art
