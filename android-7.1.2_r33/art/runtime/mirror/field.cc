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

#include "field-inl.h"

#include "class-inl.h"
#include "dex_cache-inl.h"
#include "object_array-inl.h"
#include "object-inl.h"

namespace art {
namespace mirror {

GcRoot<Class> Field::static_class_;
GcRoot<Class> Field::array_class_;

void Field::SetClass(Class* klass) {
  CHECK(static_class_.IsNull()) << static_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  static_class_ = GcRoot<Class>(klass);
}

void Field::ResetClass() {
  CHECK(!static_class_.IsNull());
  static_class_ = GcRoot<Class>(nullptr);
}

void Field::SetArrayClass(Class* klass) {
  CHECK(array_class_.IsNull()) << array_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  array_class_ = GcRoot<Class>(klass);
}

void Field::ResetArrayClass() {
  CHECK(!array_class_.IsNull());
  array_class_ = GcRoot<Class>(nullptr);
}

void Field::VisitRoots(RootVisitor* visitor) {
  static_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
  array_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

ArtField* Field::GetArtField() {
  mirror::Class* declaring_class = GetDeclaringClass();
  if (UNLIKELY(declaring_class->IsProxyClass())) {
    DCHECK(IsStatic());
    DCHECK_EQ(declaring_class->NumStaticFields(), 2U);
    // 0 == Class[] interfaces; 1 == Class[][] throws;
    if (GetDexFieldIndex() == 0) {
      return &declaring_class->GetSFieldsPtr()->At(0);
    } else {
      DCHECK_EQ(GetDexFieldIndex(), 1U);
      return &declaring_class->GetSFieldsPtr()->At(1);
    }
  }
  mirror::DexCache* const dex_cache = declaring_class->GetDexCache();
  ArtField* const art_field = dex_cache->GetResolvedField(GetDexFieldIndex(), sizeof(void*));
  CHECK(art_field != nullptr);
  CHECK_EQ(declaring_class, art_field->GetDeclaringClass());
  return art_field;
}

}  // namespace mirror
}  // namespace art
