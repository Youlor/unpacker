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

#include "unstarted_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include <cmath>
#include <limits>
#include <locale>
#include <unordered_map>

#include "ScopedLocalRef.h"

#include "art_method-inl.h"
#include "base/casts.h"
#include "base/logging.h"
#include "base/macros.h"
#include "class_linker.h"
#include "common_throws.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/reference_processor.h"
#include "handle_scope-inl.h"
#include "interpreter/interpreter_common.h"
#include "mirror/array-inl.h"
#include "mirror/class.h"
#include "mirror/field-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "nth_caller_visitor.h"
#include "reflection.h"
#include "thread.h"
#include "transaction.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {
namespace interpreter {

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    SHARED_REQUIRES(Locks::mutator_lock_);

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...) {
  va_list args;
  if (Runtime::Current()->IsActiveTransaction()) {
    va_start(args, fmt);
    AbortTransactionV(self, fmt, args);
    va_end(args);
  } else {
    va_start(args, fmt);
    std::string msg;
    StringAppendV(&msg, fmt, args);
    va_end(args);
    LOG(FATAL) << "Trying to abort, but not in transaction mode: " << msg;
    UNREACHABLE();
  }
}

// Restricted support for character upper case / lower case. Only support ASCII, where
// it's easy. Abort the transaction otherwise.
static void CharacterLowerUpper(Thread* self,
                                ShadowFrame* shadow_frame,
                                JValue* result,
                                size_t arg_offset,
                                bool to_lower_case) SHARED_REQUIRES(Locks::mutator_lock_) {
  uint32_t int_value = static_cast<uint32_t>(shadow_frame->GetVReg(arg_offset));

  // Only ASCII (7-bit).
  if (!isascii(int_value)) {
    AbortTransactionOrFail(self,
                           "Only support ASCII characters for toLowerCase/toUpperCase: %u",
                           int_value);
    return;
  }

  std::locale c_locale("C");
  char char_value = static_cast<char>(int_value);

  if (to_lower_case) {
    result->SetI(std::tolower(char_value, c_locale));
  } else {
    result->SetI(std::toupper(char_value, c_locale));
  }
}

void UnstartedRuntime::UnstartedCharacterToLowerCase(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  CharacterLowerUpper(self, shadow_frame, result, arg_offset, true);
}

void UnstartedRuntime::UnstartedCharacterToUpperCase(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  CharacterLowerUpper(self, shadow_frame, result, arg_offset, false);
}

// Helper function to deal with class loading in an unstarted runtime.
static void UnstartedRuntimeFindClass(Thread* self, Handle<mirror::String> className,
                                      Handle<mirror::ClassLoader> class_loader, JValue* result,
                                      const std::string& method_name, bool initialize_class,
                                      bool abort_if_not_found)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  CHECK(className.Get() != nullptr);
  std::string descriptor(DotToDescriptor(className->ToModifiedUtf8().c_str()));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  mirror::Class* found = class_linker->FindClass(self, descriptor.c_str(), class_loader);
  if (found == nullptr && abort_if_not_found) {
    if (!self->IsExceptionPending()) {
      AbortTransactionOrFail(self, "%s failed in un-started runtime for class: %s",
                             method_name.c_str(), PrettyDescriptor(descriptor.c_str()).c_str());
    }
    return;
  }
  if (found != nullptr && initialize_class) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(found));
    if (!class_linker->EnsureInitialized(self, h_class, true, true)) {
      CHECK(self->IsExceptionPending());
      return;
    }
  }
  result->SetL(found);
}

// Common helper for class-loading cutouts in an unstarted runtime. We call Runtime methods that
// rely on Java code to wrap errors in the correct exception class (i.e., NoClassDefFoundError into
// ClassNotFoundException), so need to do the same. The only exception is if the exception is
// actually the transaction abort exception. This must not be wrapped, as it signals an
// initialization abort.
static void CheckExceptionGenerateClassNotFound(Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (self->IsExceptionPending()) {
    // If it is not the transaction abort exception, wrap it.
    std::string type(PrettyTypeOf(self->GetException()));
    if (type != Transaction::kAbortExceptionDescriptor) {
      self->ThrowNewWrappedException("Ljava/lang/ClassNotFoundException;",
                                     "ClassNotFoundException");
    }
  }
}

static mirror::String* GetClassName(Thread* self, ShadowFrame* shadow_frame, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Object* param = shadow_frame->GetVRegReference(arg_offset);
  if (param == nullptr) {
    AbortTransactionOrFail(self, "Null-pointer in Class.forName.");
    return nullptr;
  }
  return param->AsString();
}

void UnstartedRuntime::UnstartedClassForName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::String* class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  UnstartedRuntimeFindClass(self,
                            h_class_name,
                            ScopedNullHandle<mirror::ClassLoader>(),
                            result,
                            "Class.forName",
                            true,
                            false);
  CheckExceptionGenerateClassNotFound(self);
}

void UnstartedRuntime::UnstartedClassForNameLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::String* class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  bool initialize_class = shadow_frame->GetVReg(arg_offset + 1) != 0;
  mirror::ClassLoader* class_loader =
      down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset + 2));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result, "Class.forName",
                            initialize_class, false);
  CheckExceptionGenerateClassNotFound(self);
}

void UnstartedRuntime::UnstartedClassClassForName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::String* class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  bool initialize_class = shadow_frame->GetVReg(arg_offset + 1) != 0;
  mirror::ClassLoader* class_loader =
      down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset + 2));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result, "Class.classForName",
                            initialize_class, false);
  CheckExceptionGenerateClassNotFound(self);
}

void UnstartedRuntime::UnstartedClassNewInstance(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<2> hs(self);  // Class, constructor, object.
  mirror::Object* param = shadow_frame->GetVRegReference(arg_offset);
  if (param == nullptr) {
    AbortTransactionOrFail(self, "Null-pointer in Class.newInstance.");
    return;
  }
  mirror::Class* klass = param->AsClass();
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));

  // Check that it's not null.
  if (h_klass.Get() == nullptr) {
    AbortTransactionOrFail(self, "Class reference is null for newInstance");
    return;
  }

  // If we're in a transaction, class must not be finalizable (it or a superclass has a finalizer).
  if (Runtime::Current()->IsActiveTransaction()) {
    if (h_klass.Get()->IsFinalizable()) {
      AbortTransactionF(self, "Class for newInstance is finalizable: '%s'",
                        PrettyClass(h_klass.Get()).c_str());
      return;
    }
  }

  // There are two situations in which we'll abort this run.
  //  1) If the class isn't yet initialized and initialization fails.
  //  2) If we can't find the default constructor. We'll postpone the exception to runtime.
  // Note that 2) could likely be handled here, but for safety abort the transaction.
  bool ok = false;
  auto* cl = Runtime::Current()->GetClassLinker();
  if (cl->EnsureInitialized(self, h_klass, true, true)) {
    auto* cons = h_klass->FindDeclaredDirectMethod("<init>", "()V", cl->GetImagePointerSize());
    if (cons != nullptr) {
      Handle<mirror::Object> h_obj(hs.NewHandle(klass->AllocObject(self)));
      CHECK(h_obj.Get() != nullptr);  // We don't expect OOM at compile-time.
      EnterInterpreterFromInvoke(self, cons, h_obj.Get(), nullptr, nullptr);
      if (!self->IsExceptionPending()) {
        result->SetL(h_obj.Get());
        ok = true;
      }
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Could not find default constructor for '%s'",
                               PrettyClass(h_klass.Get()).c_str());
    }
  }
  if (!ok) {
    AbortTransactionOrFail(self, "Failed in Class.newInstance for '%s' with %s",
                           PrettyClass(h_klass.Get()).c_str(),
                           PrettyTypeOf(self->GetException()).c_str());
  }
}

void UnstartedRuntime::UnstartedClassGetDeclaredField(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
  // going the reflective Dex way.
  mirror::Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  mirror::String* name2 = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  ArtField* found = nullptr;
  for (ArtField& field : klass->GetIFields()) {
    if (name2->Equals(field.GetName())) {
      found = &field;
      break;
    }
  }
  if (found == nullptr) {
    for (ArtField& field : klass->GetSFields()) {
      if (name2->Equals(field.GetName())) {
        found = &field;
        break;
      }
    }
  }
  if (found == nullptr) {
    AbortTransactionOrFail(self, "Failed to find field in Class.getDeclaredField in un-started "
                           " runtime. name=%s class=%s", name2->ToModifiedUtf8().c_str(),
                           PrettyDescriptor(klass).c_str());
    return;
  }
  if (Runtime::Current()->IsActiveTransaction()) {
    result->SetL(mirror::Field::CreateFromArtField<true>(self, found, true));
  } else {
    result->SetL(mirror::Field::CreateFromArtField<false>(self, found, true));
  }
}

// This is required for Enum(Set) code, as that uses reflection to inspect enum classes.
void UnstartedRuntime::UnstartedClassGetDeclaredMethod(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Special managed code cut-out to allow method lookup in a un-started runtime.
  mirror::Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  if (klass == nullptr) {
    ThrowNullPointerExceptionForMethodAccess(shadow_frame->GetMethod(), InvokeType::kVirtual);
    return;
  }
  mirror::String* name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  mirror::ObjectArray<mirror::Class>* args =
      shadow_frame->GetVRegReference(arg_offset + 2)->AsObjectArray<mirror::Class>();
  if (Runtime::Current()->IsActiveTransaction()) {
    result->SetL(mirror::Class::GetDeclaredMethodInternal<true>(self, klass, name, args));
  } else {
    result->SetL(mirror::Class::GetDeclaredMethodInternal<false>(self, klass, name, args));
  }
}

// Special managed code cut-out to allow constructor lookup in a un-started runtime.
void UnstartedRuntime::UnstartedClassGetDeclaredConstructor(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  if (klass == nullptr) {
    ThrowNullPointerExceptionForMethodAccess(shadow_frame->GetMethod(), InvokeType::kVirtual);
    return;
  }
  mirror::ObjectArray<mirror::Class>* args =
      shadow_frame->GetVRegReference(arg_offset + 1)->AsObjectArray<mirror::Class>();
  if (Runtime::Current()->IsActiveTransaction()) {
    result->SetL(mirror::Class::GetDeclaredConstructorInternal<true>(self, klass, args));
  } else {
    result->SetL(mirror::Class::GetDeclaredConstructorInternal<false>(self, klass, args));
  }
}

void UnstartedRuntime::UnstartedClassGetEnclosingClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsClass()));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    result->SetL(nullptr);
  }
  result->SetL(klass->GetDexFile().GetEnclosingClass(klass));
}

void UnstartedRuntime::UnstartedClassGetInnerClassFlags(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(
      reinterpret_cast<mirror::Class*>(shadow_frame->GetVRegReference(arg_offset))));
  const int32_t default_value = shadow_frame->GetVReg(arg_offset + 1);
  result->SetI(mirror::Class::GetInnerClassFlags(klass, default_value));
}

static std::unique_ptr<MemMap> FindAndExtractEntry(const std::string& jar_file,
                                                   const char* entry_name,
                                                   size_t* size,
                                                   std::string* error_msg) {
  CHECK(size != nullptr);

  std::unique_ptr<ZipArchive> zip_archive(ZipArchive::Open(jar_file.c_str(), error_msg));
  if (zip_archive == nullptr) {
    return nullptr;;
  }
  std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find(entry_name, error_msg));
  if (zip_entry == nullptr) {
    return nullptr;
  }
  std::unique_ptr<MemMap> tmp_map(
      zip_entry->ExtractToMemMap(jar_file.c_str(), entry_name, error_msg));
  if (tmp_map == nullptr) {
    return nullptr;
  }

  // OK, from here everything seems fine.
  *size = zip_entry->GetUncompressedLength();
  return tmp_map;
}

static void GetResourceAsStream(Thread* self,
                                ShadowFrame* shadow_frame,
                                JValue* result,
                                size_t arg_offset) SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Object* resource_obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (resource_obj == nullptr) {
    AbortTransactionOrFail(self, "null name for getResourceAsStream");
    return;
  }
  CHECK(resource_obj->IsString());
  mirror::String* resource_name = resource_obj->AsString();

  std::string resource_name_str = resource_name->ToModifiedUtf8();
  if (resource_name_str.empty() || resource_name_str == "/") {
    AbortTransactionOrFail(self,
                           "Unsupported name %s for getResourceAsStream",
                           resource_name_str.c_str());
    return;
  }
  const char* resource_cstr = resource_name_str.c_str();
  if (resource_cstr[0] == '/') {
    resource_cstr++;
  }

  Runtime* runtime = Runtime::Current();

  std::vector<std::string> split;
  Split(runtime->GetBootClassPathString(), ':', &split);
  if (split.empty()) {
    AbortTransactionOrFail(self,
                           "Boot classpath not set or split error:: %s",
                           runtime->GetBootClassPathString().c_str());
    return;
  }

  std::unique_ptr<MemMap> mem_map;
  size_t map_size;
  std::string last_error_msg;  // Only store the last message (we could concatenate).

  for (const std::string& jar_file : split) {
    mem_map = FindAndExtractEntry(jar_file, resource_cstr, &map_size, &last_error_msg);
    if (mem_map != nullptr) {
      break;
    }
  }

  if (mem_map == nullptr) {
    // Didn't find it. There's a good chance this will be the same at runtime, but still
    // conservatively abort the transaction here.
    AbortTransactionOrFail(self,
                           "Could not find resource %s. Last error was %s.",
                           resource_name_str.c_str(),
                           last_error_msg.c_str());
    return;
  }

  StackHandleScope<3> hs(self);

  // Create byte array for content.
  Handle<mirror::ByteArray> h_array(hs.NewHandle(mirror::ByteArray::Alloc(self, map_size)));
  if (h_array.Get() == nullptr) {
    AbortTransactionOrFail(self, "Could not find/create byte array class");
    return;
  }
  // Copy in content.
  memcpy(h_array->GetData(), mem_map->Begin(), map_size);
  // Be proactive releasing memory.
  mem_map.release();

  // Create a ByteArrayInputStream.
  Handle<mirror::Class> h_class(hs.NewHandle(
      runtime->GetClassLinker()->FindClass(self,
                                           "Ljava/io/ByteArrayInputStream;",
                                           ScopedNullHandle<mirror::ClassLoader>())));
  if (h_class.Get() == nullptr) {
    AbortTransactionOrFail(self, "Could not find ByteArrayInputStream class");
    return;
  }
  if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
    AbortTransactionOrFail(self, "Could not initialize ByteArrayInputStream class");
    return;
  }

  Handle<mirror::Object> h_obj(hs.NewHandle(h_class->AllocObject(self)));
  if (h_obj.Get() == nullptr) {
    AbortTransactionOrFail(self, "Could not allocate ByteArrayInputStream object");
    return;
  }

  auto* cl = Runtime::Current()->GetClassLinker();
  ArtMethod* constructor = h_class->FindDeclaredDirectMethod(
      "<init>", "([B)V", cl->GetImagePointerSize());
  if (constructor == nullptr) {
    AbortTransactionOrFail(self, "Could not find ByteArrayInputStream constructor");
    return;
  }

  uint32_t args[1];
  args[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h_array.Get()));
  EnterInterpreterFromInvoke(self, constructor, h_obj.Get(), args, nullptr);

  if (self->IsExceptionPending()) {
    AbortTransactionOrFail(self, "Could not run ByteArrayInputStream constructor");
    return;
  }

  result->SetL(h_obj.Get());
}

void UnstartedRuntime::UnstartedClassLoaderGetResourceAsStream(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  {
    mirror::Object* this_obj = shadow_frame->GetVRegReference(arg_offset);
    CHECK(this_obj != nullptr);
    CHECK(this_obj->IsClassLoader());

    StackHandleScope<1> hs(self);
    Handle<mirror::Class> this_classloader_class(hs.NewHandle(this_obj->GetClass()));

    if (self->DecodeJObject(WellKnownClasses::java_lang_BootClassLoader) !=
            this_classloader_class.Get()) {
      AbortTransactionOrFail(self,
                            "Unsupported classloader type %s for getResourceAsStream",
                            PrettyClass(this_classloader_class.Get()).c_str());
      return;
    }
  }

  GetResourceAsStream(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedVmClassLoaderFindLoadedClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  mirror::ClassLoader* class_loader =
      down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result,
                            "VMClassLoader.findLoadedClass", false, false);
  // This might have an error pending. But semantics are to just return null.
  if (self->IsExceptionPending()) {
    // If it is an InternalError, keep it. See CheckExceptionGenerateClassNotFound.
    std::string type(PrettyTypeOf(self->GetException()));
    if (type != "java.lang.InternalError") {
      self->ClearException();
    }
  }
}

void UnstartedRuntime::UnstartedVoidLookupType(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame ATTRIBUTE_UNUSED, JValue* result,
    size_t arg_offset ATTRIBUTE_UNUSED) {
  result->SetL(Runtime::Current()->GetClassLinker()->FindPrimitiveClass('V'));
}

// Arraycopy emulation.
// Note: we can't use any fast copy functions, as they are not available under transaction.

template <typename T>
static void PrimitiveArrayCopy(Thread* self,
                               mirror::Array* src_array, int32_t src_pos,
                               mirror::Array* dst_array, int32_t dst_pos,
                               int32_t length)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (src_array->GetClass()->GetComponentType() != dst_array->GetClass()->GetComponentType()) {
    AbortTransactionOrFail(self, "Types mismatched in arraycopy: %s vs %s.",
                           PrettyDescriptor(src_array->GetClass()->GetComponentType()).c_str(),
                           PrettyDescriptor(dst_array->GetClass()->GetComponentType()).c_str());
    return;
  }
  mirror::PrimitiveArray<T>* src = down_cast<mirror::PrimitiveArray<T>*>(src_array);
  mirror::PrimitiveArray<T>* dst = down_cast<mirror::PrimitiveArray<T>*>(dst_array);
  const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= length);
  if (copy_forward) {
    for (int32_t i = 0; i < length; ++i) {
      dst->Set(dst_pos + i, src->Get(src_pos + i));
    }
  } else {
    for (int32_t i = 1; i <= length; ++i) {
      dst->Set(dst_pos + length - i, src->Get(src_pos + length - i));
    }
  }
}

void UnstartedRuntime::UnstartedSystemArraycopy(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset) {
  // Special case array copying without initializing System.
  jint src_pos = shadow_frame->GetVReg(arg_offset + 1);
  jint dst_pos = shadow_frame->GetVReg(arg_offset + 3);
  jint length = shadow_frame->GetVReg(arg_offset + 4);

  mirror::Object* src_obj = shadow_frame->GetVRegReference(arg_offset);
  mirror::Object* dst_obj = shadow_frame->GetVRegReference(arg_offset + 2);
  // Null checking. For simplicity, abort transaction.
  if (src_obj == nullptr) {
    AbortTransactionOrFail(self, "src is null in arraycopy.");
    return;
  }
  if (dst_obj == nullptr) {
    AbortTransactionOrFail(self, "dst is null in arraycopy.");
    return;
  }
  // Test for arrayness. Throw ArrayStoreException.
  if (!src_obj->IsArrayInstance() || !dst_obj->IsArrayInstance()) {
    self->ThrowNewException("Ljava/lang/ArrayStoreException;", "src or trg is not an array");
    return;
  }

  mirror::Array* src_array = src_obj->AsArray();
  mirror::Array* dst_array = dst_obj->AsArray();

  // Bounds checking. Throw IndexOutOfBoundsException.
  if (UNLIKELY(src_pos < 0) || UNLIKELY(dst_pos < 0) || UNLIKELY(length < 0) ||
      UNLIKELY(src_pos > src_array->GetLength() - length) ||
      UNLIKELY(dst_pos > dst_array->GetLength() - length)) {
    self->ThrowNewExceptionF("Ljava/lang/IndexOutOfBoundsException;",
                             "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
                             src_array->GetLength(), src_pos, dst_array->GetLength(), dst_pos,
                             length);
    return;
  }

  // Type checking.
  mirror::Class* src_type = shadow_frame->GetVRegReference(arg_offset)->GetClass()->
      GetComponentType();

  if (!src_type->IsPrimitive()) {
    // Check that the second type is not primitive.
    mirror::Class* trg_type = shadow_frame->GetVRegReference(arg_offset + 2)->GetClass()->
        GetComponentType();
    if (trg_type->IsPrimitiveInt()) {
      AbortTransactionOrFail(self, "Type mismatch in arraycopy: %s vs %s",
                             PrettyDescriptor(src_array->GetClass()->GetComponentType()).c_str(),
                             PrettyDescriptor(dst_array->GetClass()->GetComponentType()).c_str());
      return;
    }

    mirror::ObjectArray<mirror::Object>* src = src_array->AsObjectArray<mirror::Object>();
    mirror::ObjectArray<mirror::Object>* dst = dst_array->AsObjectArray<mirror::Object>();
    if (src == dst) {
      // Can overlap, but not have type mismatches.
      // We cannot use ObjectArray::MemMove here, as it doesn't support transactions.
      const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= length);
      if (copy_forward) {
        for (int32_t i = 0; i < length; ++i) {
          dst->Set(dst_pos + i, src->Get(src_pos + i));
        }
      } else {
        for (int32_t i = 1; i <= length; ++i) {
          dst->Set(dst_pos + length - i, src->Get(src_pos + length - i));
        }
      }
    } else {
      // We're being lazy here. Optimally this could be a memcpy (if component types are
      // assignable), but the ObjectArray implementation doesn't support transactions. The
      // checking version, however, does.
      if (Runtime::Current()->IsActiveTransaction()) {
        dst->AssignableCheckingMemcpy<true>(
            dst_pos, src, src_pos, length, true /* throw_exception */);
      } else {
        dst->AssignableCheckingMemcpy<false>(
                    dst_pos, src, src_pos, length, true /* throw_exception */);
      }
    }
  } else if (src_type->IsPrimitiveByte()) {
    PrimitiveArrayCopy<uint8_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else if (src_type->IsPrimitiveChar()) {
    PrimitiveArrayCopy<uint16_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else if (src_type->IsPrimitiveInt()) {
    PrimitiveArrayCopy<int32_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else {
    AbortTransactionOrFail(self, "Unimplemented System.arraycopy for type '%s'",
                           PrettyDescriptor(src_type).c_str());
  }
}

void UnstartedRuntime::UnstartedSystemArraycopyByte(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Just forward.
  UnstartedRuntime::UnstartedSystemArraycopy(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedSystemArraycopyChar(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Just forward.
  UnstartedRuntime::UnstartedSystemArraycopy(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedSystemArraycopyInt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Just forward.
  UnstartedRuntime::UnstartedSystemArraycopy(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedSystemGetSecurityManager(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame ATTRIBUTE_UNUSED,
    JValue* result, size_t arg_offset ATTRIBUTE_UNUSED) {
  result->SetL(nullptr);
}

static constexpr const char* kAndroidHardcodedSystemPropertiesFieldName = "STATIC_PROPERTIES";

static void GetSystemProperty(Thread* self,
                              ShadowFrame* shadow_frame,
                              JValue* result,
                              size_t arg_offset,
                              bool is_default_version)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  StackHandleScope<4> hs(self);
  Handle<mirror::String> h_key(
      hs.NewHandle(reinterpret_cast<mirror::String*>(shadow_frame->GetVRegReference(arg_offset))));
  if (h_key.Get() == nullptr) {
    AbortTransactionOrFail(self, "getProperty key was null");
    return;
  }

  // This is overall inefficient, but reflecting the values here is not great, either. So
  // for simplicity, and with the assumption that the number of getProperty calls is not
  // too great, just iterate each time.

  // Get the storage class.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> h_props_class(hs.NewHandle(
      class_linker->FindClass(self,
                              "Ljava/lang/AndroidHardcodedSystemProperties;",
                              ScopedNullHandle<mirror::ClassLoader>())));
  if (h_props_class.Get() == nullptr) {
    AbortTransactionOrFail(self, "Could not find AndroidHardcodedSystemProperties");
    return;
  }
  if (!class_linker->EnsureInitialized(self, h_props_class, true, true)) {
    AbortTransactionOrFail(self, "Could not initialize AndroidHardcodedSystemProperties");
    return;
  }

  // Get the storage array.
  ArtField* static_properties =
      h_props_class->FindDeclaredStaticField(kAndroidHardcodedSystemPropertiesFieldName,
                                             "[[Ljava/lang/String;");
  if (static_properties == nullptr) {
    AbortTransactionOrFail(self,
                           "Could not find %s field",
                           kAndroidHardcodedSystemPropertiesFieldName);
    return;
  }
  Handle<mirror::ObjectArray<mirror::ObjectArray<mirror::String>>> h_2string_array(
      hs.NewHandle(reinterpret_cast<mirror::ObjectArray<mirror::ObjectArray<mirror::String>>*>(
          static_properties->GetObject(h_props_class.Get()))));
  if (h_2string_array.Get() == nullptr) {
    AbortTransactionOrFail(self, "Field %s is null", kAndroidHardcodedSystemPropertiesFieldName);
    return;
  }

  // Iterate over it.
  const int32_t prop_count = h_2string_array->GetLength();
  // Use the third handle as mutable.
  MutableHandle<mirror::ObjectArray<mirror::String>> h_string_array(
      hs.NewHandle<mirror::ObjectArray<mirror::String>>(nullptr));
  for (int32_t i = 0; i < prop_count; ++i) {
    h_string_array.Assign(h_2string_array->Get(i));
    if (h_string_array.Get() == nullptr ||
        h_string_array->GetLength() != 2 ||
        h_string_array->Get(0) == nullptr) {
      AbortTransactionOrFail(self,
                             "Unexpected content of %s",
                             kAndroidHardcodedSystemPropertiesFieldName);
      return;
    }
    if (h_key->Equals(h_string_array->Get(0))) {
      // Found a value.
      if (h_string_array->Get(1) == nullptr && is_default_version) {
        // Null is being delegated to the default map, and then resolved to the given default value.
        // As there's no default map, return the given value.
        result->SetL(shadow_frame->GetVRegReference(arg_offset + 1));
      } else {
        result->SetL(h_string_array->Get(1));
      }
      return;
    }
  }

  // Key is not supported.
  AbortTransactionOrFail(self, "getProperty key %s not supported", h_key->ToModifiedUtf8().c_str());
}

void UnstartedRuntime::UnstartedSystemGetProperty(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  GetSystemProperty(self, shadow_frame, result, arg_offset, false);
}

void UnstartedRuntime::UnstartedSystemGetPropertyWithDefault(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  GetSystemProperty(self, shadow_frame, result, arg_offset, true);
}

void UnstartedRuntime::UnstartedThreadLocalGet(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset ATTRIBUTE_UNUSED) {
  std::string caller(PrettyMethod(shadow_frame->GetLink()->GetMethod()));
  bool ok = false;
  if (caller == "void java.lang.FloatingDecimal.developLongDigits(int, long, long)" ||
      caller == "java.lang.String java.lang.FloatingDecimal.toJavaFormatString()") {
    // Allocate non-threadlocal buffer.
    result->SetL(mirror::CharArray::Alloc(self, 26));
    ok = true;
  } else if (caller ==
             "java.lang.FloatingDecimal java.lang.FloatingDecimal.getThreadLocalInstance()") {
    // Allocate new object.
    StackHandleScope<2> hs(self);
    Handle<mirror::Class> h_real_to_string_class(hs.NewHandle(
        shadow_frame->GetLink()->GetMethod()->GetDeclaringClass()));
    Handle<mirror::Object> h_real_to_string_obj(hs.NewHandle(
        h_real_to_string_class->AllocObject(self)));
    if (h_real_to_string_obj.Get() != nullptr) {
      auto* cl = Runtime::Current()->GetClassLinker();
      ArtMethod* init_method = h_real_to_string_class->FindDirectMethod(
          "<init>", "()V", cl->GetImagePointerSize());
      if (init_method == nullptr) {
        h_real_to_string_class->DumpClass(LOG(FATAL), mirror::Class::kDumpClassFullDetail);
      } else {
        JValue invoke_result;
        EnterInterpreterFromInvoke(self, init_method, h_real_to_string_obj.Get(), nullptr,
                                   nullptr);
        if (!self->IsExceptionPending()) {
          result->SetL(h_real_to_string_obj.Get());
          ok = true;
        }
      }
    }
  }

  if (!ok) {
    AbortTransactionOrFail(self, "Could not create RealToString object");
  }
}

void UnstartedRuntime::UnstartedMathCeil(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  result->SetD(ceil(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathFloor(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  result->SetD(floor(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathSin(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  result->SetD(sin(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathCos(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  result->SetD(cos(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathPow(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  result->SetD(pow(shadow_frame->GetVRegDouble(arg_offset),
                   shadow_frame->GetVRegDouble(arg_offset + 2)));
}

void UnstartedRuntime::UnstartedObjectHashCode(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  result->SetI(obj->IdentityHashCode());
}

void UnstartedRuntime::UnstartedDoubleDoubleToRawLongBits(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  double in = shadow_frame->GetVRegDouble(arg_offset);
  result->SetJ(bit_cast<int64_t, double>(in));
}

static mirror::Object* GetDexFromDexCache(Thread* self, mirror::DexCache* dex_cache)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const DexFile* dex_file = dex_cache->GetDexFile();
  if (dex_file == nullptr) {
    return nullptr;
  }

  // Create the direct byte buffer.
  JNIEnv* env = self->GetJniEnv();
  DCHECK(env != nullptr);
  void* address = const_cast<void*>(reinterpret_cast<const void*>(dex_file->Begin()));
  ScopedLocalRef<jobject> byte_buffer(env, env->NewDirectByteBuffer(address, dex_file->Size()));
  if (byte_buffer.get() == nullptr) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  jvalue args[1];
  args[0].l = byte_buffer.get();

  ScopedLocalRef<jobject> dex(env, env->CallStaticObjectMethodA(
      WellKnownClasses::com_android_dex_Dex,
      WellKnownClasses::com_android_dex_Dex_create,
      args));

  return self->DecodeJObject(dex.get());
}

void UnstartedRuntime::UnstartedDexCacheGetDexNative(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // We will create the Dex object, but the image writer will release it before creating the
  // art file.
  mirror::Object* src = shadow_frame->GetVRegReference(arg_offset);
  bool have_dex = false;
  if (src != nullptr) {
    mirror::Object* dex = GetDexFromDexCache(self, reinterpret_cast<mirror::DexCache*>(src));
    if (dex != nullptr) {
      have_dex = true;
      result->SetL(dex);
    }
  }
  if (!have_dex) {
    self->ClearException();
    Runtime::Current()->AbortTransactionAndThrowAbortError(self, "Could not create Dex object");
  }
}

static void UnstartedMemoryPeek(
    Primitive::Type type, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  int64_t address = shadow_frame->GetVRegLong(arg_offset);
  // TODO: Check that this is in the heap somewhere. Otherwise we will segfault instead of
  //       aborting the transaction.

  switch (type) {
    case Primitive::kPrimByte: {
      result->SetB(*reinterpret_cast<int8_t*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimShort: {
      typedef int16_t unaligned_short __attribute__ ((aligned (1)));
      result->SetS(*reinterpret_cast<unaligned_short*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimInt: {
      typedef int32_t unaligned_int __attribute__ ((aligned (1)));
      result->SetI(*reinterpret_cast<unaligned_int*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimLong: {
      typedef int64_t unaligned_long __attribute__ ((aligned (1)));
      result->SetJ(*reinterpret_cast<unaligned_long*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimVoid:
    case Primitive::kPrimNot:
      LOG(FATAL) << "Not in the Memory API: " << type;
      UNREACHABLE();
  }
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

void UnstartedRuntime::UnstartedMemoryPeekByte(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimByte, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedMemoryPeekShort(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimShort, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedMemoryPeekInt(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimInt, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedMemoryPeekLong(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimLong, shadow_frame, result, arg_offset);
}

static void UnstartedMemoryPeekArray(
    Primitive::Type type, Thread* self, ShadowFrame* shadow_frame, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  int64_t address_long = shadow_frame->GetVRegLong(arg_offset);
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 2);
  if (obj == nullptr) {
    Runtime::Current()->AbortTransactionAndThrowAbortError(self, "Null pointer in peekArray");
    return;
  }
  mirror::Array* array = obj->AsArray();

  int offset = shadow_frame->GetVReg(arg_offset + 3);
  int count = shadow_frame->GetVReg(arg_offset + 4);
  if (offset < 0 || offset + count > array->GetLength()) {
    std::string error_msg(StringPrintf("Array out of bounds in peekArray: %d/%d vs %d",
                                       offset, count, array->GetLength()));
    Runtime::Current()->AbortTransactionAndThrowAbortError(self, error_msg.c_str());
    return;
  }

  switch (type) {
    case Primitive::kPrimByte: {
      int8_t* address = reinterpret_cast<int8_t*>(static_cast<intptr_t>(address_long));
      mirror::ByteArray* byte_array = array->AsByteArray();
      for (int32_t i = 0; i < count; ++i, ++address) {
        byte_array->SetWithoutChecks<true>(i + offset, *address);
      }
      return;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      LOG(FATAL) << "Type unimplemented for Memory Array API, should not reach here: " << type;
      UNREACHABLE();

    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimVoid:
    case Primitive::kPrimNot:
      LOG(FATAL) << "Not in the Memory API: " << type;
      UNREACHABLE();
  }
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

void UnstartedRuntime::UnstartedMemoryPeekByteArray(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset) {
  UnstartedMemoryPeekArray(Primitive::kPrimByte, self, shadow_frame, arg_offset);
}

// This allows reading the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringGetCharsNoCheck(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset) {
  jint start = shadow_frame->GetVReg(arg_offset + 1);
  jint end = shadow_frame->GetVReg(arg_offset + 2);
  jint index = shadow_frame->GetVReg(arg_offset + 4);
  mirror::String* string = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.getCharsNoCheck with null object");
    return;
  }
  DCHECK_GE(start, 0);
  DCHECK_GE(end, string->GetLength());
  StackHandleScope<1> hs(self);
  Handle<mirror::CharArray> h_char_array(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset + 3)->AsCharArray()));
  DCHECK_LE(index, h_char_array->GetLength());
  DCHECK_LE(end - start, h_char_array->GetLength() - index);
  string->GetChars(start, end, h_char_array, index);
}

// This allows reading chars from the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringCharAt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint index = shadow_frame->GetVReg(arg_offset + 1);
  mirror::String* string = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.charAt with null object");
    return;
  }
  result->SetC(string->CharAt(index));
}

// This allows setting chars from the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringSetCharAt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset) {
  jint index = shadow_frame->GetVReg(arg_offset + 1);
  jchar c = shadow_frame->GetVReg(arg_offset + 2);
  mirror::String* string = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.setCharAt with null object");
    return;
  }
  string->SetCharAt(index, c);
}

// This allows creating the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringFactoryNewStringFromChars(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint offset = shadow_frame->GetVReg(arg_offset);
  jint char_count = shadow_frame->GetVReg(arg_offset + 1);
  DCHECK_GE(char_count, 0);
  StackHandleScope<1> hs(self);
  Handle<mirror::CharArray> h_char_array(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset + 2)->AsCharArray()));
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::String::AllocFromCharArray<true>(self, char_count, h_char_array, offset, allocator));
}

// This allows creating the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringFactoryNewStringFromString(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::String* to_copy = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (to_copy == nullptr) {
    AbortTransactionOrFail(self, "StringFactory.newStringFromString with null object");
    return;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_string(hs.NewHandle(to_copy));
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::String::AllocFromString<true>(self, h_string->GetLength(), h_string, 0,
                                                     allocator));
}

void UnstartedRuntime::UnstartedStringFastSubstring(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint start = shadow_frame->GetVReg(arg_offset + 1);
  jint length = shadow_frame->GetVReg(arg_offset + 2);
  DCHECK_GE(start, 0);
  DCHECK_GE(length, 0);
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_string(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsString()));
  DCHECK_LE(start, h_string->GetLength());
  DCHECK_LE(start + length, h_string->GetLength());
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::String::AllocFromString<true>(self, length, h_string, start, allocator));
}

// This allows getting the char array for new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringToCharArray(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::String* string = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.charAt with null object");
    return;
  }
  result->SetL(string->ToCharArray(self));
}

// This allows statically initializing ConcurrentHashMap and SynchronousQueue.
void UnstartedRuntime::UnstartedReferenceGetReferent(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  mirror::Reference* const ref = down_cast<mirror::Reference*>(
      shadow_frame->GetVRegReference(arg_offset));
  if (ref == nullptr) {
    AbortTransactionOrFail(self, "Reference.getReferent() with null object");
    return;
  }
  mirror::Object* const referent =
      Runtime::Current()->GetHeap()->GetReferenceProcessor()->GetReferent(self, ref);
  result->SetL(referent);
}

// This allows statically initializing ConcurrentHashMap and SynchronousQueue. We use a somewhat
// conservative upper bound. We restrict the callers to SynchronousQueue and ConcurrentHashMap,
// where we can predict the behavior (somewhat).
// Note: this is required (instead of lazy initialization) as these classes are used in the static
//       initialization of other classes, so will *use* the value.
void UnstartedRuntime::UnstartedRuntimeAvailableProcessors(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset ATTRIBUTE_UNUSED) {
  std::string caller(PrettyMethod(shadow_frame->GetLink()->GetMethod()));
  if (caller == "void java.util.concurrent.SynchronousQueue.<clinit>()") {
    // SynchronousQueue really only separates between single- and multiprocessor case. Return
    // 8 as a conservative upper approximation.
    result->SetI(8);
  } else if (caller == "void java.util.concurrent.ConcurrentHashMap.<clinit>()") {
    // ConcurrentHashMap uses it for striding. 8 still seems an OK general value, as it's likely
    // a good upper bound.
    // TODO: Consider resetting in the zygote?
    result->SetI(8);
  } else {
    // Not supported.
    AbortTransactionOrFail(self, "Accessing availableProcessors not allowed");
  }
}

// This allows accessing ConcurrentHashMap/SynchronousQueue.

void UnstartedRuntime::UnstartedUnsafeCompareAndSwapLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  int64_t expectedValue = shadow_frame->GetVRegLong(arg_offset + 4);
  int64_t newValue = shadow_frame->GetVRegLong(arg_offset + 6);

  // Must use non transactional mode.
  if (kUseReadBarrier) {
    // Need to make sure the reference stored in the field is a to-space one before attempting the
    // CAS or the CAS could fail incorrectly.
    mirror::HeapReference<mirror::Object>* field_addr =
        reinterpret_cast<mirror::HeapReference<mirror::Object>*>(
            reinterpret_cast<uint8_t*>(obj) + static_cast<size_t>(offset));
    ReadBarrier::Barrier<mirror::Object, kWithReadBarrier, /*kAlwaysUpdateField*/true>(
        obj,
        MemberOffset(offset),
        field_addr);
  }
  bool success;
  // Check whether we're in a transaction, call accordingly.
  if (Runtime::Current()->IsActiveTransaction()) {
    success = obj->CasFieldStrongSequentiallyConsistent64<true>(MemberOffset(offset),
                                                                expectedValue,
                                                                newValue);
  } else {
    success = obj->CasFieldStrongSequentiallyConsistent64<false>(MemberOffset(offset),
                                                                 expectedValue,
                                                                 newValue);
  }
  result->SetZ(success ? 1 : 0);
}

void UnstartedRuntime::UnstartedUnsafeCompareAndSwapObject(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* expected_value = shadow_frame->GetVRegReference(arg_offset + 4);
  mirror::Object* newValue = shadow_frame->GetVRegReference(arg_offset + 5);

  // Must use non transactional mode.
  if (kUseReadBarrier) {
    // Need to make sure the reference stored in the field is a to-space one before attempting the
    // CAS or the CAS could fail incorrectly.
    mirror::HeapReference<mirror::Object>* field_addr =
        reinterpret_cast<mirror::HeapReference<mirror::Object>*>(
            reinterpret_cast<uint8_t*>(obj) + static_cast<size_t>(offset));
    ReadBarrier::Barrier<mirror::Object, kWithReadBarrier, /*kAlwaysUpdateField*/true>(
        obj,
        MemberOffset(offset),
        field_addr);
  }
  bool success;
  // Check whether we're in a transaction, call accordingly.
  if (Runtime::Current()->IsActiveTransaction()) {
    success = obj->CasFieldStrongSequentiallyConsistentObject<true>(MemberOffset(offset),
                                                                    expected_value,
                                                                    newValue);
  } else {
    success = obj->CasFieldStrongSequentiallyConsistentObject<false>(MemberOffset(offset),
                                                                     expected_value,
                                                                     newValue);
  }
  result->SetZ(success ? 1 : 0);
}

void UnstartedRuntime::UnstartedUnsafeGetObjectVolatile(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* value = obj->GetFieldObjectVolatile<mirror::Object>(MemberOffset(offset));
  result->SetL(value);
}

void UnstartedRuntime::UnstartedUnsafePutObjectVolatile(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* value = shadow_frame->GetVRegReference(arg_offset + 4);
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->SetFieldObjectVolatile<true>(MemberOffset(offset), value);
  } else {
    obj->SetFieldObjectVolatile<false>(MemberOffset(offset), value);
  }
}

void UnstartedRuntime::UnstartedUnsafePutOrderedObject(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* newValue = shadow_frame->GetVRegReference(arg_offset + 4);
  QuasiAtomic::ThreadFenceRelease();
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->SetFieldObject<true>(MemberOffset(offset), newValue);
  } else {
    obj->SetFieldObject<false>(MemberOffset(offset), newValue);
  }
}

// A cutout for Integer.parseInt(String). Note: this code is conservative and will bail instead
// of correctly handling the corner cases.
void UnstartedRuntime::UnstartedIntegerParseInt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot parse null string, retry at runtime.");
    return;
  }

  std::string string_value = obj->AsString()->ToModifiedUtf8();
  if (string_value.empty()) {
    AbortTransactionOrFail(self, "Cannot parse empty string, retry at runtime.");
    return;
  }

  const char* c_str = string_value.c_str();
  char *end;
  // Can we set errno to 0? Is this always a variable, and not a macro?
  // Worst case, we'll incorrectly fail a transaction. Seems OK.
  int64_t l = strtol(c_str, &end, 10);

  if ((errno == ERANGE && l == LONG_MAX) || l > std::numeric_limits<int32_t>::max() ||
      (errno == ERANGE && l == LONG_MIN) || l < std::numeric_limits<int32_t>::min()) {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }
  if (l == 0) {
    // Check whether the string wasn't exactly zero.
    if (string_value != "0") {
      AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
      return;
    }
  } else if (*end != '\0') {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }

  result->SetI(static_cast<int32_t>(l));
}

// A cutout for Long.parseLong.
//
// Note: for now use code equivalent to Integer.parseInt, as the full range may not be supported
//       well.
void UnstartedRuntime::UnstartedLongParseLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot parse null string, retry at runtime.");
    return;
  }

  std::string string_value = obj->AsString()->ToModifiedUtf8();
  if (string_value.empty()) {
    AbortTransactionOrFail(self, "Cannot parse empty string, retry at runtime.");
    return;
  }

  const char* c_str = string_value.c_str();
  char *end;
  // Can we set errno to 0? Is this always a variable, and not a macro?
  // Worst case, we'll incorrectly fail a transaction. Seems OK.
  int64_t l = strtol(c_str, &end, 10);

  // Note: comparing against int32_t min/max is intentional here.
  if ((errno == ERANGE && l == LONG_MAX) || l > std::numeric_limits<int32_t>::max() ||
      (errno == ERANGE && l == LONG_MIN) || l < std::numeric_limits<int32_t>::min()) {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }
  if (l == 0) {
    // Check whether the string wasn't exactly zero.
    if (string_value != "0") {
      AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
      return;
    }
  } else if (*end != '\0') {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }

  result->SetJ(l);
}

void UnstartedRuntime::UnstartedMethodInvoke(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(self);

  mirror::Object* java_method_obj = shadow_frame->GetVRegReference(arg_offset);
  ScopedLocalRef<jobject> java_method(env,
      java_method_obj == nullptr ? nullptr :env->AddLocalReference<jobject>(java_method_obj));

  mirror::Object* java_receiver_obj = shadow_frame->GetVRegReference(arg_offset + 1);
  ScopedLocalRef<jobject> java_receiver(env,
      java_receiver_obj == nullptr ? nullptr : env->AddLocalReference<jobject>(java_receiver_obj));

  mirror::Object* java_args_obj = shadow_frame->GetVRegReference(arg_offset + 2);
  ScopedLocalRef<jobject> java_args(env,
      java_args_obj == nullptr ? nullptr : env->AddLocalReference<jobject>(java_args_obj));

  ScopedLocalRef<jobject> result_jobj(env,
      InvokeMethod(soa, java_method.get(), java_receiver.get(), java_args.get()));

  result->SetL(self->DecodeJObject(result_jobj.get()));

  // Conservatively flag all exceptions as transaction aborts. This way we don't need to unwrap
  // InvocationTargetExceptions.
  if (self->IsExceptionPending()) {
    AbortTransactionOrFail(self, "Failed Method.invoke");
  }
}


void UnstartedRuntime::UnstartedJNIVMRuntimeNewUnpaddedArray(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args, JValue* result) {
  int32_t length = args[1];
  DCHECK_GE(length, 0);
  mirror::Class* element_class = reinterpret_cast<mirror::Object*>(args[0])->AsClass();
  Runtime* runtime = Runtime::Current();
  mirror::Class* array_class = runtime->GetClassLinker()->FindArrayClass(self, &element_class);
  DCHECK(array_class != nullptr);
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::Array::Alloc<true, true>(self, array_class, length,
                                                array_class->GetComponentSizeShift(), allocator));
}

void UnstartedRuntime::UnstartedJNIVMStackGetCallingClassLoader(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  result->SetL(nullptr);
}

void UnstartedRuntime::UnstartedJNIVMStackGetStackClass2(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  NthCallerVisitor visitor(self, 3);
  visitor.WalkStack();
  if (visitor.caller != nullptr) {
    result->SetL(visitor.caller->GetDeclaringClass());
  }
}

void UnstartedRuntime::UnstartedJNIMathLog(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  JValue value;
  value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
  result->SetD(log(value.GetD()));
}

void UnstartedRuntime::UnstartedJNIMathExp(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  JValue value;
  value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
  result->SetD(exp(value.GetD()));
}

void UnstartedRuntime::UnstartedJNIAtomicLongVMSupportsCS8(
    Thread* self ATTRIBUTE_UNUSED,
    ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args ATTRIBUTE_UNUSED,
    JValue* result) {
  result->SetZ(QuasiAtomic::LongAtomicsUseMutexes(Runtime::Current()->GetInstructionSet())
                   ? 0
                   : 1);
}

void UnstartedRuntime::UnstartedJNIClassGetNameNative(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver,
    uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  StackHandleScope<1> hs(self);
  result->SetL(mirror::Class::ComputeName(hs.NewHandle(receiver->AsClass())));
}

void UnstartedRuntime::UnstartedJNIDoubleLongBitsToDouble(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  uint64_t long_input = args[0] | (static_cast<uint64_t>(args[1]) << 32);
  result->SetD(bit_cast<double>(long_input));
}

void UnstartedRuntime::UnstartedJNIFloatFloatToRawIntBits(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  result->SetI(args[0]);
}

void UnstartedRuntime::UnstartedJNIFloatIntBitsToFloat(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  result->SetI(args[0]);
}

void UnstartedRuntime::UnstartedJNIObjectInternalClone(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver,
    uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  result->SetL(receiver->Clone(self));
}

void UnstartedRuntime::UnstartedJNIObjectNotifyAll(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver,
    uint32_t* args ATTRIBUTE_UNUSED, JValue* result ATTRIBUTE_UNUSED) {
  receiver->NotifyAll(self);
}

void UnstartedRuntime::UnstartedJNIStringCompareTo(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver, uint32_t* args,
    JValue* result) {
  mirror::String* rhs = reinterpret_cast<mirror::Object*>(args[0])->AsString();
  if (rhs == nullptr) {
    AbortTransactionOrFail(self, "String.compareTo with null object");
  }
  result->SetI(receiver->AsString()->CompareTo(rhs));
}

void UnstartedRuntime::UnstartedJNIStringIntern(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver,
    uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  result->SetL(receiver->AsString()->Intern());
}

void UnstartedRuntime::UnstartedJNIStringFastIndexOf(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver,
    uint32_t* args, JValue* result) {
  result->SetI(receiver->AsString()->FastIndexOf(args[0], args[1]));
}

void UnstartedRuntime::UnstartedJNIArrayCreateMultiArray(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args, JValue* result) {
  StackHandleScope<2> hs(self);
  auto h_class(hs.NewHandle(reinterpret_cast<mirror::Class*>(args[0])->AsClass()));
  auto h_dimensions(hs.NewHandle(reinterpret_cast<mirror::IntArray*>(args[1])->AsIntArray()));
  result->SetL(mirror::Array::CreateMultiArray(self, h_class, h_dimensions));
}

void UnstartedRuntime::UnstartedJNIArrayCreateObjectArray(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args, JValue* result) {
  int32_t length = static_cast<int32_t>(args[1]);
  if (length < 0) {
    ThrowNegativeArraySizeException(length);
    return;
  }
  mirror::Class* element_class = reinterpret_cast<mirror::Class*>(args[0])->AsClass();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  mirror::Class* array_class = class_linker->FindArrayClass(self, &element_class);
  if (UNLIKELY(array_class == nullptr)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  DCHECK(array_class->IsObjectArrayClass());
  mirror::Array* new_array = mirror::ObjectArray<mirror::Object*>::Alloc(
      self, array_class, length, runtime->GetHeap()->GetCurrentAllocator());
  result->SetL(new_array);
}

void UnstartedRuntime::UnstartedJNIThrowableNativeFillInStackTrace(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  ScopedObjectAccessUnchecked soa(self);
  if (Runtime::Current()->IsActiveTransaction()) {
    result->SetL(soa.Decode<mirror::Object*>(self->CreateInternalStackTrace<true>(soa)));
  } else {
    result->SetL(soa.Decode<mirror::Object*>(self->CreateInternalStackTrace<false>(soa)));
  }
}

void UnstartedRuntime::UnstartedJNISystemIdentityHashCode(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  result->SetI((obj != nullptr) ? obj->IdentityHashCode() : 0);
}

void UnstartedRuntime::UnstartedJNIByteOrderIsLittleEndian(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args ATTRIBUTE_UNUSED, JValue* result) {
  result->SetZ(JNI_TRUE);
}

void UnstartedRuntime::UnstartedJNIUnsafeCompareAndSwapInt(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  jint expectedValue = args[3];
  jint newValue = args[4];
  bool success;
  if (Runtime::Current()->IsActiveTransaction()) {
    success = obj->CasFieldStrongSequentiallyConsistent32<true>(MemberOffset(offset),
                                                                expectedValue, newValue);
  } else {
    success = obj->CasFieldStrongSequentiallyConsistent32<false>(MemberOffset(offset),
                                                                 expectedValue, newValue);
  }
  result->SetZ(success ? JNI_TRUE : JNI_FALSE);
}

void UnstartedRuntime::UnstartedJNIUnsafeGetIntVolatile(
    Thread* self, ArtMethod* method ATTRIBUTE_UNUSED, mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args, JValue* result) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }

  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  result->SetI(obj->GetField32Volatile(MemberOffset(offset)));
}

void UnstartedRuntime::UnstartedJNIUnsafePutObject(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result ATTRIBUTE_UNUSED) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  mirror::Object* newValue = reinterpret_cast<mirror::Object*>(args[3]);
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->SetFieldObject<true>(MemberOffset(offset), newValue);
  } else {
    obj->SetFieldObject<false>(MemberOffset(offset), newValue);
  }
}

void UnstartedRuntime::UnstartedJNIUnsafeGetArrayBaseOffsetForComponentType(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  mirror::Class* component = reinterpret_cast<mirror::Object*>(args[0])->AsClass();
  Primitive::Type primitive_type = component->GetPrimitiveType();
  result->SetI(mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value());
}

void UnstartedRuntime::UnstartedJNIUnsafeGetArrayIndexScaleForComponentType(
    Thread* self ATTRIBUTE_UNUSED, ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED, uint32_t* args, JValue* result) {
  mirror::Class* component = reinterpret_cast<mirror::Object*>(args[0])->AsClass();
  Primitive::Type primitive_type = component->GetPrimitiveType();
  result->SetI(Primitive::ComponentSize(primitive_type));
}

typedef void (*InvokeHandler)(Thread* self, ShadowFrame* shadow_frame, JValue* result,
    size_t arg_size);

typedef void (*JNIHandler)(Thread* self, ArtMethod* method, mirror::Object* receiver,
    uint32_t* args, JValue* result);

static bool tables_initialized_ = false;
static std::unordered_map<std::string, InvokeHandler> invoke_handlers_;
static std::unordered_map<std::string, JNIHandler> jni_handlers_;

void UnstartedRuntime::InitializeInvokeHandlers() {
#define UNSTARTED_DIRECT(ShortName, Sig) \
  invoke_handlers_.insert(std::make_pair(Sig, & UnstartedRuntime::Unstarted ## ShortName));
#include "unstarted_runtime_list.h"
  UNSTARTED_RUNTIME_DIRECT_LIST(UNSTARTED_DIRECT)
#undef UNSTARTED_RUNTIME_DIRECT_LIST
#undef UNSTARTED_RUNTIME_JNI_LIST
#undef UNSTARTED_DIRECT
}

void UnstartedRuntime::InitializeJNIHandlers() {
#define UNSTARTED_JNI(ShortName, Sig) \
  jni_handlers_.insert(std::make_pair(Sig, & UnstartedRuntime::UnstartedJNI ## ShortName));
#include "unstarted_runtime_list.h"
  UNSTARTED_RUNTIME_JNI_LIST(UNSTARTED_JNI)
#undef UNSTARTED_RUNTIME_DIRECT_LIST
#undef UNSTARTED_RUNTIME_JNI_LIST
#undef UNSTARTED_JNI
}

void UnstartedRuntime::Initialize() {
  CHECK(!tables_initialized_);

  InitializeInvokeHandlers();
  InitializeJNIHandlers();

  tables_initialized_ = true;
}

void UnstartedRuntime::Invoke(Thread* self, const DexFile::CodeItem* code_item,
                              ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  CHECK(tables_initialized_);

  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  const auto& iter = invoke_handlers_.find(name);
  if (iter != invoke_handlers_.end()) {
    // Clear out the result in case it's not zeroed out.
    result->SetL(0);

    // Push the shadow frame. This is so the failing method can be seen in abort dumps.
    self->PushShadowFrame(shadow_frame);

    (*iter->second)(self, shadow_frame, result, arg_offset);

    self->PopShadowFrame();
  } else {
    // Not special, continue with regular interpreter execution.
    ArtInterpreterToInterpreterBridge(self, code_item, shadow_frame, result);
  }
}

// Hand select a number of methods to be run in a not yet started runtime without using JNI.
void UnstartedRuntime::Jni(Thread* self, ArtMethod* method, mirror::Object* receiver,
                           uint32_t* args, JValue* result) {
  std::string name(PrettyMethod(method));
  const auto& iter = jni_handlers_.find(name);
  if (iter != jni_handlers_.end()) {
    // Clear out the result in case it's not zeroed out.
    result->SetL(0);
    (*iter->second)(self, method, receiver, args, result);
  } else if (Runtime::Current()->IsActiveTransaction()) {
    AbortTransactionF(self, "Attempt to invoke native method in non-started runtime: %s",
                      name.c_str());
  } else {
    LOG(FATAL) << "Calling native method " << PrettyMethod(method) << " in an unstarted "
        "non-transactional runtime";
  }
}

}  // namespace interpreter
}  // namespace art
