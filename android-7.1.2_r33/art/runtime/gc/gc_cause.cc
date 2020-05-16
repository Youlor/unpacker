/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "gc_cause.h"
#include "globals.h"
#include "base/logging.h"

#include <ostream>

namespace art {
namespace gc {

const char* PrettyCause(GcCause cause) {
  switch (cause) {
    case kGcCauseForAlloc: return "Alloc";
    case kGcCauseBackground: return "Background";
    case kGcCauseExplicit: return "Explicit";
    case kGcCauseForNativeAlloc: return "NativeAlloc";
    case kGcCauseCollectorTransition: return "CollectorTransition";
    case kGcCauseDisableMovingGc: return "DisableMovingGc";
    case kGcCauseHomogeneousSpaceCompact: return "HomogeneousSpaceCompact";
    case kGcCauseTrim: return "HeapTrim";
    case kGcCauseInstrumentation: return "Instrumentation";
    case kGcCauseAddRemoveAppImageSpace: return "AddRemoveAppImageSpace";
    case kGcCauseClassLinker: return "ClassLinker";
    case kGcCauseJitCodeCache: return "JitCodeCache";
    default:
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
  }
}

std::ostream& operator<<(std::ostream& os, const GcCause& gc_cause) {
  os << PrettyCause(gc_cause);
  return os;
}

}  // namespace gc
}  // namespace art
