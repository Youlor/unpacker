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

#include <dlfcn.h>

#include "code_simulator_container.h"
#include "globals.h"

namespace art {

CodeSimulatorContainer::CodeSimulatorContainer(InstructionSet target_isa)
    : libart_simulator_handle_(nullptr),
      simulator_(nullptr) {
  const char* libart_simulator_so_name =
      kIsDebugBuild ? "libartd-simulator.so" : "libart-simulator.so";
  libart_simulator_handle_ = dlopen(libart_simulator_so_name, RTLD_NOW);
  // It is not a real error when libart-simulator does not exist, e.g., on target.
  if (libart_simulator_handle_ == nullptr) {
    VLOG(simulator) << "Could not load " << libart_simulator_so_name << ": " << dlerror();
  } else {
    typedef CodeSimulator* (*create_code_simulator_ptr_)(InstructionSet target_isa);
    create_code_simulator_ptr_ create_code_simulator_ =
        reinterpret_cast<create_code_simulator_ptr_>(
            dlsym(libart_simulator_handle_, "CreateCodeSimulator"));
    DCHECK(create_code_simulator_ != nullptr) << "Fail to find symbol of CreateCodeSimulator: "
        << dlerror();
    simulator_ = create_code_simulator_(target_isa);
  }
}

CodeSimulatorContainer::~CodeSimulatorContainer() {
  // Free simulator object before closing libart-simulator because destructor of
  // CodeSimulator lives in it.
  if (simulator_ != nullptr) {
    delete simulator_;
  }
  if (libart_simulator_handle_ != nullptr) {
    dlclose(libart_simulator_handle_);
  }
}

}  // namespace art
