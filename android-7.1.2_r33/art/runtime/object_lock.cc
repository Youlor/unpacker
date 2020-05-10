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

#include "object_lock.h"

#include "mirror/object-inl.h"
#include "monitor.h"

namespace art {

template <typename T>
ObjectLock<T>::ObjectLock(Thread* self, Handle<T> object) : self_(self), obj_(object) {
  CHECK(object.Get() != nullptr);
  obj_->MonitorEnter(self_);
}

template <typename T>
ObjectLock<T>::~ObjectLock() {
  obj_->MonitorExit(self_);
}

template <typename T>
void ObjectLock<T>::WaitIgnoringInterrupts() {
  Monitor::Wait(self_, obj_.Get(), 0, 0, false, kWaiting);
}

template <typename T>
void ObjectLock<T>::Notify() {
  obj_->Notify(self_);
}

template <typename T>
void ObjectLock<T>::NotifyAll() {
  obj_->NotifyAll(self_);
}

template <typename T>
ObjectTryLock<T>::ObjectTryLock(Thread* self, Handle<T> object) : self_(self), obj_(object) {
  CHECK(object.Get() != nullptr);
  acquired_ = obj_->MonitorTryEnter(self_) != nullptr;
}

template <typename T>
ObjectTryLock<T>::~ObjectTryLock() {
  if (acquired_) {
    obj_->MonitorExit(self_);
  }
}

template class ObjectLock<mirror::Class>;
template class ObjectLock<mirror::Object>;
template class ObjectTryLock<mirror::Class>;
template class ObjectTryLock<mirror::Object>;

}  // namespace art
