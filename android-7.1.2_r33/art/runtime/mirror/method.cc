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

#include "method.h"

#include "art_method.h"
#include "gc_root-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"

namespace art {
namespace mirror {

GcRoot<Class> Method::static_class_;
GcRoot<Class> Method::array_class_;
GcRoot<Class> Constructor::static_class_;
GcRoot<Class> Constructor::array_class_;

void Method::SetClass(Class* klass) {
  CHECK(static_class_.IsNull()) << static_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  static_class_ = GcRoot<Class>(klass);
}

void Method::ResetClass() {
  CHECK(!static_class_.IsNull());
  static_class_ = GcRoot<Class>(nullptr);
}

void Method::SetArrayClass(Class* klass) {
  CHECK(array_class_.IsNull()) << array_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  array_class_ = GcRoot<Class>(klass);
}

void Method::ResetArrayClass() {
  CHECK(!array_class_.IsNull());
  array_class_ = GcRoot<Class>(nullptr);
}

template <bool kTransactionActive>
Method* Method::CreateFromArtMethod(Thread* self, ArtMethod* method) {
  DCHECK(!method->IsConstructor()) << PrettyMethod(method);
  auto* ret = down_cast<Method*>(StaticClass()->AllocObject(self));
  if (LIKELY(ret != nullptr)) {
    static_cast<AbstractMethod*>(ret)->CreateFromArtMethod<kTransactionActive>(method);
  }
  return ret;
}

template Method* Method::CreateFromArtMethod<false>(Thread* self, ArtMethod* method);
template Method* Method::CreateFromArtMethod<true>(Thread* self, ArtMethod* method);

void Method::VisitRoots(RootVisitor* visitor) {
  static_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
  array_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

void Constructor::SetClass(Class* klass) {
  CHECK(static_class_.IsNull()) << static_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  static_class_ = GcRoot<Class>(klass);
}

void Constructor::ResetClass() {
  CHECK(!static_class_.IsNull());
  static_class_ = GcRoot<Class>(nullptr);
}

void Constructor::SetArrayClass(Class* klass) {
  CHECK(array_class_.IsNull()) << array_class_.Read() << " " << klass;
  CHECK(klass != nullptr);
  array_class_ = GcRoot<Class>(klass);
}

void Constructor::ResetArrayClass() {
  CHECK(!array_class_.IsNull());
  array_class_ = GcRoot<Class>(nullptr);
}

void Constructor::VisitRoots(RootVisitor* visitor) {
  static_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
  array_class_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

template <bool kTransactionActive>
Constructor* Constructor::CreateFromArtMethod(Thread* self, ArtMethod* method) {
  DCHECK(method->IsConstructor()) << PrettyMethod(method);
  auto* ret = down_cast<Constructor*>(StaticClass()->AllocObject(self));
  if (LIKELY(ret != nullptr)) {
    static_cast<AbstractMethod*>(ret)->CreateFromArtMethod<kTransactionActive>(method);
  }
  return ret;
}

template Constructor* Constructor::CreateFromArtMethod<false>(Thread* self, ArtMethod* method);
template Constructor* Constructor::CreateFromArtMethod<true>(Thread* self, ArtMethod* method);

}  // namespace mirror
}  // namespace art
