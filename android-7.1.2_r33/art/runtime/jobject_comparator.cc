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

#include "jobject_comparator.h"

#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

bool JobjectComparator::operator()(jobject jobj1, jobject jobj2) const {
  // Ensure null references and cleared jweaks appear at the end.
  if (jobj1 == nullptr) {
    return true;
  } else if (jobj2 == nullptr) {
    return false;
  }
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Object> obj1(hs.NewHandle(soa.Decode<mirror::Object*>(jobj1)));
  Handle<mirror::Object> obj2(hs.NewHandle(soa.Decode<mirror::Object*>(jobj2)));
  if (obj1.Get() == nullptr) {
    return true;
  } else if (obj2.Get() == nullptr) {
    return false;
  }
  // Sort by class...
  if (obj1->GetClass() != obj2->GetClass()) {
    return obj1->GetClass()->IdentityHashCode() < obj2->GetClass()->IdentityHashCode();
  }
  // ...then by size...
  const size_t count1 = obj1->SizeOf();
  const size_t count2 = obj2->SizeOf();
  if (count1 != count2) {
    return count1 < count2;
  }
  // ...and finally by identity hash code.
  return obj1->IdentityHashCode() < obj2->IdentityHashCode();
}

}  // namespace art
