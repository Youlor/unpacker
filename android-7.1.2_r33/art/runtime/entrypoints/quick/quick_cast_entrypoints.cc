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

#include "mirror/class-inl.h"
#include "mirror/object-inl.h"

namespace art {

// Assignable test for code, won't throw.  Null and equality tests already performed
extern "C" uint32_t artIsAssignableFromCode(mirror::Class* klass, mirror::Class* ref_class)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(klass != nullptr);
  DCHECK(ref_class != nullptr);
  return klass->IsAssignableFrom(ref_class) ? 1 : 0;
}

}  // namespace art
