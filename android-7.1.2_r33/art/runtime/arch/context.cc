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

#include "context.h"

#if defined(__arm__)
#include "arm/context_arm.h"
#elif defined(__aarch64__)
#include "arm64/context_arm64.h"
#elif defined(__mips__) && !defined(__LP64__)
#include "mips/context_mips.h"
#elif defined(__mips__) && defined(__LP64__)
#include "mips64/context_mips64.h"
#elif defined(__i386__)
#include "x86/context_x86.h"
#elif defined(__x86_64__)
#include "x86_64/context_x86_64.h"
#else
#include "base/logging.h"
#endif

namespace art {

Context* Context::Create() {
#if defined(__arm__)
  return new arm::ArmContext();
#elif defined(__aarch64__)
  return new arm64::Arm64Context();
#elif defined(__mips__) && !defined(__LP64__)
  return new mips::MipsContext();
#elif defined(__mips__) && defined(__LP64__)
  return new mips64::Mips64Context();
#elif defined(__i386__)
  return new x86::X86Context();
#elif defined(__x86_64__)
  return new x86_64::X86_64Context();
#else
  UNIMPLEMENTED(FATAL);
  return nullptr;
#endif
}

}  // namespace art
