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

#include "atomic.h"
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
// Math entrypoints.
extern int32_t CmpgDouble(double a, double b);
extern int32_t CmplDouble(double a, double b);
extern int32_t CmpgFloat(float a, float b);
extern int32_t CmplFloat(float a, float b);
extern "C" int64_t artLmul(int64_t a, int64_t b);
extern "C" int64_t artLdiv(int64_t a, int64_t b);
extern "C" int64_t artLmod(int64_t a, int64_t b);

// Math conversions.
extern "C" int32_t __fixsfsi(float op1);      // FLOAT_TO_INT
extern "C" int32_t __fixdfsi(double op1);     // DOUBLE_TO_INT
extern "C" float __floatdisf(int64_t op1);    // LONG_TO_FLOAT
extern "C" double __floatdidf(int64_t op1);   // LONG_TO_DOUBLE
extern "C" int64_t __fixsfdi(float op1);      // FLOAT_TO_LONG
extern "C" int64_t __fixdfdi(double op1);     // DOUBLE_TO_LONG

// Single-precision FP arithmetics.
extern "C" float fmodf(float a, float b);      // REM_FLOAT[_2ADDR]

// Double-precision FP arithmetics.
extern "C" double fmod(double a, double b);     // REM_DOUBLE[_2ADDR]

// Long long arithmetics - REM_LONG[_2ADDR] and DIV_LONG[_2ADDR]
extern "C" int64_t __divdi3(int64_t, int64_t);
extern "C" int64_t __moddi3(int64_t, int64_t);

void InitEntryPoints(JniEntryPoints* jpoints, QuickEntryPoints* qpoints) {
  DefaultInitEntryPoints(jpoints, qpoints);

  // Cast
  qpoints->pInstanceofNonTrivial = artIsAssignableFromCode;
  qpoints->pCheckCast = art_quick_check_cast;

  // Math
  qpoints->pCmpgDouble = CmpgDouble;
  qpoints->pCmpgFloat = CmpgFloat;
  qpoints->pCmplDouble = CmplDouble;
  qpoints->pCmplFloat = CmplFloat;
  qpoints->pFmod = fmod;
  qpoints->pL2d = art_l2d;
  qpoints->pFmodf = fmodf;
  qpoints->pL2f = art_l2f;
  qpoints->pD2iz = art_d2i;
  qpoints->pF2iz = art_f2i;
  qpoints->pIdivmod = nullptr;
  qpoints->pD2l = art_d2l;
  qpoints->pF2l = art_f2l;
  qpoints->pLdiv = artLdiv;
  qpoints->pLmod = artLmod;
  qpoints->pLmul = artLmul;
  qpoints->pShlLong = nullptr;
  qpoints->pShrLong = nullptr;
  qpoints->pUshrLong = nullptr;

  // Intrinsics
  qpoints->pIndexOf = art_quick_indexof;
  qpoints->pStringCompareTo = art_quick_string_compareto;
  qpoints->pMemcpy = memcpy;

  // TODO - use lld/scd instructions for Mips64
  // Atomic 64-bit load/store
  qpoints->pA64Load = QuasiAtomic::Read64;
  qpoints->pA64Store = QuasiAtomic::Write64;

  // Read barrier.
  qpoints->pReadBarrierJni = ReadBarrierJni;
  qpoints->pReadBarrierMark = artReadBarrierMark;
  qpoints->pReadBarrierSlow = artReadBarrierSlow;
  qpoints->pReadBarrierForRootSlow = artReadBarrierForRootSlow;
};

}  // namespace art
