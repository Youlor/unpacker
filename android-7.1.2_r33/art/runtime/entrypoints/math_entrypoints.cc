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

#include "math_entrypoints.h"

#include "entrypoint_utils-inl.h"

namespace art {

extern "C" double art_l2d(int64_t l) {
  return static_cast<double>(l);
}

extern "C" float art_l2f(int64_t l) {
  return static_cast<float>(l);
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
extern "C" int64_t art_d2l(double d) {
  return art_float_to_integral<int64_t, double>(d);
}

extern "C" int64_t art_f2l(float f) {
  return art_float_to_integral<int64_t, float>(f);
}

extern "C" int32_t art_d2i(double d) {
  return art_float_to_integral<int32_t, double>(d);
}

extern "C" int32_t art_f2i(float f) {
  return art_float_to_integral<int32_t, float>(f);
}

}  // namespace art
