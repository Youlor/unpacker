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

#include "base/bit_utils.h"
#include "lambda/leaking_allocator.h"
#include "linear_alloc.h"
#include "runtime.h"

namespace art {
namespace lambda {

void* LeakingAllocator::AllocateMemoryImpl(Thread* self, size_t byte_size, size_t align_size) {
  // TODO: use GetAllocatorForClassLoader to allocate lambda ArtMethod data.
  void* mem = Runtime::Current()->GetLinearAlloc()->Alloc(self, byte_size);
  DCHECK_ALIGNED_PARAM(reinterpret_cast<uintptr_t>(mem), align_size);
  return mem;
}

}  // namespace lambda
}  // namespace art
