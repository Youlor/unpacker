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

#include "entrypoints/jni/jni_entrypoints.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "entrypoints/quick/quick_default_externs.h"
#include "entrypoints/quick/quick_default_init_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/math_entrypoints.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"

namespace art {

// Cast entrypoints.
extern "C" uint32_t artIsAssignableFromCode(const mirror::Class* klass,
                                            const mirror::Class* ref_class);


// Used by soft float.
// Single-precision FP arithmetics.
extern "C" float fmodf(float a, float b);              // REM_FLOAT[_2ADDR]
// Double-precision FP arithmetics.
extern "C" double fmod(double a, double b);            // REM_DOUBLE[_2ADDR]

// Used by hard float.
extern "C" float art_quick_fmodf(float a, float b);    // REM_FLOAT[_2ADDR]
extern "C" double art_quick_fmod(double a, double b);  // REM_DOUBLE[_2ADDR]

// Integer arithmetics.
extern "C" int __aeabi_idivmod(int32_t, int32_t);  // [DIV|REM]_INT[_2ADDR|_LIT8|_LIT16]

// Long long arithmetics - REM_LONG[_2ADDR] and DIV_LONG[_2ADDR]
extern "C" int64_t __aeabi_ldivmod(int64_t, int64_t);

void InitEntryPoints(JniEntryPoints* jpoints, QuickEntryPoints* qpoints) {
  DefaultInitEntryPoints(jpoints, qpoints);

  // Cast
  qpoints->pInstanceofNonTrivial = artIsAssignableFromCode;
  qpoints->pCheckCast = art_quick_check_cast;

  // Math
  qpoints->pIdivmod = __aeabi_idivmod;
  qpoints->pLdiv = __aeabi_ldivmod;
  qpoints->pLmod = __aeabi_ldivmod;  // result returned in r2:r3
  qpoints->pLmul = art_quick_mul_long;
  qpoints->pShlLong = art_quick_shl_long;
  qpoints->pShrLong = art_quick_shr_long;
  qpoints->pUshrLong = art_quick_ushr_long;
  if (kArm32QuickCodeUseSoftFloat) {
    qpoints->pFmod = fmod;
    qpoints->pFmodf = fmodf;
    qpoints->pD2l = art_d2l;
    qpoints->pF2l = art_f2l;
    qpoints->pL2f = art_l2f;
  } else {
    qpoints->pFmod = art_quick_fmod;
    qpoints->pFmodf = art_quick_fmodf;
    qpoints->pD2l = art_quick_d2l;
    qpoints->pF2l = art_quick_f2l;
    qpoints->pL2f = art_quick_l2f;
  }

  // More math.
  qpoints->pCos = cos;
  qpoints->pSin = sin;
  qpoints->pAcos = acos;
  qpoints->pAsin = asin;
  qpoints->pAtan = atan;
  qpoints->pAtan2 = atan2;
  qpoints->pCbrt = cbrt;
  qpoints->pCosh = cosh;
  qpoints->pExp = exp;
  qpoints->pExpm1 = expm1;
  qpoints->pHypot = hypot;
  qpoints->pLog = log;
  qpoints->pLog10 = log10;
  qpoints->pNextAfter = nextafter;
  qpoints->pSinh = sinh;
  qpoints->pTan = tan;
  qpoints->pTanh = tanh;

  // Intrinsics
  qpoints->pIndexOf = art_quick_indexof;
  qpoints->pStringCompareTo = art_quick_string_compareto;
  qpoints->pMemcpy = memcpy;

  // Read barrier.
  qpoints->pReadBarrierJni = ReadBarrierJni;
  qpoints->pReadBarrierMark = artReadBarrierMark;
  qpoints->pReadBarrierSlow = artReadBarrierSlow;
  qpoints->pReadBarrierForRootSlow = artReadBarrierForRootSlow;
}

}  // namespace art
