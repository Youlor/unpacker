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

#include "space_test.h"

namespace art {
namespace gc {
namespace space {

MallocSpace* CreateRosAllocSpace(const std::string& name, size_t initial_size, size_t growth_limit,
                                 size_t capacity, uint8_t* requested_begin) {
  return RosAllocSpace::Create(name, initial_size, growth_limit, capacity, requested_begin,
                               Runtime::Current()->GetHeap()->IsLowMemoryMode(), false);
}

TEST_SPACE_CREATE_FN_STATIC(RosAllocSpace, CreateRosAllocSpace)


}  // namespace space
}  // namespace gc
}  // namespace art
