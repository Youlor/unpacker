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

#include "lambda/art_lambda_method.h"

#include "base/logging.h"
#include "lambda/shorty_field_type.h"

namespace art {
namespace lambda {

ArtLambdaMethod::ArtLambdaMethod(ArtMethod* target_method,
                                 const char* captured_variables_type_descriptor,
                                 const char* captured_variables_shorty,
                                 bool innate_lambda)
    : method_(target_method),
      captured_variables_type_descriptor_(captured_variables_type_descriptor),
      captured_variables_shorty_(captured_variables_shorty),
      innate_lambda_(innate_lambda) {
  DCHECK(target_method != nullptr);
  DCHECK(captured_variables_type_descriptor != nullptr);
  DCHECK(captured_variables_shorty != nullptr);

  // Calculate the static closure size from the captured variables.
  size_t size = sizeof(ArtLambdaMethod*);  // Initial size is just this method.
  bool static_size = true;
  const char* shorty = captured_variables_shorty_;
  while (shorty != nullptr && *shorty != '\0') {
    // Each captured variable also appends to the size.
    ShortyFieldType shorty_field{*shorty};  // NOLINT [readability/braces] [4]
    size += shorty_field.GetStaticSize();
    static_size &= shorty_field.IsStaticSize();
    ++shorty;
  }
  closure_size_ = size;

  // We determine whether or not the size is dynamic by checking for nested lambdas.
  //
  // This is conservative, since in theory an optimization could determine the size
  // of the nested lambdas recursively. In practice it's probably better to flatten out
  // nested lambdas and inline all their code if they are known statically.
  dynamic_size_ = !static_size;

  if (kIsDebugBuild) {
    // Double check that the number of captured variables match in both strings.
    size_t shorty_count = strlen(captured_variables_shorty);

    size_t long_count = 0;
    const char* long_type = captured_variables_type_descriptor;
    ShortyFieldType out;
    while ((long_type = ShortyFieldType::ParseFromFieldTypeDescriptor(long_type, &out))
           != nullptr) {
      ++long_count;
    }

    DCHECK_EQ(shorty_count, long_count)
        << "number of captured variables in long type '" << captured_variables_type_descriptor
        << "' (" << long_count << ")" << " did not match short type '"
        << captured_variables_shorty << "' (" << shorty_count << ")";
  }
}

}  // namespace lambda
}  // namespace art
