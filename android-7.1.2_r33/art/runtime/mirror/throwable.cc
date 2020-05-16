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

#include "throwable.h"

#include "art_method-inl.h"
#include "class-inl.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "object-inl.h"
#include "object_array.h"
#include "object_array-inl.h"
#include "stack_trace_element.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

GcRoot<Class> Throwable::java_lang_Throwable_;

void Throwable::SetDetailMessage(String* new_detail_message) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Throwable, detail_message_), new_detail_message);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Throwable, detail_message_),
                          new_detail_message);
  }
}

void Throwable::SetCause(Throwable* cause) {
  CHECK(cause != nullptr);
  CHECK(cause != this);
  Throwable* current_cause = GetFieldObject<Throwable>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_));
  CHECK(current_cause == nullptr || current_cause == this);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), cause);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), cause);
  }
}

void Throwable::SetStackState(Object* state) SHARED_REQUIRES(Locks::mutator_lock_) {
  CHECK(state != nullptr);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObjectVolatile<true>(OFFSET_OF_OBJECT_MEMBER(Throwable, backtrace_), state);
  } else {
    SetFieldObjectVolatile<false>(OFFSET_OF_OBJECT_MEMBER(Throwable, backtrace_), state);
  }
}

bool Throwable::IsCheckedException() {
  if (InstanceOf(WellKnownClasses::ToClass(WellKnownClasses::java_lang_Error))) {
    return false;
  }
  return !InstanceOf(WellKnownClasses::ToClass(WellKnownClasses::java_lang_RuntimeException));
}

int32_t Throwable::GetStackDepth() {
  Object* stack_state = GetStackState();
  if (stack_state == nullptr || !stack_state->IsObjectArray()) {
    return -1;
  }
  mirror::ObjectArray<mirror::Object>* const trace = stack_state->AsObjectArray<mirror::Object>();
  const int32_t array_len = trace->GetLength();
  DCHECK_GT(array_len, 0);
  // See method BuildInternalStackTraceVisitor::Init for the format.
  return array_len - 1;
}

std::string Throwable::Dump() {
  std::string result(PrettyTypeOf(this));
  result += ": ";
  String* msg = GetDetailMessage();
  if (msg != nullptr) {
    result += msg->ToModifiedUtf8();
  }
  result += "\n";
  Object* stack_state = GetStackState();
  // check stack state isn't missing or corrupt
  if (stack_state != nullptr && stack_state->IsObjectArray()) {
    mirror::ObjectArray<mirror::Object>* object_array =
        stack_state->AsObjectArray<mirror::Object>();
    // Decode the internal stack trace into the depth and method trace
    // See method BuildInternalStackTraceVisitor::Init for the format.
    DCHECK_GT(object_array->GetLength(), 0);
    mirror::Object* methods_and_dex_pcs = object_array->Get(0);
    DCHECK(methods_and_dex_pcs->IsIntArray() || methods_and_dex_pcs->IsLongArray());
    mirror::PointerArray* method_trace = down_cast<mirror::PointerArray*>(methods_and_dex_pcs);
    const int32_t array_len = method_trace->GetLength();
    CHECK_EQ(array_len % 2, 0);
    const auto depth = array_len / 2;
    if (depth == 0) {
      result += "(Throwable with empty stack trace)";
    } else {
      const size_t ptr_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
      for (int32_t i = 0; i < depth; ++i) {
        ArtMethod* method = method_trace->GetElementPtrSize<ArtMethod*>(i, ptr_size);
        uintptr_t dex_pc = method_trace->GetElementPtrSize<uintptr_t>(i + depth, ptr_size);
        int32_t line_number = method->GetLineNumFromDexPC(dex_pc);
        const char* source_file = method->GetDeclaringClassSourceFile();
        result += StringPrintf("  at %s (%s:%d)\n", PrettyMethod(method, true).c_str(),
                               source_file, line_number);
      }
    }
  } else {
    Object* stack_trace = GetStackTrace();
    if (stack_trace != nullptr && stack_trace->IsObjectArray()) {
      CHECK_EQ(stack_trace->GetClass()->GetComponentType(),
               StackTraceElement::GetStackTraceElement());
      auto* ste_array = down_cast<ObjectArray<StackTraceElement>*>(stack_trace);
      if (ste_array->GetLength() == 0) {
        result += "(Throwable with empty stack trace)";
      } else {
        for (int32_t i = 0; i < ste_array->GetLength(); ++i) {
          StackTraceElement* ste = ste_array->Get(i);
          DCHECK(ste != nullptr);
          auto* method_name = ste->GetMethodName();
          auto* file_name = ste->GetFileName();
          result += StringPrintf(
              "  at %s (%s:%d)\n",
              method_name != nullptr ? method_name->ToModifiedUtf8().c_str() : "<unknown method>",
              file_name != nullptr ? file_name->ToModifiedUtf8().c_str() : "(Unknown Source)",
              ste->GetLineNumber());
        }
      }
    } else {
      result += "(Throwable with no stack trace)";
    }
  }
  Throwable* cause = GetFieldObject<Throwable>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_));
  if (cause != nullptr && cause != this) {  // Constructor makes cause == this by default.
    result += "Caused by: ";
    result += cause->Dump();
  }
  return result;
}

void Throwable::SetClass(Class* java_lang_Throwable) {
  CHECK(java_lang_Throwable_.IsNull());
  CHECK(java_lang_Throwable != nullptr);
  java_lang_Throwable_ = GcRoot<Class>(java_lang_Throwable);
}

void Throwable::ResetClass() {
  CHECK(!java_lang_Throwable_.IsNull());
  java_lang_Throwable_ = GcRoot<Class>(nullptr);
}

void Throwable::VisitRoots(RootVisitor* visitor) {
  java_lang_Throwable_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

}  // namespace mirror
}  // namespace art
