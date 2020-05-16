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

#include "reflection-inl.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "indirect_reference_table-inl.h"
#include "jni_internal.h"
#include "mirror/abstract_method.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "nth_caller_visitor.h"
#include "scoped_thread_state_change.h"
#include "stack.h"
#include "well_known_classes.h"

namespace art {

class ArgArray {
 public:
  ArgArray(const char* shorty, uint32_t shorty_len)
      : shorty_(shorty), shorty_len_(shorty_len), num_bytes_(0) {
    size_t num_slots = shorty_len + 1;  // +1 in case of receiver.
    if (LIKELY((num_slots * 2) < kSmallArgArraySize)) {
      // We can trivially use the small arg array.
      arg_array_ = small_arg_array_;
    } else {
      // Analyze shorty to see if we need the large arg array.
      for (size_t i = 1; i < shorty_len; ++i) {
        char c = shorty[i];
        if (c == 'J' || c == 'D') {
          num_slots++;
        }
      }
      if (num_slots <= kSmallArgArraySize) {
        arg_array_ = small_arg_array_;
      } else {
        large_arg_array_.reset(new uint32_t[num_slots]);
        arg_array_ = large_arg_array_.get();
      }
    }
  }

  uint32_t* GetArray() {
    return arg_array_;
  }

  uint32_t GetNumBytes() {
    return num_bytes_;
  }

  void Append(uint32_t value) {
    arg_array_[num_bytes_ / 4] = value;
    num_bytes_ += 4;
  }

  void Append(mirror::Object* obj) SHARED_REQUIRES(Locks::mutator_lock_) {
    Append(StackReference<mirror::Object>::FromMirrorPtr(obj).AsVRegValue());
  }

  void AppendWide(uint64_t value) {
    arg_array_[num_bytes_ / 4] = value;
    arg_array_[(num_bytes_ / 4) + 1] = value >> 32;
    num_bytes_ += 8;
  }

  void AppendFloat(float value) {
    jvalue jv;
    jv.f = value;
    Append(jv.i);
  }

  void AppendDouble(double value) {
    jvalue jv;
    jv.d = value;
    AppendWide(jv.j);
  }

  void BuildArgArrayFromVarArgs(const ScopedObjectAccessAlreadyRunnable& soa,
                                mirror::Object* receiver, va_list ap)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    // Set receiver if non-null (method is not static)
    if (receiver != nullptr) {
      Append(receiver);
    }
    for (size_t i = 1; i < shorty_len_; ++i) {
      switch (shorty_[i]) {
        case 'Z':
        case 'B':
        case 'C':
        case 'S':
        case 'I':
          Append(va_arg(ap, jint));
          break;
        case 'F':
          AppendFloat(va_arg(ap, jdouble));
          break;
        case 'L':
          Append(soa.Decode<mirror::Object*>(va_arg(ap, jobject)));
          break;
        case 'D':
          AppendDouble(va_arg(ap, jdouble));
          break;
        case 'J':
          AppendWide(va_arg(ap, jlong));
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
    }
  }

  void BuildArgArrayFromJValues(const ScopedObjectAccessAlreadyRunnable& soa,
                                mirror::Object* receiver, jvalue* args)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    // Set receiver if non-null (method is not static)
    if (receiver != nullptr) {
      Append(receiver);
    }
    for (size_t i = 1, args_offset = 0; i < shorty_len_; ++i, ++args_offset) {
      switch (shorty_[i]) {
        case 'Z':
          Append(args[args_offset].z);
          break;
        case 'B':
          Append(args[args_offset].b);
          break;
        case 'C':
          Append(args[args_offset].c);
          break;
        case 'S':
          Append(args[args_offset].s);
          break;
        case 'I':
        case 'F':
          Append(args[args_offset].i);
          break;
        case 'L':
          Append(soa.Decode<mirror::Object*>(args[args_offset].l));
          break;
        case 'D':
        case 'J':
          AppendWide(args[args_offset].j);
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
    }
  }

  void BuildArgArrayFromFrame(ShadowFrame* shadow_frame, uint32_t arg_offset)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    // Set receiver if non-null (method is not static)
    size_t cur_arg = arg_offset;
    if (!shadow_frame->GetMethod()->IsStatic()) {
      Append(shadow_frame->GetVReg(cur_arg));
      cur_arg++;
    }
    for (size_t i = 1; i < shorty_len_; ++i) {
      switch (shorty_[i]) {
        case 'Z':
        case 'B':
        case 'C':
        case 'S':
        case 'I':
        case 'F':
        case 'L':
          Append(shadow_frame->GetVReg(cur_arg));
          cur_arg++;
          break;
        case 'D':
        case 'J':
          AppendWide(shadow_frame->GetVRegLong(cur_arg));
          cur_arg++;
          cur_arg++;
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
#endif
      }
    }
  }

  static void ThrowIllegalPrimitiveArgumentException(const char* expected,
                                                     const char* found_descriptor)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ThrowIllegalArgumentException(
        StringPrintf("Invalid primitive conversion from %s to %s", expected,
                     PrettyDescriptor(found_descriptor).c_str()).c_str());
  }

  bool BuildArgArrayFromObjectArray(mirror::Object* receiver,
                                    mirror::ObjectArray<mirror::Object>* args, ArtMethod* m)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const DexFile::TypeList* classes = m->GetParameterTypeList();
    // Set receiver if non-null (method is not static)
    if (receiver != nullptr) {
      Append(receiver);
    }
    for (size_t i = 1, args_offset = 0; i < shorty_len_; ++i, ++args_offset) {
      mirror::Object* arg = args->Get(args_offset);
      if (((shorty_[i] == 'L') && (arg != nullptr)) || ((arg == nullptr && shorty_[i] != 'L'))) {
        size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
        mirror::Class* dst_class =
            m->GetClassFromTypeIndex(classes->GetTypeItem(args_offset).type_idx_,
                                     true /* resolve */,
                                     pointer_size);
        if (UNLIKELY(arg == nullptr || !arg->InstanceOf(dst_class))) {
          ThrowIllegalArgumentException(
              StringPrintf("method %s argument %zd has type %s, got %s",
                  PrettyMethod(m, false).c_str(),
                  args_offset + 1,  // Humans don't count from 0.
                  PrettyDescriptor(dst_class).c_str(),
                  PrettyTypeOf(arg).c_str()).c_str());
          return false;
        }
      }

#define DO_FIRST_ARG(match_descriptor, get_fn, append) { \
          if (LIKELY(arg != nullptr && arg->GetClass<>()->DescriptorEquals(match_descriptor))) { \
            ArtField* primitive_field = arg->GetClass()->GetInstanceField(0); \
            append(primitive_field-> get_fn(arg));

#define DO_ARG(match_descriptor, get_fn, append) \
          } else if (LIKELY(arg != nullptr && \
                            arg->GetClass<>()->DescriptorEquals(match_descriptor))) { \
            ArtField* primitive_field = arg->GetClass()->GetInstanceField(0); \
            append(primitive_field-> get_fn(arg));

#define DO_FAIL(expected) \
          } else { \
            if (arg->GetClass<>()->IsPrimitive()) { \
              std::string temp; \
              ThrowIllegalPrimitiveArgumentException(expected, \
                                                     arg->GetClass<>()->GetDescriptor(&temp)); \
            } else { \
              ThrowIllegalArgumentException(\
                  StringPrintf("method %s argument %zd has type %s, got %s", \
                      PrettyMethod(m, false).c_str(), \
                      args_offset + 1, \
                      expected, \
                      PrettyTypeOf(arg).c_str()).c_str()); \
            } \
            return false; \
          } }

      switch (shorty_[i]) {
        case 'L':
          Append(arg);
          break;
        case 'Z':
          DO_FIRST_ARG("Ljava/lang/Boolean;", GetBoolean, Append)
          DO_FAIL("boolean")
          break;
        case 'B':
          DO_FIRST_ARG("Ljava/lang/Byte;", GetByte, Append)
          DO_FAIL("byte")
          break;
        case 'C':
          DO_FIRST_ARG("Ljava/lang/Character;", GetChar, Append)
          DO_FAIL("char")
          break;
        case 'S':
          DO_FIRST_ARG("Ljava/lang/Short;", GetShort, Append)
          DO_ARG("Ljava/lang/Byte;", GetByte, Append)
          DO_FAIL("short")
          break;
        case 'I':
          DO_FIRST_ARG("Ljava/lang/Integer;", GetInt, Append)
          DO_ARG("Ljava/lang/Character;", GetChar, Append)
          DO_ARG("Ljava/lang/Short;", GetShort, Append)
          DO_ARG("Ljava/lang/Byte;", GetByte, Append)
          DO_FAIL("int")
          break;
        case 'J':
          DO_FIRST_ARG("Ljava/lang/Long;", GetLong, AppendWide)
          DO_ARG("Ljava/lang/Integer;", GetInt, AppendWide)
          DO_ARG("Ljava/lang/Character;", GetChar, AppendWide)
          DO_ARG("Ljava/lang/Short;", GetShort, AppendWide)
          DO_ARG("Ljava/lang/Byte;", GetByte, AppendWide)
          DO_FAIL("long")
          break;
        case 'F':
          DO_FIRST_ARG("Ljava/lang/Float;", GetFloat, AppendFloat)
          DO_ARG("Ljava/lang/Long;", GetLong, AppendFloat)
          DO_ARG("Ljava/lang/Integer;", GetInt, AppendFloat)
          DO_ARG("Ljava/lang/Character;", GetChar, AppendFloat)
          DO_ARG("Ljava/lang/Short;", GetShort, AppendFloat)
          DO_ARG("Ljava/lang/Byte;", GetByte, AppendFloat)
          DO_FAIL("float")
          break;
        case 'D':
          DO_FIRST_ARG("Ljava/lang/Double;", GetDouble, AppendDouble)
          DO_ARG("Ljava/lang/Float;", GetFloat, AppendDouble)
          DO_ARG("Ljava/lang/Long;", GetLong, AppendDouble)
          DO_ARG("Ljava/lang/Integer;", GetInt, AppendDouble)
          DO_ARG("Ljava/lang/Character;", GetChar, AppendDouble)
          DO_ARG("Ljava/lang/Short;", GetShort, AppendDouble)
          DO_ARG("Ljava/lang/Byte;", GetByte, AppendDouble)
          DO_FAIL("double")
          break;
#ifndef NDEBUG
        default:
          LOG(FATAL) << "Unexpected shorty character: " << shorty_[i];
          UNREACHABLE();
#endif
      }
#undef DO_FIRST_ARG
#undef DO_ARG
#undef DO_FAIL
    }
    return true;
  }

 private:
  enum { kSmallArgArraySize = 16 };
  const char* const shorty_;
  const uint32_t shorty_len_;
  uint32_t num_bytes_;
  uint32_t* arg_array_;
  uint32_t small_arg_array_[kSmallArgArraySize];
  std::unique_ptr<uint32_t[]> large_arg_array_;
};

static void CheckMethodArguments(JavaVMExt* vm, ArtMethod* m, uint32_t* args)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const DexFile::TypeList* params = m->GetParameterTypeList();
  if (params == nullptr) {
    return;  // No arguments so nothing to check.
  }
  uint32_t offset = 0;
  uint32_t num_params = params->Size();
  size_t error_count = 0;
  if (!m->IsStatic()) {
    offset = 1;
  }
  // TODO: If args contain object references, it may cause problems.
  Thread* const self = Thread::Current();
  size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  for (uint32_t i = 0; i < num_params; i++) {
    uint16_t type_idx = params->GetTypeItem(i).type_idx_;
    mirror::Class* param_type = m->GetClassFromTypeIndex(type_idx,
                                                         true /* resolve*/,
                                                         pointer_size);
    if (param_type == nullptr) {
      CHECK(self->IsExceptionPending());
      LOG(ERROR) << "Internal error: unresolvable type for argument type in JNI invoke: "
          << m->GetTypeDescriptorFromTypeIdx(type_idx) << "\n"
          << self->GetException()->Dump();
      self->ClearException();
      ++error_count;
    } else if (!param_type->IsPrimitive()) {
      // TODO: There is a compaction bug here since GetClassFromTypeIdx can cause thread suspension,
      // this is a hard to fix problem since the args can contain Object*, we need to save and
      // restore them by using a visitor similar to the ones used in the trampoline entrypoints.
      mirror::Object* argument =
          (reinterpret_cast<StackReference<mirror::Object>*>(&args[i + offset]))->AsMirrorPtr();
      if (argument != nullptr && !argument->InstanceOf(param_type)) {
        LOG(ERROR) << "JNI ERROR (app bug): attempt to pass an instance of "
                   << PrettyTypeOf(argument) << " as argument " << (i + 1)
                   << " to " << PrettyMethod(m);
        ++error_count;
      }
    } else if (param_type->IsPrimitiveLong() || param_type->IsPrimitiveDouble()) {
      offset++;
    } else {
      int32_t arg = static_cast<int32_t>(args[i + offset]);
      if (param_type->IsPrimitiveBoolean()) {
        if (arg != JNI_TRUE && arg != JNI_FALSE) {
          LOG(ERROR) << "JNI ERROR (app bug): expected jboolean (0/1) but got value of "
              << arg << " as argument " << (i + 1) << " to " << PrettyMethod(m);
          ++error_count;
        }
      } else if (param_type->IsPrimitiveByte()) {
        if (arg < -128 || arg > 127) {
          LOG(ERROR) << "JNI ERROR (app bug): expected jbyte but got value of "
              << arg << " as argument " << (i + 1) << " to " << PrettyMethod(m);
          ++error_count;
        }
      } else if (param_type->IsPrimitiveChar()) {
        if (args[i + offset] > 0xFFFF) {
          LOG(ERROR) << "JNI ERROR (app bug): expected jchar but got value of "
              << arg << " as argument " << (i + 1) << " to " << PrettyMethod(m);
          ++error_count;
        }
      } else if (param_type->IsPrimitiveShort()) {
        if (arg < -32768 || arg > 0x7FFF) {
          LOG(ERROR) << "JNI ERROR (app bug): expected jshort but got value of "
              << arg << " as argument " << (i + 1) << " to " << PrettyMethod(m);
          ++error_count;
        }
      }
    }
  }
  if (UNLIKELY(error_count > 0)) {
    // TODO: pass the JNI function name (such as "CallVoidMethodV") through so we can call JniAbort
    // with an argument.
    vm->JniAbortF(nullptr, "bad arguments passed to %s (see above for details)",
                  PrettyMethod(m).c_str());
  }
}

static ArtMethod* FindVirtualMethod(mirror::Object* receiver, ArtMethod* method)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(method, sizeof(void*));
}


static void InvokeWithArgArray(const ScopedObjectAccessAlreadyRunnable& soa,
                               ArtMethod* method, ArgArray* arg_array, JValue* result,
                               const char* shorty)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  uint32_t* args = arg_array->GetArray();
  if (UNLIKELY(soa.Env()->check_jni)) {
    CheckMethodArguments(soa.Vm(), method->GetInterfaceMethodIfProxy(sizeof(void*)), args);
  }
  method->Invoke(soa.Self(), args, arg_array->GetNumBytes(), result, shorty);
}

JValue InvokeWithVarArgs(const ScopedObjectAccessAlreadyRunnable& soa, jobject obj, jmethodID mid,
                         va_list args)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // We want to make sure that the stack is not within a small distance from the
  // protected region in case we are calling into a leaf function whose stack
  // check has been elided.
  if (UNLIKELY(__builtin_frame_address(0) < soa.Self()->GetStackEnd())) {
    ThrowStackOverflowError(soa.Self());
    return JValue();
  }

  ArtMethod* method = soa.DecodeMethod(mid);
  bool is_string_init = method->GetDeclaringClass()->IsStringClass() && method->IsConstructor();
  if (is_string_init) {
    // Replace calls to String.<init> with equivalent StringFactory call.
    method = soa.DecodeMethod(WellKnownClasses::StringInitToStringFactoryMethodID(mid));
  }
  mirror::Object* receiver = method->IsStatic() ? nullptr : soa.Decode<mirror::Object*>(obj);
  uint32_t shorty_len = 0;
  const char* shorty = method->GetInterfaceMethodIfProxy(sizeof(void*))->GetShorty(&shorty_len);
  JValue result;
  ArgArray arg_array(shorty, shorty_len);
  arg_array.BuildArgArrayFromVarArgs(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, shorty);
  if (is_string_init) {
    // For string init, remap original receiver to StringFactory result.
    UpdateReference(soa.Self(), obj, result.GetL());
  }
  return result;
}

JValue InvokeWithJValues(const ScopedObjectAccessAlreadyRunnable& soa, jobject obj, jmethodID mid,
                         jvalue* args) {
  // We want to make sure that the stack is not within a small distance from the
  // protected region in case we are calling into a leaf function whose stack
  // check has been elided.
  if (UNLIKELY(__builtin_frame_address(0) < soa.Self()->GetStackEnd())) {
    ThrowStackOverflowError(soa.Self());
    return JValue();
  }

  ArtMethod* method = soa.DecodeMethod(mid);
  bool is_string_init = method->GetDeclaringClass()->IsStringClass() && method->IsConstructor();
  if (is_string_init) {
    // Replace calls to String.<init> with equivalent StringFactory call.
    method = soa.DecodeMethod(WellKnownClasses::StringInitToStringFactoryMethodID(mid));
  }
  mirror::Object* receiver = method->IsStatic() ? nullptr : soa.Decode<mirror::Object*>(obj);
  uint32_t shorty_len = 0;
  const char* shorty = method->GetInterfaceMethodIfProxy(sizeof(void*))->GetShorty(&shorty_len);
  JValue result;
  ArgArray arg_array(shorty, shorty_len);
  arg_array.BuildArgArrayFromJValues(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, shorty);
  if (is_string_init) {
    // For string init, remap original receiver to StringFactory result.
    UpdateReference(soa.Self(), obj, result.GetL());
  }
  return result;
}

JValue InvokeVirtualOrInterfaceWithJValues(const ScopedObjectAccessAlreadyRunnable& soa,
                                           jobject obj, jmethodID mid, jvalue* args) {
  // We want to make sure that the stack is not within a small distance from the
  // protected region in case we are calling into a leaf function whose stack
  // check has been elided.
  if (UNLIKELY(__builtin_frame_address(0) < soa.Self()->GetStackEnd())) {
    ThrowStackOverflowError(soa.Self());
    return JValue();
  }

  mirror::Object* receiver = soa.Decode<mirror::Object*>(obj);
  ArtMethod* method = FindVirtualMethod(receiver, soa.DecodeMethod(mid));
  bool is_string_init = method->GetDeclaringClass()->IsStringClass() && method->IsConstructor();
  if (is_string_init) {
    // Replace calls to String.<init> with equivalent StringFactory call.
    method = soa.DecodeMethod(WellKnownClasses::StringInitToStringFactoryMethodID(mid));
    receiver = nullptr;
  }
  uint32_t shorty_len = 0;
  const char* shorty = method->GetInterfaceMethodIfProxy(sizeof(void*))->GetShorty(&shorty_len);
  JValue result;
  ArgArray arg_array(shorty, shorty_len);
  arg_array.BuildArgArrayFromJValues(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, shorty);
  if (is_string_init) {
    // For string init, remap original receiver to StringFactory result.
    UpdateReference(soa.Self(), obj, result.GetL());
  }
  return result;
}

JValue InvokeVirtualOrInterfaceWithVarArgs(const ScopedObjectAccessAlreadyRunnable& soa,
                                           jobject obj, jmethodID mid, va_list args) {
  // We want to make sure that the stack is not within a small distance from the
  // protected region in case we are calling into a leaf function whose stack
  // check has been elided.
  if (UNLIKELY(__builtin_frame_address(0) < soa.Self()->GetStackEnd())) {
    ThrowStackOverflowError(soa.Self());
    return JValue();
  }

  mirror::Object* receiver = soa.Decode<mirror::Object*>(obj);
  ArtMethod* method = FindVirtualMethod(receiver, soa.DecodeMethod(mid));
  bool is_string_init = method->GetDeclaringClass()->IsStringClass() && method->IsConstructor();
  if (is_string_init) {
    // Replace calls to String.<init> with equivalent StringFactory call.
    method = soa.DecodeMethod(WellKnownClasses::StringInitToStringFactoryMethodID(mid));
    receiver = nullptr;
  }
  uint32_t shorty_len = 0;
  const char* shorty = method->GetInterfaceMethodIfProxy(sizeof(void*))->GetShorty(&shorty_len);
  JValue result;
  ArgArray arg_array(shorty, shorty_len);
  arg_array.BuildArgArrayFromVarArgs(soa, receiver, args);
  InvokeWithArgArray(soa, method, &arg_array, &result, shorty);
  if (is_string_init) {
    // For string init, remap original receiver to StringFactory result.
    UpdateReference(soa.Self(), obj, result.GetL());
  }
  return result;
}

jobject InvokeMethod(const ScopedObjectAccessAlreadyRunnable& soa, jobject javaMethod,
                     jobject javaReceiver, jobject javaArgs, size_t num_frames) {
  // We want to make sure that the stack is not within a small distance from the
  // protected region in case we are calling into a leaf function whose stack
  // check has been elided.
  if (UNLIKELY(__builtin_frame_address(0) <
               soa.Self()->GetStackEndForInterpreter(true))) {
    ThrowStackOverflowError(soa.Self());
    return nullptr;
  }

  auto* abstract_method = soa.Decode<mirror::AbstractMethod*>(javaMethod);
  const bool accessible = abstract_method->IsAccessible();
  ArtMethod* m = abstract_method->GetArtMethod();

  mirror::Class* declaring_class = m->GetDeclaringClass();
  if (UNLIKELY(!declaring_class->IsInitialized())) {
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> h_class(hs.NewHandle(declaring_class));
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(soa.Self(), h_class, true, true)) {
      return nullptr;
    }
    declaring_class = h_class.Get();
  }

  mirror::Object* receiver = nullptr;
  if (!m->IsStatic()) {
    // Replace calls to String.<init> with equivalent StringFactory call.
    if (declaring_class->IsStringClass() && m->IsConstructor()) {
      jmethodID mid = soa.EncodeMethod(m);
      m = soa.DecodeMethod(WellKnownClasses::StringInitToStringFactoryMethodID(mid));
      CHECK(javaReceiver == nullptr);
    } else {
      // Check that the receiver is non-null and an instance of the field's declaring class.
      receiver = soa.Decode<mirror::Object*>(javaReceiver);
      if (!VerifyObjectIsClass(receiver, declaring_class)) {
        return nullptr;
      }

      // Find the actual implementation of the virtual method.
      m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(m, sizeof(void*));
    }
  }

  // Get our arrays of arguments and their types, and check they're the same size.
  auto* objects = soa.Decode<mirror::ObjectArray<mirror::Object>*>(javaArgs);
  auto* np_method = m->GetInterfaceMethodIfProxy(sizeof(void*));
  const DexFile::TypeList* classes = np_method->GetParameterTypeList();
  uint32_t classes_size = (classes == nullptr) ? 0 : classes->Size();
  uint32_t arg_count = (objects != nullptr) ? objects->GetLength() : 0;
  if (arg_count != classes_size) {
    ThrowIllegalArgumentException(StringPrintf("Wrong number of arguments; expected %d, got %d",
                                               classes_size, arg_count).c_str());
    return nullptr;
  }

  // If method is not set to be accessible, verify it can be accessed by the caller.
  mirror::Class* calling_class = nullptr;
  if (!accessible && !VerifyAccess(soa.Self(), receiver, declaring_class, m->GetAccessFlags(),
                                   &calling_class, num_frames)) {
    ThrowIllegalAccessException(
        StringPrintf("Class %s cannot access %s method %s of class %s",
            calling_class == nullptr ? "null" : PrettyClass(calling_class).c_str(),
            PrettyJavaAccessFlags(m->GetAccessFlags()).c_str(),
            PrettyMethod(m).c_str(),
            m->GetDeclaringClass() == nullptr ? "null" :
                PrettyClass(m->GetDeclaringClass()).c_str()).c_str());
    return nullptr;
  }

  // Invoke the method.
  JValue result;
  uint32_t shorty_len = 0;
  const char* shorty = np_method->GetShorty(&shorty_len);
  ArgArray arg_array(shorty, shorty_len);
  if (!arg_array.BuildArgArrayFromObjectArray(receiver, objects, np_method)) {
    CHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }

  InvokeWithArgArray(soa, m, &arg_array, &result, shorty);

  // Wrap any exception with "Ljava/lang/reflect/InvocationTargetException;" and return early.
  if (soa.Self()->IsExceptionPending()) {
    // If we get another exception when we are trying to wrap, then just use that instead.
    jthrowable th = soa.Env()->ExceptionOccurred();
    soa.Self()->ClearException();
    jclass exception_class = soa.Env()->FindClass("java/lang/reflect/InvocationTargetException");
    if (exception_class == nullptr) {
      soa.Self()->AssertPendingOOMException();
      return nullptr;
    }
    jmethodID mid = soa.Env()->GetMethodID(exception_class, "<init>", "(Ljava/lang/Throwable;)V");
    CHECK(mid != nullptr);
    jobject exception_instance = soa.Env()->NewObject(exception_class, mid, th);
    if (exception_instance == nullptr) {
      soa.Self()->AssertPendingOOMException();
      return nullptr;
    }
    soa.Env()->Throw(reinterpret_cast<jthrowable>(exception_instance));
    return nullptr;
  }

  // Box if necessary and return.
  return soa.AddLocalReference<jobject>(BoxPrimitive(Primitive::GetType(shorty[0]), result));
}

mirror::Object* BoxPrimitive(Primitive::Type src_class, const JValue& value) {
  if (src_class == Primitive::kPrimNot) {
    return value.GetL();
  }
  if (src_class == Primitive::kPrimVoid) {
    // There's no such thing as a void field, and void methods invoked via reflection return null.
    return nullptr;
  }

  jmethodID m = nullptr;
  const char* shorty;
  switch (src_class) {
  case Primitive::kPrimBoolean:
    m = WellKnownClasses::java_lang_Boolean_valueOf;
    shorty = "LZ";
    break;
  case Primitive::kPrimByte:
    m = WellKnownClasses::java_lang_Byte_valueOf;
    shorty = "LB";
    break;
  case Primitive::kPrimChar:
    m = WellKnownClasses::java_lang_Character_valueOf;
    shorty = "LC";
    break;
  case Primitive::kPrimDouble:
    m = WellKnownClasses::java_lang_Double_valueOf;
    shorty = "LD";
    break;
  case Primitive::kPrimFloat:
    m = WellKnownClasses::java_lang_Float_valueOf;
    shorty = "LF";
    break;
  case Primitive::kPrimInt:
    m = WellKnownClasses::java_lang_Integer_valueOf;
    shorty = "LI";
    break;
  case Primitive::kPrimLong:
    m = WellKnownClasses::java_lang_Long_valueOf;
    shorty = "LJ";
    break;
  case Primitive::kPrimShort:
    m = WellKnownClasses::java_lang_Short_valueOf;
    shorty = "LS";
    break;
  default:
    LOG(FATAL) << static_cast<int>(src_class);
    shorty = nullptr;
  }

  ScopedObjectAccessUnchecked soa(Thread::Current());
  DCHECK_EQ(soa.Self()->GetState(), kRunnable);

  ArgArray arg_array(shorty, 2);
  JValue result;
  if (src_class == Primitive::kPrimDouble || src_class == Primitive::kPrimLong) {
    arg_array.AppendWide(value.GetJ());
  } else {
    arg_array.Append(value.GetI());
  }

  soa.DecodeMethod(m)->Invoke(soa.Self(), arg_array.GetArray(), arg_array.GetNumBytes(),
                              &result, shorty);
  return result.GetL();
}

static std::string UnboxingFailureKind(ArtField* f)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (f != nullptr) {
    return "field " + PrettyField(f, false);
  }
  return "result";
}

static bool UnboxPrimitive(mirror::Object* o,
                           mirror::Class* dst_class, ArtField* f,
                           JValue* unboxed_value)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  bool unbox_for_result = (f == nullptr);
  if (!dst_class->IsPrimitive()) {
    if (UNLIKELY(o != nullptr && !o->InstanceOf(dst_class))) {
      if (!unbox_for_result) {
        ThrowIllegalArgumentException(StringPrintf("%s has type %s, got %s",
                                                   UnboxingFailureKind(f).c_str(),
                                                   PrettyDescriptor(dst_class).c_str(),
                                                   PrettyTypeOf(o).c_str()).c_str());
      } else {
        ThrowClassCastException(StringPrintf("Couldn't convert result of type %s to %s",
                                             PrettyTypeOf(o).c_str(),
                                             PrettyDescriptor(dst_class).c_str()).c_str());
      }
      return false;
    }
    unboxed_value->SetL(o);
    return true;
  }
  if (UNLIKELY(dst_class->GetPrimitiveType() == Primitive::kPrimVoid)) {
    ThrowIllegalArgumentException(StringPrintf("Can't unbox %s to void",
                                               UnboxingFailureKind(f).c_str()).c_str());
    return false;
  }
  if (UNLIKELY(o == nullptr)) {
    if (!unbox_for_result) {
      ThrowIllegalArgumentException(StringPrintf("%s has type %s, got null",
                                                 UnboxingFailureKind(f).c_str(),
                                                 PrettyDescriptor(dst_class).c_str()).c_str());
    } else {
      ThrowNullPointerException(StringPrintf("Expected to unbox a '%s' primitive type but was returned null",
                                             PrettyDescriptor(dst_class).c_str()).c_str());
    }
    return false;
  }

  JValue boxed_value;
  mirror::Class* klass = o->GetClass();
  mirror::Class* src_class = nullptr;
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  ArtField* primitive_field = &klass->GetIFieldsPtr()->At(0);
  if (klass->DescriptorEquals("Ljava/lang/Boolean;")) {
    src_class = class_linker->FindPrimitiveClass('Z');
    boxed_value.SetZ(primitive_field->GetBoolean(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Byte;")) {
    src_class = class_linker->FindPrimitiveClass('B');
    boxed_value.SetB(primitive_field->GetByte(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Character;")) {
    src_class = class_linker->FindPrimitiveClass('C');
    boxed_value.SetC(primitive_field->GetChar(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Float;")) {
    src_class = class_linker->FindPrimitiveClass('F');
    boxed_value.SetF(primitive_field->GetFloat(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Double;")) {
    src_class = class_linker->FindPrimitiveClass('D');
    boxed_value.SetD(primitive_field->GetDouble(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Integer;")) {
    src_class = class_linker->FindPrimitiveClass('I');
    boxed_value.SetI(primitive_field->GetInt(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Long;")) {
    src_class = class_linker->FindPrimitiveClass('J');
    boxed_value.SetJ(primitive_field->GetLong(o));
  } else if (klass->DescriptorEquals("Ljava/lang/Short;")) {
    src_class = class_linker->FindPrimitiveClass('S');
    boxed_value.SetS(primitive_field->GetShort(o));
  } else {
    std::string temp;
    ThrowIllegalArgumentException(
        StringPrintf("%s has type %s, got %s", UnboxingFailureKind(f).c_str(),
            PrettyDescriptor(dst_class).c_str(),
            PrettyDescriptor(o->GetClass()->GetDescriptor(&temp)).c_str()).c_str());
    return false;
  }

  return ConvertPrimitiveValue(unbox_for_result,
                               src_class->GetPrimitiveType(), dst_class->GetPrimitiveType(),
                               boxed_value, unboxed_value);
}

bool UnboxPrimitiveForField(mirror::Object* o, mirror::Class* dst_class, ArtField* f,
                            JValue* unboxed_value) {
  DCHECK(f != nullptr);
  return UnboxPrimitive(o, dst_class, f, unboxed_value);
}

bool UnboxPrimitiveForResult(mirror::Object* o, mirror::Class* dst_class, JValue* unboxed_value) {
  return UnboxPrimitive(o, dst_class, nullptr, unboxed_value);
}

mirror::Class* GetCallingClass(Thread* self, size_t num_frames) {
  NthCallerVisitor visitor(self, num_frames);
  visitor.WalkStack();
  return visitor.caller != nullptr ? visitor.caller->GetDeclaringClass() : nullptr;
}

bool VerifyAccess(Thread* self, mirror::Object* obj, mirror::Class* declaring_class,
                  uint32_t access_flags, mirror::Class** calling_class, size_t num_frames) {
  if ((access_flags & kAccPublic) != 0) {
    return true;
  }
  auto* klass = GetCallingClass(self, num_frames);
  if (UNLIKELY(klass == nullptr)) {
    // The caller is an attached native thread.
    return false;
  }
  *calling_class = klass;
  return VerifyAccess(self, obj, declaring_class, access_flags, klass);
}

bool VerifyAccess(Thread* self, mirror::Object* obj, mirror::Class* declaring_class,
                  uint32_t access_flags, mirror::Class* calling_class) {
  if (calling_class == declaring_class) {
    return true;
  }
  ScopedAssertNoThreadSuspension sants(self, "verify-access");
  if ((access_flags & kAccPrivate) != 0) {
    return false;
  }
  if ((access_flags & kAccProtected) != 0) {
    if (obj != nullptr && !obj->InstanceOf(calling_class) &&
        !declaring_class->IsInSamePackage(calling_class)) {
      return false;
    } else if (declaring_class->IsAssignableFrom(calling_class)) {
      return true;
    }
  }
  return declaring_class->IsInSamePackage(calling_class);
}

void InvalidReceiverError(mirror::Object* o, mirror::Class* c) {
  std::string expected_class_name(PrettyDescriptor(c));
  std::string actual_class_name(PrettyTypeOf(o));
  ThrowIllegalArgumentException(StringPrintf("Expected receiver of type %s, but got %s",
                                             expected_class_name.c_str(),
                                             actual_class_name.c_str()).c_str());
}

// This only works if there's one reference which points to the object in obj.
// Will need to be fixed if there's cases where it's not.
void UpdateReference(Thread* self, jobject obj, mirror::Object* result) {
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = GetIndirectRefKind(ref);
  if (kind == kLocal) {
    self->GetJniEnv()->locals.Update(obj, result);
  } else if (kind == kHandleScopeOrInvalid) {
    LOG(FATAL) << "Unsupported UpdateReference for kind kHandleScopeOrInvalid";
  } else if (kind == kGlobal) {
    self->GetJniEnv()->vm->UpdateGlobal(self, ref, result);
  } else {
    DCHECK_EQ(kind, kWeakGlobal);
    self->GetJniEnv()->vm->UpdateWeakGlobal(self, ref, result);
  }
}

}  // namespace art
