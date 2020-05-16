/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "allocator.h"

#include <inttypes.h>
#include <stdlib.h>

#include "atomic.h"
#include "base/logging.h"
#include "thread-inl.h"

namespace art {

class MallocAllocator FINAL : public Allocator {
 public:
  explicit MallocAllocator() {}
  ~MallocAllocator() {}

  void* Alloc(size_t size) {
    return calloc(sizeof(uint8_t), size);
  }

  void Free(void* p) {
    free(p);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MallocAllocator);
};

MallocAllocator g_malloc_allocator;

class NoopAllocator FINAL : public Allocator {
 public:
  explicit NoopAllocator() {}
  ~NoopAllocator() {}

  void* Alloc(size_t size ATTRIBUTE_UNUSED) {
    LOG(FATAL) << "NoopAllocator::Alloc should not be called";
    UNREACHABLE();
  }

  void Free(void* p ATTRIBUTE_UNUSED) {
    // Noop.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(NoopAllocator);
};

NoopAllocator g_noop_allocator;

Allocator* Allocator::GetMallocAllocator() {
  return &g_malloc_allocator;
}

Allocator* Allocator::GetNoopAllocator() {
  return &g_noop_allocator;
}

namespace TrackedAllocators {

// These globals are safe since they don't have any non-trivial destructors.
Atomic<size_t> g_bytes_used[kAllocatorTagCount];
volatile size_t g_max_bytes_used[kAllocatorTagCount];
Atomic<uint64_t> g_total_bytes_used[kAllocatorTagCount];

void Dump(std::ostream& os) {
  if (kEnableTrackingAllocator) {
    os << "Dumping native memory usage\n";
    for (size_t i = 0; i < kAllocatorTagCount; ++i) {
      uint64_t bytes_used = g_bytes_used[i].LoadRelaxed();
      uint64_t max_bytes_used = g_max_bytes_used[i];
      uint64_t total_bytes_used = g_total_bytes_used[i].LoadRelaxed();
      if (total_bytes_used != 0) {
        os << static_cast<AllocatorTag>(i) << " active=" << bytes_used << " max="
           << max_bytes_used << " total=" << total_bytes_used << "\n";
      }
    }
  }
}

}  // namespace TrackedAllocators

}  // namespace art
