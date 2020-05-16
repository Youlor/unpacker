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

#include "art_method.h"
#include "utils.h"  // For RoundUp().

namespace art {

// Assembly stub that does the final part of the up-call into Java.
extern "C" void art_quick_invoke_stub_internal(ArtMethod*, uint32_t*, uint32_t,
                                               Thread* self, JValue* result, uint32_t, uint32_t*,
                                               uint32_t*);

template <bool kIsStatic>
static void quick_invoke_reg_setup(ArtMethod* method, uint32_t* args, uint32_t args_size,
                                   Thread* self, JValue* result, const char* shorty) {
  // Note: We do not follow aapcs ABI in quick code for both softfp and hardfp.
  uint32_t core_reg_args[4];  // r0 ~ r3
  uint32_t fp_reg_args[16];  // s0 ~ s15 (d0 ~ d7)
  uint32_t gpr_index = 1;  // Index into core registers. Reserve r0 for ArtMethod*.
  uint32_t fpr_index = 0;  // Index into float registers.
  uint32_t fpr_double_index = 0;  // Index into float registers for doubles.
  uint32_t arg_index = 0;  // Index into argument array.
  const uint32_t result_in_float = kArm32QuickCodeUseSoftFloat ? 0 :
      (shorty[0] == 'F' || shorty[0] == 'D') ? 1 : 0;

  if (!kIsStatic) {
    // Copy receiver for non-static methods.
    core_reg_args[gpr_index++] = args[arg_index++];
  }

  for (uint32_t shorty_index = 1; shorty[shorty_index] != '\0'; ++shorty_index, ++arg_index) {
    char arg_type = shorty[shorty_index];
    if (kArm32QuickCodeUseSoftFloat) {
      arg_type = (arg_type == 'D') ? 'J' : arg_type;  // Regard double as long.
      arg_type = (arg_type == 'F') ? 'I' : arg_type;  // Regard float as int.
    }
    switch (arg_type) {
      case 'D': {
        // Copy double argument into fp_reg_args if there are still floating point reg arguments.
        // Double should not overlap with float.
        fpr_double_index = std::max(fpr_double_index, RoundUp(fpr_index, 2));
        if (fpr_double_index < arraysize(fp_reg_args)) {
          fp_reg_args[fpr_double_index++] = args[arg_index];
          fp_reg_args[fpr_double_index++] = args[arg_index + 1];
        }
        ++arg_index;
        break;
      }
      case 'F':
        // Copy float argument into fp_reg_args if there are still floating point reg arguments.
        // If fpr_index is odd then its pointing at a hole next to an existing float argument. If we
        // encounter a float argument then pick it up from that hole. In the case fpr_index is even,
        // ensure that we don't pick up an argument that overlaps with with a double from
        // fpr_double_index. In either case, take care not to go beyond the maximum number of
        // floating point arguments.
        if (fpr_index % 2 == 0) {
          fpr_index = std::max(fpr_double_index, fpr_index);
        }
        if (fpr_index < arraysize(fp_reg_args)) {
          fp_reg_args[fpr_index++] = args[arg_index];
        }
        break;
      case 'J':
        if (gpr_index == 1 && !kArm32QuickCodeUseSoftFloat) {
          // Don't use r1-r2 as a register pair, move to r2-r3 instead.
          gpr_index++;
        }
        if (gpr_index < arraysize(core_reg_args)) {
          // Note that we don't need to do this if two registers are not available
          // when !kArm32QuickCodeUseSoftFloat. We do it anyway to leave this
          // code simple.
          core_reg_args[gpr_index++] = args[arg_index];
        }
        ++arg_index;
        FALLTHROUGH_INTENDED;  // Fall-through to take of the high part.
      default:
        if (gpr_index < arraysize(core_reg_args)) {
          core_reg_args[gpr_index++] = args[arg_index];
        }
        break;
    }
  }

  art_quick_invoke_stub_internal(method, args, args_size, self, result, result_in_float,
      core_reg_args, fp_reg_args);
}

// Called by art::ArtMethod::Invoke to do entry into a non-static method.
// TODO: migrate into an assembly implementation as with ARM64.
extern "C" void art_quick_invoke_stub(ArtMethod* method, uint32_t* args, uint32_t args_size,
                                      Thread* self, JValue* result, const char* shorty) {
  quick_invoke_reg_setup<false>(method, args, args_size, self, result, shorty);
}

// Called by art::ArtMethod::Invoke to do entry into a static method.
// TODO: migrate into an assembly implementation as with ARM64.
extern "C" void art_quick_invoke_static_stub(ArtMethod* method, uint32_t* args,
                                             uint32_t args_size, Thread* self, JValue* result,
                                             const char* shorty) {
  quick_invoke_reg_setup<true>(method, args, args_size, self, result, shorty);
}

}  // namespace art
