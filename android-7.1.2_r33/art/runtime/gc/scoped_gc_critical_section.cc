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

#include "scoped_gc_critical_section.h"

#include "gc/collector_type.h"
#include "gc/heap.h"
#include "runtime.h"
#include "thread-inl.h"

namespace art {
namespace gc {

ScopedGCCriticalSection::ScopedGCCriticalSection(Thread* self,
                                                 GcCause cause,
                                                 CollectorType collector_type)
    : self_(self) {
  Runtime::Current()->GetHeap()->StartGC(self, cause, collector_type);
  old_cause_ = self->StartAssertNoThreadSuspension("ScopedGCCriticalSection");
}
ScopedGCCriticalSection::~ScopedGCCriticalSection() {
  self_->EndAssertNoThreadSuspension(old_cause_);
  Runtime::Current()->GetHeap()->FinishGC(self_, collector::kGcTypeNone);
}

}  // namespace gc
}  // namespace art
