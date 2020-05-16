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

#include "linear_alloc.h"

#include "thread-inl.h"

namespace art {

LinearAlloc::LinearAlloc(ArenaPool* pool) : lock_("linear alloc"), allocator_(pool) {
}

void* LinearAlloc::Realloc(Thread* self, void* ptr, size_t old_size, size_t new_size) {
  MutexLock mu(self, lock_);
  return allocator_.Realloc(ptr, old_size, new_size);
}

void* LinearAlloc::Alloc(Thread* self, size_t size) {
  MutexLock mu(self, lock_);
  return allocator_.Alloc(size);
}

size_t LinearAlloc::GetUsedMemory() const {
  MutexLock mu(Thread::Current(), lock_);
  return allocator_.BytesUsed();
}

ArenaPool* LinearAlloc::GetArenaPool() {
  MutexLock mu(Thread::Current(), lock_);
  return allocator_.GetArenaPool();
}

bool LinearAlloc::Contains(void* ptr) const {
  MutexLock mu(Thread::Current(), lock_);
  return allocator_.Contains(ptr);
}

bool LinearAlloc::ContainsUnsafe(void* ptr) const {
  return allocator_.Contains(ptr);
}

}  // namespace art
