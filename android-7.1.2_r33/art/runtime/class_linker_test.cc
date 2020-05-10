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

#include "class_linker.h"

#include <memory>
#include <string>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "experimental_flags.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/heap.h"
#include "mirror/abstract_method.h"
#include "mirror/accessible_object.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/field.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/proxy.h"
#include "mirror/reference.h"
#include "mirror/stack_trace_element.h"
#include "mirror/string-inl.h"
#include "handle_scope-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

class ClassLinkerTest : public CommonRuntimeTest {
 protected:
  void AssertNonExistentClass(const std::string& descriptor)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    EXPECT_TRUE(class_linker_->FindSystemClass(self, descriptor.c_str()) == nullptr);
    EXPECT_TRUE(self->IsExceptionPending());
    mirror::Object* exception = self->GetException();
    self->ClearException();
    mirror::Class* exception_class =
        class_linker_->FindSystemClass(self, "Ljava/lang/NoClassDefFoundError;");
    EXPECT_TRUE(exception->InstanceOf(exception_class));
  }

  void AssertPrimitiveClass(const std::string& descriptor)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    AssertPrimitiveClass(descriptor, class_linker_->FindSystemClass(self, descriptor.c_str()));
  }

  void AssertPrimitiveClass(const std::string& descriptor, mirror::Class* primitive)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ASSERT_TRUE(primitive != nullptr);
    ASSERT_TRUE(primitive->GetClass() != nullptr);
    ASSERT_EQ(primitive->GetClass(), primitive->GetClass()->GetClass());
    EXPECT_TRUE(primitive->GetClass()->GetSuperClass() != nullptr);
    std::string temp;
    ASSERT_STREQ(descriptor.c_str(), primitive->GetDescriptor(&temp));
    EXPECT_TRUE(primitive->GetSuperClass() == nullptr);
    EXPECT_FALSE(primitive->HasSuperClass());
    EXPECT_TRUE(primitive->GetClassLoader() == nullptr);
    EXPECT_EQ(mirror::Class::kStatusInitialized, primitive->GetStatus());
    EXPECT_FALSE(primitive->IsErroneous());
    EXPECT_TRUE(primitive->IsLoaded());
    EXPECT_TRUE(primitive->IsResolved());
    EXPECT_TRUE(primitive->IsVerified());
    EXPECT_TRUE(primitive->IsInitialized());
    EXPECT_FALSE(primitive->IsArrayInstance());
    EXPECT_FALSE(primitive->IsArrayClass());
    EXPECT_TRUE(primitive->GetComponentType() == nullptr);
    EXPECT_FALSE(primitive->IsInterface());
    EXPECT_TRUE(primitive->IsPublic());
    EXPECT_TRUE(primitive->IsFinal());
    EXPECT_TRUE(primitive->IsPrimitive());
    EXPECT_FALSE(primitive->IsSynthetic());
    EXPECT_EQ(0U, primitive->NumDirectMethods());
    EXPECT_EQ(0U, primitive->NumVirtualMethods());
    EXPECT_EQ(0U, primitive->NumInstanceFields());
    EXPECT_EQ(0U, primitive->NumStaticFields());
    EXPECT_EQ(0U, primitive->NumDirectInterfaces());
    EXPECT_FALSE(primitive->HasVTable());
    EXPECT_EQ(0, primitive->GetIfTableCount());
    EXPECT_TRUE(primitive->GetIfTable() == nullptr);
    EXPECT_EQ(kAccPublic | kAccFinal | kAccAbstract, primitive->GetAccessFlags());
  }

  void AssertObjectClass(mirror::Class* JavaLangObject)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ASSERT_TRUE(JavaLangObject != nullptr);
    ASSERT_TRUE(JavaLangObject->GetClass() != nullptr);
    ASSERT_EQ(JavaLangObject->GetClass(),
              JavaLangObject->GetClass()->GetClass());
    EXPECT_EQ(JavaLangObject, JavaLangObject->GetClass()->GetSuperClass());
    std::string temp;
    ASSERT_STREQ(JavaLangObject->GetDescriptor(&temp), "Ljava/lang/Object;");
    EXPECT_TRUE(JavaLangObject->GetSuperClass() == nullptr);
    EXPECT_FALSE(JavaLangObject->HasSuperClass());
    EXPECT_TRUE(JavaLangObject->GetClassLoader() == nullptr);
    EXPECT_EQ(mirror::Class::kStatusInitialized, JavaLangObject->GetStatus());
    EXPECT_FALSE(JavaLangObject->IsErroneous());
    EXPECT_TRUE(JavaLangObject->IsLoaded());
    EXPECT_TRUE(JavaLangObject->IsResolved());
    EXPECT_TRUE(JavaLangObject->IsVerified());
    EXPECT_TRUE(JavaLangObject->IsInitialized());
    EXPECT_FALSE(JavaLangObject->IsArrayInstance());
    EXPECT_FALSE(JavaLangObject->IsArrayClass());
    EXPECT_TRUE(JavaLangObject->GetComponentType() == nullptr);
    EXPECT_FALSE(JavaLangObject->IsInterface());
    EXPECT_TRUE(JavaLangObject->IsPublic());
    EXPECT_FALSE(JavaLangObject->IsFinal());
    EXPECT_FALSE(JavaLangObject->IsPrimitive());
    EXPECT_FALSE(JavaLangObject->IsSynthetic());
    EXPECT_EQ(2U, JavaLangObject->NumDirectMethods());
    EXPECT_EQ(11U, JavaLangObject->NumVirtualMethods());
    if (!kUseBrooksReadBarrier) {
      EXPECT_EQ(2U, JavaLangObject->NumInstanceFields());
    } else {
      EXPECT_EQ(4U, JavaLangObject->NumInstanceFields());
    }
    EXPECT_STREQ(JavaLangObject->GetInstanceField(0)->GetName(),
                 "shadow$_klass_");
    EXPECT_STREQ(JavaLangObject->GetInstanceField(1)->GetName(),
                 "shadow$_monitor_");
    if (kUseBrooksReadBarrier) {
      EXPECT_STREQ(JavaLangObject->GetInstanceField(2)->GetName(),
                   "shadow$_x_rb_ptr_");
      EXPECT_STREQ(JavaLangObject->GetInstanceField(3)->GetName(),
                   "shadow$_x_xpadding_");
    }

    EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
    EXPECT_EQ(0U, JavaLangObject->NumDirectInterfaces());

    size_t pointer_size = class_linker_->GetImagePointerSize();
    ArtMethod* unimplemented = runtime_->GetImtUnimplementedMethod();
    ImTable* imt = JavaLangObject->GetImt(pointer_size);
    ASSERT_NE(nullptr, imt);
    for (size_t i = 0; i < ImTable::kSize; ++i) {
      ASSERT_EQ(unimplemented, imt->Get(i, pointer_size));
    }
  }

  void AssertArrayClass(const std::string& array_descriptor,
                        const std::string& component_type,
                        mirror::ClassLoader* class_loader)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    StackHandleScope<2> hs(self);
    Handle<mirror::ClassLoader> loader(hs.NewHandle(class_loader));
    Handle<mirror::Class> array(
        hs.NewHandle(class_linker_->FindClass(self, array_descriptor.c_str(), loader)));
    std::string temp;
    EXPECT_STREQ(component_type.c_str(), array->GetComponentType()->GetDescriptor(&temp));
    EXPECT_EQ(class_loader, array->GetClassLoader());
    EXPECT_EQ(kAccFinal | kAccAbstract, (array->GetAccessFlags() & (kAccFinal | kAccAbstract)));
    AssertArrayClass(array_descriptor, array);
  }

  void AssertArrayClass(const std::string& array_descriptor, Handle<mirror::Class> array)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ASSERT_TRUE(array.Get() != nullptr);
    ASSERT_TRUE(array->GetClass() != nullptr);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != nullptr);
    std::string temp;
    ASSERT_STREQ(array_descriptor.c_str(), array->GetDescriptor(&temp));
    EXPECT_TRUE(array->GetSuperClass() != nullptr);
    Thread* self = Thread::Current();
    EXPECT_EQ(class_linker_->FindSystemClass(self, "Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    ASSERT_TRUE(array->GetComponentType() != nullptr);
    ASSERT_GT(strlen(array->GetComponentType()->GetDescriptor(&temp)), 0U);
    EXPECT_EQ(mirror::Class::kStatusInitialized, array->GetStatus());
    EXPECT_FALSE(array->IsErroneous());
    EXPECT_TRUE(array->IsLoaded());
    EXPECT_TRUE(array->IsResolved());
    EXPECT_TRUE(array->IsVerified());
    EXPECT_TRUE(array->IsInitialized());
    EXPECT_FALSE(array->IsArrayInstance());
    EXPECT_TRUE(array->IsArrayClass());
    EXPECT_FALSE(array->IsInterface());
    EXPECT_EQ(array->GetComponentType()->IsPublic(), array->IsPublic());
    EXPECT_TRUE(array->IsFinal());
    EXPECT_FALSE(array->IsPrimitive());
    EXPECT_FALSE(array->IsSynthetic());
    EXPECT_EQ(0U, array->NumDirectMethods());
    EXPECT_EQ(0U, array->NumVirtualMethods());
    EXPECT_EQ(0U, array->NumInstanceFields());
    EXPECT_EQ(0U, array->NumStaticFields());
    EXPECT_EQ(2U, array->NumDirectInterfaces());
    EXPECT_TRUE(array->ShouldHaveImt());
    EXPECT_TRUE(array->ShouldHaveEmbeddedVTable());
    EXPECT_EQ(2, array->GetIfTableCount());
    ASSERT_TRUE(array->GetIfTable() != nullptr);
    mirror::Class* direct_interface0 = mirror::Class::GetDirectInterface(self, array, 0);
    EXPECT_TRUE(direct_interface0 != nullptr);
    EXPECT_STREQ(direct_interface0->GetDescriptor(&temp), "Ljava/lang/Cloneable;");
    mirror::Class* direct_interface1 = mirror::Class::GetDirectInterface(self, array, 1);
    EXPECT_STREQ(direct_interface1->GetDescriptor(&temp), "Ljava/io/Serializable;");
    mirror::Class* array_ptr = array->GetComponentType();
    EXPECT_EQ(class_linker_->FindArrayClass(self, &array_ptr), array.Get());

    size_t pointer_size = class_linker_->GetImagePointerSize();
    mirror::Class* JavaLangObject =
        class_linker_->FindSystemClass(self, "Ljava/lang/Object;");
    ImTable* JavaLangObject_imt = JavaLangObject->GetImt(pointer_size);
    // IMT of a array class should be shared with the IMT of the java.lag.Object
    ASSERT_EQ(JavaLangObject_imt, array->GetImt(pointer_size));
  }

  void AssertMethod(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_) {
    EXPECT_TRUE(method != nullptr);
    EXPECT_TRUE(method->GetDeclaringClass() != nullptr);
    EXPECT_TRUE(method->GetName() != nullptr);
    EXPECT_TRUE(method->GetSignature() != Signature::NoSignature());

    EXPECT_TRUE(method->HasDexCacheResolvedMethods(sizeof(void*)));
    EXPECT_TRUE(method->HasDexCacheResolvedTypes(sizeof(void*)));
    EXPECT_TRUE(method->HasSameDexCacheResolvedMethods(
        method->GetDeclaringClass()->GetDexCache()->GetResolvedMethods(),
        sizeof(void*)));
    EXPECT_TRUE(method->HasSameDexCacheResolvedTypes(
        method->GetDeclaringClass()->GetDexCache()->GetResolvedTypes(),
        sizeof(void*)));
  }

  void AssertField(mirror::Class* klass, ArtField* field)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    EXPECT_TRUE(field != nullptr);
    EXPECT_EQ(klass, field->GetDeclaringClass());
    EXPECT_TRUE(field->GetName() != nullptr);
    EXPECT_TRUE(field->GetType<true>() != nullptr);
  }

  void AssertClass(const std::string& descriptor, Handle<mirror::Class> klass)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string temp;
    EXPECT_STREQ(descriptor.c_str(), klass->GetDescriptor(&temp));
    if (descriptor == "Ljava/lang/Object;") {
      EXPECT_FALSE(klass->HasSuperClass());
    } else {
      EXPECT_TRUE(klass->HasSuperClass());
      EXPECT_TRUE(klass->GetSuperClass() != nullptr);
    }
    EXPECT_TRUE(klass->GetClass() != nullptr);
    EXPECT_EQ(klass->GetClass(), klass->GetClass()->GetClass());
    EXPECT_TRUE(klass->GetDexCache() != nullptr);
    EXPECT_TRUE(klass->IsLoaded());
    EXPECT_TRUE(klass->IsResolved());
    EXPECT_FALSE(klass->IsErroneous());
    EXPECT_FALSE(klass->IsArrayClass());
    EXPECT_TRUE(klass->GetComponentType() == nullptr);
    EXPECT_TRUE(klass->IsInSamePackage(klass.Get()));
    EXPECT_TRUE(klass->GetDexCacheStrings() != nullptr);
    EXPECT_EQ(klass->GetDexCacheStrings(), klass->GetDexCache()->GetStrings());
    std::string temp2;
    EXPECT_TRUE(mirror::Class::IsInSamePackage(klass->GetDescriptor(&temp),
                                               klass->GetDescriptor(&temp2)));
    if (klass->IsInterface()) {
      EXPECT_TRUE(klass->IsAbstract());
      // Check that all direct methods are static (either <clinit> or a regular static method).
      for (ArtMethod& m : klass->GetDirectMethods(sizeof(void*))) {
        EXPECT_TRUE(m.IsStatic());
        EXPECT_TRUE(m.IsDirect());
      }
    } else {
      if (!klass->IsSynthetic()) {
        EXPECT_NE(0U, klass->NumDirectMethods());
      }
    }
    EXPECT_EQ(klass->IsInterface(), !klass->HasVTable());
    mirror::IfTable* iftable = klass->GetIfTable();
    for (int i = 0; i < klass->GetIfTableCount(); i++) {
      mirror::Class* interface = iftable->GetInterface(i);
      ASSERT_TRUE(interface != nullptr);
      if (klass->IsInterface()) {
        EXPECT_EQ(0U, iftable->GetMethodArrayCount(i));
      } else {
        EXPECT_EQ(interface->NumDeclaredVirtualMethods(), iftable->GetMethodArrayCount(i));
      }
    }
    if (klass->IsAbstract()) {
      EXPECT_FALSE(klass->IsFinal());
    } else {
      EXPECT_FALSE(klass->IsAnnotation());
    }
    if (klass->IsFinal()) {
      EXPECT_FALSE(klass->IsAbstract());
      EXPECT_FALSE(klass->IsAnnotation());
    }
    if (klass->IsAnnotation()) {
      EXPECT_FALSE(klass->IsFinal());
      EXPECT_TRUE(klass->IsAbstract());
    }

    EXPECT_FALSE(klass->IsPrimitive());
    EXPECT_TRUE(klass->CanAccess(klass.Get()));

    for (ArtMethod& method : klass->GetDirectMethods(sizeof(void*))) {
      AssertMethod(&method);
      EXPECT_TRUE(method.IsDirect());
      EXPECT_EQ(klass.Get(), method.GetDeclaringClass());
    }

    for (ArtMethod& method : klass->GetDeclaredVirtualMethods(sizeof(void*))) {
      AssertMethod(&method);
      EXPECT_FALSE(method.IsDirect());
      EXPECT_EQ(klass.Get(), method.GetDeclaringClass());
    }

    for (ArtMethod& method : klass->GetCopiedMethods(sizeof(void*))) {
      AssertMethod(&method);
      EXPECT_FALSE(method.IsDirect());
      EXPECT_TRUE(method.IsCopied());
      EXPECT_TRUE(method.GetDeclaringClass()->IsInterface())
          << "declaring class: " << PrettyClass(method.GetDeclaringClass());
      EXPECT_TRUE(method.GetDeclaringClass()->IsAssignableFrom(klass.Get()))
          << "declaring class: " << PrettyClass(method.GetDeclaringClass());
    }

    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
      ArtField* field = klass->GetInstanceField(i);
      AssertField(klass.Get(), field);
      EXPECT_FALSE(field->IsStatic());
    }

    for (size_t i = 0; i < klass->NumStaticFields(); i++) {
      ArtField* field = klass->GetStaticField(i);
      AssertField(klass.Get(), field);
      EXPECT_TRUE(field->IsStatic());
    }

    // Confirm that all instances field offsets are packed together at the start.
    EXPECT_GE(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
    MemberOffset start_ref_offset = klass->GetFirstReferenceInstanceFieldOffset();
    MemberOffset end_ref_offset(start_ref_offset.Uint32Value() +
                                klass->NumReferenceInstanceFields() *
                                    sizeof(mirror::HeapReference<mirror::Object>));
    MemberOffset current_ref_offset = start_ref_offset;
    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
      ArtField* field = klass->GetInstanceField(i);
      mirror::Class* field_type = field->GetType<true>();
      ASSERT_TRUE(field_type != nullptr);
      if (!field->IsPrimitiveType()) {
        ASSERT_TRUE(!field_type->IsPrimitive());
        ASSERT_EQ(current_ref_offset.Uint32Value(), field->GetOffset().Uint32Value());
        if (current_ref_offset.Uint32Value() == end_ref_offset.Uint32Value()) {
          // While Reference.referent is not primitive, the ClassLinker
          // treats it as such so that the garbage collector won't scan it.
          EXPECT_EQ(PrettyField(field),
                    "java.lang.Object java.lang.ref.Reference.referent");
        } else {
          current_ref_offset = MemberOffset(current_ref_offset.Uint32Value() +
                                            sizeof(mirror::HeapReference<mirror::Object>));
        }
      } else {
        if (field->GetOffset().Uint32Value() < end_ref_offset.Uint32Value()) {
          // Shuffled before references.
          ASSERT_LT(field->GetOffset().Uint32Value(), start_ref_offset.Uint32Value());
          CHECK(!IsAligned<4>(field->GetOffset().Uint32Value()));
        }
      }
    }
    ASSERT_EQ(end_ref_offset.Uint32Value(), current_ref_offset.Uint32Value());

    uint32_t total_num_reference_instance_fields = 0;
    mirror::Class* k = klass.Get();
    while (k != nullptr) {
      total_num_reference_instance_fields += k->NumReferenceInstanceFields();
      k = k->GetSuperClass();
    }
    EXPECT_GE(total_num_reference_instance_fields, 1U);  // Should always have Object's class.
    if (klass->GetReferenceInstanceOffsets() != mirror::Class::kClassWalkSuper) {
      // The reference instance offsets have a bit set for each reference offset.
      // +1 for Object's class.
      EXPECT_EQ(static_cast<uint32_t>(POPCOUNT(klass->GetReferenceInstanceOffsets())) + 1,
                total_num_reference_instance_fields);
    }
  }

  void AssertDexFileClass(mirror::ClassLoader* class_loader, const std::string& descriptor)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    ASSERT_TRUE(descriptor != nullptr);
    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> klass(
        hs.NewHandle(class_linker_->FindSystemClass(self, descriptor.c_str())));
    ASSERT_TRUE(klass.Get() != nullptr);
    std::string temp;
    EXPECT_STREQ(descriptor.c_str(), klass.Get()->GetDescriptor(&temp));
    EXPECT_EQ(class_loader, klass->GetClassLoader());
    if (klass->IsPrimitive()) {
      AssertPrimitiveClass(descriptor, klass.Get());
    } else if (klass->IsArrayClass()) {
      AssertArrayClass(descriptor, klass);
    } else {
      AssertClass(descriptor, klass);
    }
  }

  void AssertDexFile(const DexFile& dex, mirror::ClassLoader* class_loader)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    // Verify all the classes defined in this file
    for (size_t i = 0; i < dex.NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex.GetClassDef(i);
      const char* descriptor = dex.GetClassDescriptor(class_def);
      AssertDexFileClass(class_loader, descriptor);
    }
    // Verify all the types referenced by this file
    for (size_t i = 0; i < dex.NumTypeIds(); i++) {
      const DexFile::TypeId& type_id = dex.GetTypeId(i);
      const char* descriptor = dex.GetTypeDescriptor(type_id);
      AssertDexFileClass(class_loader, descriptor);
    }
    TestRootVisitor visitor;
    class_linker_->VisitRoots(&visitor, kVisitRootFlagAllRoots);
    // Verify the dex cache has resolution methods in all resolved method slots
    mirror::DexCache* dex_cache = class_linker_->FindDexCache(Thread::Current(), dex);
    auto* resolved_methods = dex_cache->GetResolvedMethods();
    for (size_t i = 0, num_methods = dex_cache->NumResolvedMethods(); i != num_methods; ++i) {
      EXPECT_TRUE(
          mirror::DexCache::GetElementPtrSize(resolved_methods, i, sizeof(void*)) != nullptr)
          << dex.GetLocation() << " i=" << i;
    }
  }

  class TestRootVisitor : public SingleRootVisitor {
   public:
    void VisitRoot(mirror::Object* root, const RootInfo& info ATTRIBUTE_UNUSED) OVERRIDE {
      EXPECT_TRUE(root != nullptr);
    }
  };
};

struct CheckOffset {
  size_t cpp_offset;
  const char* java_name;
  CheckOffset(size_t c, const char* j) : cpp_offset(c), java_name(j) {}
};

template <typename T>
struct CheckOffsets {
  CheckOffsets(bool is_static_in, const char* class_descriptor_in)
      : is_static(is_static_in), class_descriptor(class_descriptor_in) {}
  bool is_static;
  std::string class_descriptor;
  std::vector<CheckOffset> offsets;

  bool Check() SHARED_REQUIRES(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    mirror::Class* klass =
        Runtime::Current()->GetClassLinker()->FindSystemClass(self, class_descriptor.c_str());
    CHECK(klass != nullptr) << class_descriptor;

    bool error = false;

    // Classes have a different size due to padding field. Strings are variable length.
    if (!klass->IsClassClass() && !klass->IsStringClass() && !is_static) {
      // Currently only required for AccessibleObject since of the padding fields. The class linker
      // says AccessibleObject is 9 bytes but sizeof(AccessibleObject) is 12 bytes due to padding.
      // The RoundUp is to get around this case.
      static constexpr size_t kPackAlignment = 4;
      size_t expected_size = RoundUp(is_static ? klass->GetClassSize(): klass->GetObjectSize(),
          kPackAlignment);
      if (sizeof(T) != expected_size) {
        LOG(ERROR) << "Class size mismatch:"
           << " class=" << class_descriptor
           << " Java=" << expected_size
           << " C++=" << sizeof(T);
        error = true;
      }
    }

    size_t num_fields = is_static ? klass->NumStaticFields() : klass->NumInstanceFields();
    if (offsets.size() != num_fields) {
      LOG(ERROR) << "Field count mismatch:"
         << " class=" << class_descriptor
         << " Java=" << num_fields
         << " C++=" << offsets.size();
      error = true;
    }

    for (size_t i = 0; i < offsets.size(); i++) {
      ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
      StringPiece field_name(field->GetName());
      if (field_name != offsets[i].java_name) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
        StringPiece field_name(field->GetName());
        if (field_name != offsets[i].java_name) {
          LOG(ERROR) << "JAVA FIELD ORDER MISMATCH NEXT LINE:";
        }
        LOG(ERROR) << "Java field order:"
           << " i=" << i << " class=" << class_descriptor
           << " Java=" << field_name
           << " CheckOffsets=" << offset.java_name;
      }
    }

    for (size_t i = 0; i < offsets.size(); i++) {
      CheckOffset& offset = offsets[i];
      ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
      if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
        if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
          LOG(ERROR) << "OFFSET MISMATCH NEXT LINE:";
        }
        LOG(ERROR) << "Offset: class=" << class_descriptor << " field=" << offset.java_name
           << " Java=" << field->GetOffset().Uint32Value() << " C++=" << offset.cpp_offset;
      }
    }

    return !error;
  };

  void addOffset(size_t offset, const char* name) {
    offsets.push_back(CheckOffset(offset, name));
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(CheckOffsets);
};

// Note that ClassLinkerTest.ValidateFieldOrderOfJavaCppUnionClasses
// is first since if it is failing, others are unlikely to succeed.

struct ObjectOffsets : public CheckOffsets<mirror::Object> {
  ObjectOffsets() : CheckOffsets<mirror::Object>(false, "Ljava/lang/Object;") {
    addOffset(OFFSETOF_MEMBER(mirror::Object, klass_), "shadow$_klass_");
    addOffset(OFFSETOF_MEMBER(mirror::Object, monitor_), "shadow$_monitor_");
#ifdef USE_BROOKS_READ_BARRIER
    addOffset(OFFSETOF_MEMBER(mirror::Object, x_rb_ptr_), "shadow$_x_rb_ptr_");
    addOffset(OFFSETOF_MEMBER(mirror::Object, x_xpadding_), "shadow$_x_xpadding_");
#endif
  };
};

struct ClassOffsets : public CheckOffsets<mirror::Class> {
  ClassOffsets() : CheckOffsets<mirror::Class>(false, "Ljava/lang/Class;") {
    addOffset(OFFSETOF_MEMBER(mirror::Class, access_flags_), "accessFlags");
    addOffset(OFFSETOF_MEMBER(mirror::Class, annotation_type_), "annotationType");
    addOffset(OFFSETOF_MEMBER(mirror::Class, class_flags_), "classFlags");
    addOffset(OFFSETOF_MEMBER(mirror::Class, class_loader_), "classLoader");
    addOffset(OFFSETOF_MEMBER(mirror::Class, class_size_), "classSize");
    addOffset(OFFSETOF_MEMBER(mirror::Class, clinit_thread_id_), "clinitThreadId");
    addOffset(OFFSETOF_MEMBER(mirror::Class, component_type_), "componentType");
    addOffset(OFFSETOF_MEMBER(mirror::Class, copied_methods_offset_), "copiedMethodsOffset");
    addOffset(OFFSETOF_MEMBER(mirror::Class, dex_cache_), "dexCache");
    addOffset(OFFSETOF_MEMBER(mirror::Class, dex_cache_strings_), "dexCacheStrings");
    addOffset(OFFSETOF_MEMBER(mirror::Class, dex_class_def_idx_), "dexClassDefIndex");
    addOffset(OFFSETOF_MEMBER(mirror::Class, dex_type_idx_), "dexTypeIndex");
    addOffset(OFFSETOF_MEMBER(mirror::Class, ifields_), "iFields");
    addOffset(OFFSETOF_MEMBER(mirror::Class, iftable_), "ifTable");
    addOffset(OFFSETOF_MEMBER(mirror::Class, methods_), "methods");
    addOffset(OFFSETOF_MEMBER(mirror::Class, name_), "name");
    addOffset(OFFSETOF_MEMBER(mirror::Class, num_reference_instance_fields_),
              "numReferenceInstanceFields");
    addOffset(OFFSETOF_MEMBER(mirror::Class, num_reference_static_fields_),
              "numReferenceStaticFields");
    addOffset(OFFSETOF_MEMBER(mirror::Class, object_size_), "objectSize");
    addOffset(OFFSETOF_MEMBER(mirror::Class, primitive_type_), "primitiveType");
    addOffset(OFFSETOF_MEMBER(mirror::Class, reference_instance_offsets_),
              "referenceInstanceOffsets");
    addOffset(OFFSETOF_MEMBER(mirror::Class, sfields_), "sFields");
    addOffset(OFFSETOF_MEMBER(mirror::Class, status_), "status");
    addOffset(OFFSETOF_MEMBER(mirror::Class, super_class_), "superClass");
    addOffset(OFFSETOF_MEMBER(mirror::Class, verify_error_), "verifyError");
    addOffset(OFFSETOF_MEMBER(mirror::Class, virtual_methods_offset_), "virtualMethodsOffset");
    addOffset(OFFSETOF_MEMBER(mirror::Class, vtable_), "vtable");
  };
};

struct StringOffsets : public CheckOffsets<mirror::String> {
  StringOffsets() : CheckOffsets<mirror::String>(false, "Ljava/lang/String;") {
    addOffset(OFFSETOF_MEMBER(mirror::String, count_), "count");
    addOffset(OFFSETOF_MEMBER(mirror::String, hash_code_), "hash");
  };
};

struct ThrowableOffsets : public CheckOffsets<mirror::Throwable> {
  ThrowableOffsets() : CheckOffsets<mirror::Throwable>(false, "Ljava/lang/Throwable;") {
    addOffset(OFFSETOF_MEMBER(mirror::Throwable, backtrace_), "backtrace");
    addOffset(OFFSETOF_MEMBER(mirror::Throwable, cause_), "cause");
    addOffset(OFFSETOF_MEMBER(mirror::Throwable, detail_message_), "detailMessage");
    addOffset(OFFSETOF_MEMBER(mirror::Throwable, stack_trace_), "stackTrace");
    addOffset(OFFSETOF_MEMBER(mirror::Throwable, suppressed_exceptions_), "suppressedExceptions");
  };
};

struct StackTraceElementOffsets : public CheckOffsets<mirror::StackTraceElement> {
  StackTraceElementOffsets() : CheckOffsets<mirror::StackTraceElement>(
      false, "Ljava/lang/StackTraceElement;") {
    addOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, declaring_class_), "declaringClass");
    addOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, file_name_), "fileName");
    addOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, line_number_), "lineNumber");
    addOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, method_name_), "methodName");
  };
};

struct ClassLoaderOffsets : public CheckOffsets<mirror::ClassLoader> {
  ClassLoaderOffsets() : CheckOffsets<mirror::ClassLoader>(false, "Ljava/lang/ClassLoader;") {
    addOffset(OFFSETOF_MEMBER(mirror::ClassLoader, allocator_), "allocator");
    addOffset(OFFSETOF_MEMBER(mirror::ClassLoader, class_table_), "classTable");
    addOffset(OFFSETOF_MEMBER(mirror::ClassLoader, packages_), "packages");
    addOffset(OFFSETOF_MEMBER(mirror::ClassLoader, parent_), "parent");
    addOffset(OFFSETOF_MEMBER(mirror::ClassLoader, proxyCache_), "proxyCache");
  };
};

struct ProxyOffsets : public CheckOffsets<mirror::Proxy> {
  ProxyOffsets() : CheckOffsets<mirror::Proxy>(false, "Ljava/lang/reflect/Proxy;") {
    addOffset(OFFSETOF_MEMBER(mirror::Proxy, h_), "h");
  };
};

struct DexCacheOffsets : public CheckOffsets<mirror::DexCache> {
  DexCacheOffsets() : CheckOffsets<mirror::DexCache>(false, "Ljava/lang/DexCache;") {
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, dex_), "dex");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, dex_file_), "dexFile");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, location_), "location");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, num_resolved_fields_), "numResolvedFields");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, num_resolved_methods_), "numResolvedMethods");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, num_resolved_types_), "numResolvedTypes");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, num_strings_), "numStrings");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, resolved_fields_), "resolvedFields");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, resolved_methods_), "resolvedMethods");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, resolved_types_), "resolvedTypes");
    addOffset(OFFSETOF_MEMBER(mirror::DexCache, strings_), "strings");
  };
};

struct ReferenceOffsets : public CheckOffsets<mirror::Reference> {
  ReferenceOffsets() : CheckOffsets<mirror::Reference>(false, "Ljava/lang/ref/Reference;") {
    addOffset(OFFSETOF_MEMBER(mirror::Reference, pending_next_), "pendingNext");
    addOffset(OFFSETOF_MEMBER(mirror::Reference, queue_), "queue");
    addOffset(OFFSETOF_MEMBER(mirror::Reference, queue_next_), "queueNext");
    addOffset(OFFSETOF_MEMBER(mirror::Reference, referent_), "referent");
  };
};

struct FinalizerReferenceOffsets : public CheckOffsets<mirror::FinalizerReference> {
  FinalizerReferenceOffsets() : CheckOffsets<mirror::FinalizerReference>(
      false, "Ljava/lang/ref/FinalizerReference;") {
    addOffset(OFFSETOF_MEMBER(mirror::FinalizerReference, next_), "next");
    addOffset(OFFSETOF_MEMBER(mirror::FinalizerReference, prev_), "prev");
    addOffset(OFFSETOF_MEMBER(mirror::FinalizerReference, zombie_), "zombie");
  };
};

struct AccessibleObjectOffsets : public CheckOffsets<mirror::AccessibleObject> {
  AccessibleObjectOffsets() : CheckOffsets<mirror::AccessibleObject>(
      false, "Ljava/lang/reflect/AccessibleObject;") {
    addOffset(mirror::AccessibleObject::FlagOffset().Uint32Value(), "override");
  };
};

struct FieldOffsets : public CheckOffsets<mirror::Field> {
  FieldOffsets() : CheckOffsets<mirror::Field>(false, "Ljava/lang/reflect/Field;") {
    addOffset(OFFSETOF_MEMBER(mirror::Field, access_flags_), "accessFlags");
    addOffset(OFFSETOF_MEMBER(mirror::Field, declaring_class_), "declaringClass");
    addOffset(OFFSETOF_MEMBER(mirror::Field, dex_field_index_), "dexFieldIndex");
    addOffset(OFFSETOF_MEMBER(mirror::Field, offset_), "offset");
    addOffset(OFFSETOF_MEMBER(mirror::Field, type_), "type");
  };
};

struct AbstractMethodOffsets : public CheckOffsets<mirror::AbstractMethod> {
  AbstractMethodOffsets() : CheckOffsets<mirror::AbstractMethod>(
      false, "Ljava/lang/reflect/AbstractMethod;") {
    addOffset(OFFSETOF_MEMBER(mirror::AbstractMethod, access_flags_), "accessFlags");
    addOffset(OFFSETOF_MEMBER(mirror::AbstractMethod, art_method_), "artMethod");
    addOffset(OFFSETOF_MEMBER(mirror::AbstractMethod, declaring_class_), "declaringClass");
    addOffset(OFFSETOF_MEMBER(mirror::AbstractMethod, declaring_class_of_overridden_method_),
              "declaringClassOfOverriddenMethod");
    addOffset(OFFSETOF_MEMBER(mirror::AbstractMethod, dex_method_index_), "dexMethodIndex");
  };
};

// C++ fields must exactly match the fields in the Java classes. If this fails,
// reorder the fields in the C++ class. Managed class fields are ordered by
// ClassLinker::LinkFields.
TEST_F(ClassLinkerTest, ValidateFieldOrderOfJavaCppUnionClasses) {
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_TRUE(ObjectOffsets().Check());
  EXPECT_TRUE(ClassOffsets().Check());
  EXPECT_TRUE(StringOffsets().Check());
  EXPECT_TRUE(ThrowableOffsets().Check());
  EXPECT_TRUE(StackTraceElementOffsets().Check());
  EXPECT_TRUE(ClassLoaderOffsets().Check());
  EXPECT_TRUE(ProxyOffsets().Check());
  EXPECT_TRUE(DexCacheOffsets().Check());
  EXPECT_TRUE(ReferenceOffsets().Check());
  EXPECT_TRUE(FinalizerReferenceOffsets().Check());
  EXPECT_TRUE(AccessibleObjectOffsets().Check());
  EXPECT_TRUE(FieldOffsets().Check());
  EXPECT_TRUE(AbstractMethodOffsets().Check());
}

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  ScopedObjectAccess soa(Thread::Current());
  AssertNonExistentClass("NoSuchClass;");
  AssertNonExistentClass("LNoSuchClass;");
}

TEST_F(ClassLinkerTest, GetDexFiles) {
  ScopedObjectAccess soa(Thread::Current());

  jobject jclass_loader = LoadDex("Nested");
  std::vector<const DexFile*> dex_files(GetDexFiles(jclass_loader));
  ASSERT_EQ(dex_files.size(), 1U);
  EXPECT_TRUE(EndsWith(dex_files[0]->GetLocation(), "Nested.jar"));

  jobject jclass_loader2 = LoadDex("MultiDex");
  std::vector<const DexFile*> dex_files2(GetDexFiles(jclass_loader2));
  ASSERT_EQ(dex_files2.size(), 2U);
  EXPECT_TRUE(EndsWith(dex_files2[0]->GetLocation(), "MultiDex.jar"));
}

TEST_F(ClassLinkerTest, FindClassNested) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Nested"))));

  mirror::Class* outer = class_linker_->FindClass(soa.Self(), "LNested;", class_loader);
  ASSERT_TRUE(outer != nullptr);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  mirror::Class* inner = class_linker_->FindClass(soa.Self(), "LNested$Inner;", class_loader);
  ASSERT_TRUE(inner != nullptr);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass_Primitives) {
  ScopedObjectAccess soa(Thread::Current());
  const std::string expected("BCDFIJSZV");
  for (int ch = 1; ch < 256; ++ch) {
    std::string descriptor;
    descriptor.push_back(ch);
    if (expected.find(ch) == std::string::npos) {
      AssertNonExistentClass(descriptor);
    } else {
      AssertPrimitiveClass(descriptor);
    }
  }
}

TEST_F(ClassLinkerTest, FindClass) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* JavaLangObject = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  AssertObjectClass(JavaLangObject);

  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("MyClass"))));
  AssertNonExistentClass("LMyClass;");
  mirror::Class* MyClass = class_linker_->FindClass(soa.Self(), "LMyClass;", class_loader);
  ASSERT_TRUE(MyClass != nullptr);
  ASSERT_TRUE(MyClass->GetClass() != nullptr);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  std::string temp;
  ASSERT_STREQ(MyClass->GetDescriptor(&temp), "LMyClass;");
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_EQ(class_loader.Get(), MyClass->GetClassLoader());
  EXPECT_EQ(mirror::Class::kStatusResolved, MyClass->GetStatus());
  EXPECT_FALSE(MyClass->IsErroneous());
  EXPECT_TRUE(MyClass->IsLoaded());
  EXPECT_TRUE(MyClass->IsResolved());
  EXPECT_FALSE(MyClass->IsVerified());
  EXPECT_FALSE(MyClass->IsInitialized());
  EXPECT_FALSE(MyClass->IsArrayInstance());
  EXPECT_FALSE(MyClass->IsArrayClass());
  EXPECT_TRUE(MyClass->GetComponentType() == nullptr);
  EXPECT_FALSE(MyClass->IsInterface());
  EXPECT_FALSE(MyClass->IsPublic());
  EXPECT_FALSE(MyClass->IsFinal());
  EXPECT_FALSE(MyClass->IsPrimitive());
  EXPECT_FALSE(MyClass->IsSynthetic());
  EXPECT_EQ(1U, MyClass->NumDirectMethods());
  EXPECT_EQ(0U, MyClass->NumVirtualMethods());
  EXPECT_EQ(0U, MyClass->NumInstanceFields());
  EXPECT_EQ(0U, MyClass->NumStaticFields());
  EXPECT_EQ(0U, MyClass->NumDirectInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", "C", nullptr);
  AssertArrayClass("[Ljava/lang/Object;", "Ljava/lang/Object;", nullptr);
  // synthesized on the fly
  AssertArrayClass("[[C", "[C", nullptr);
  AssertArrayClass("[[[LMyClass;", "[[LMyClass;", class_loader.Get());
  // or not available at all
  AssertNonExistentClass("[[[[LNonExistentClass;");
}

TEST_F(ClassLinkerTest, LibCore) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  AssertDexFile(*java_lang_dex_file_, nullptr);
}

// The first reference array element must be a multiple of 4 bytes from the
// start of the object
TEST_F(ClassLinkerTest, ValidateObjectArrayElementsOffset) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* array_class = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;");
  mirror::ObjectArray<mirror::String>* array =
      mirror::ObjectArray<mirror::String>::Alloc(soa.Self(), array_class, 0);
  uintptr_t data_offset =
      reinterpret_cast<uintptr_t>(array->GetRawData(sizeof(mirror::HeapReference<mirror::String>),
                                                    0));
  if (sizeof(mirror::HeapReference<mirror::String>) == sizeof(int32_t)) {
    EXPECT_TRUE(IsAligned<4>(data_offset));  // Check 4 byte alignment.
  } else {
    EXPECT_TRUE(IsAligned<8>(data_offset));  // Check 8 byte alignment.
  }
}

TEST_F(ClassLinkerTest, ValidatePrimitiveArrayElementsOffset) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::LongArray> long_array(hs.NewHandle(mirror::LongArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[J"), long_array->GetClass());
  uintptr_t data_offset = reinterpret_cast<uintptr_t>(long_array->GetData());
  EXPECT_TRUE(IsAligned<8>(data_offset));  // Longs require 8 byte alignment

  Handle<mirror::DoubleArray> double_array(hs.NewHandle(mirror::DoubleArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[D"), double_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(double_array->GetData());
  EXPECT_TRUE(IsAligned<8>(data_offset));  // Doubles require 8 byte alignment

  Handle<mirror::IntArray> int_array(hs.NewHandle(mirror::IntArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[I"), int_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(int_array->GetData());
  EXPECT_TRUE(IsAligned<4>(data_offset));  // Ints require 4 byte alignment

  Handle<mirror::CharArray> char_array(hs.NewHandle(mirror::CharArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[C"), char_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(char_array->GetData());
  EXPECT_TRUE(IsAligned<2>(data_offset));  // Chars require 2 byte alignment

  Handle<mirror::ShortArray> short_array(hs.NewHandle(mirror::ShortArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[S"), short_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(short_array->GetData());
  EXPECT_TRUE(IsAligned<2>(data_offset));  // Shorts require 2 byte alignment

  // Take it as given that bytes and booleans have byte alignment
}

TEST_F(ClassLinkerTest, ValidateBoxedTypes) {
  // Validate that the "value" field is always the 0th field in each of java.lang's box classes.
  // This lets UnboxPrimitive avoid searching for the field by name at runtime.
  ScopedObjectAccess soa(Thread::Current());
  ScopedNullHandle<mirror::ClassLoader> class_loader;
  mirror::Class* c;
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Boolean;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Byte;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Character;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Double;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Float;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Integer;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Long;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Short;", class_loader);
  EXPECT_STREQ("value", c->GetIFieldsPtr()->At(0).GetName());
}

TEST_F(ClassLinkerTest, TwoClassLoadersOneClass) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader_1(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("MyClass"))));
  Handle<mirror::ClassLoader> class_loader_2(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("MyClass"))));
  mirror::Class* MyClass_1 = class_linker_->FindClass(soa.Self(), "LMyClass;", class_loader_1);
  mirror::Class* MyClass_2 = class_linker_->FindClass(soa.Self(), "LMyClass;", class_loader_2);
  EXPECT_TRUE(MyClass_1 != nullptr);
  EXPECT_TRUE(MyClass_2 != nullptr);
  EXPECT_NE(MyClass_1, MyClass_2);
}

TEST_F(ClassLinkerTest, StaticFields) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Statics"))));
  Handle<mirror::Class> statics(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LStatics;", class_loader)));
  class_linker_->EnsureInitialized(soa.Self(), statics, true, true);

  // Static final primitives that are initialized by a compile-time constant
  // expression resolve to a copy of a constant value from the constant pool.
  // So <clinit> should be null.
  ArtMethod* clinit = statics->FindDirectMethod("<clinit>", "()V", sizeof(void*));
  EXPECT_TRUE(clinit == nullptr);

  EXPECT_EQ(9U, statics->NumStaticFields());

  ArtField* s0 = mirror::Class::FindStaticField(soa.Self(), statics, "s0", "Z");
  EXPECT_EQ(s0->GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  EXPECT_EQ(true, s0->GetBoolean(statics.Get()));
  s0->SetBoolean<false>(statics.Get(), false);

  ArtField* s1 = mirror::Class::FindStaticField(soa.Self(), statics, "s1", "B");
  EXPECT_EQ(s1->GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  EXPECT_EQ(5, s1->GetByte(statics.Get()));
  s1->SetByte<false>(statics.Get(), 6);

  ArtField* s2 = mirror::Class::FindStaticField(soa.Self(), statics, "s2", "C");
  EXPECT_EQ(s2->GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  EXPECT_EQ('a', s2->GetChar(statics.Get()));
  s2->SetChar<false>(statics.Get(), 'b');

  ArtField* s3 = mirror::Class::FindStaticField(soa.Self(), statics, "s3", "S");
  EXPECT_EQ(s3->GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  EXPECT_EQ(-536, s3->GetShort(statics.Get()));
  s3->SetShort<false>(statics.Get(), -535);

  ArtField* s4 = mirror::Class::FindStaticField(soa.Self(), statics, "s4", "I");
  EXPECT_EQ(s4->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  EXPECT_EQ(2000000000, s4->GetInt(statics.Get()));
  s4->SetInt<false>(statics.Get(), 2000000001);

  ArtField* s5 = mirror::Class::FindStaticField(soa.Self(), statics, "s5", "J");
  EXPECT_EQ(s5->GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  EXPECT_EQ(0x1234567890abcdefLL, s5->GetLong(statics.Get()));
  s5->SetLong<false>(statics.Get(), INT64_C(0x34567890abcdef12));

  ArtField* s6 = mirror::Class::FindStaticField(soa.Self(), statics, "s6", "F");
  EXPECT_EQ(s6->GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  EXPECT_DOUBLE_EQ(0.5, s6->GetFloat(statics.Get()));
  s6->SetFloat<false>(statics.Get(), 0.75);

  ArtField* s7 = mirror::Class::FindStaticField(soa.Self(), statics, "s7", "D");
  EXPECT_EQ(s7->GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  EXPECT_DOUBLE_EQ(16777217.0, s7->GetDouble(statics.Get()));
  s7->SetDouble<false>(statics.Get(), 16777219);

  ArtField* s8 = mirror::Class::FindStaticField(soa.Self(), statics, "s8",
                                                        "Ljava/lang/String;");
  EXPECT_EQ(s8->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  EXPECT_TRUE(s8->GetObject(statics.Get())->AsString()->Equals("android"));
  s8->SetObject<false>(s8->GetDeclaringClass(),
                       mirror::String::AllocFromModifiedUtf8(soa.Self(), "robot"));

  // TODO: Remove EXPECT_FALSE when GCC can handle EXPECT_EQ
  // http://code.google.com/p/googletest/issues/detail?id=322
  EXPECT_FALSE(s0->GetBoolean(statics.Get()));
  EXPECT_EQ(6, s1->GetByte(statics.Get()));
  EXPECT_EQ('b', s2->GetChar(statics.Get()));
  EXPECT_EQ(-535, s3->GetShort(statics.Get()));
  EXPECT_EQ(2000000001, s4->GetInt(statics.Get()));
  EXPECT_EQ(INT64_C(0x34567890abcdef12), s5->GetLong(statics.Get()));
  EXPECT_FLOAT_EQ(0.75, s6->GetFloat(statics.Get()));
  EXPECT_DOUBLE_EQ(16777219.0, s7->GetDouble(statics.Get()));
  EXPECT_TRUE(s8->GetObject(statics.Get())->AsString()->Equals("robot"));
}

TEST_F(ClassLinkerTest, Interfaces) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<6> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Interfaces"))));
  Handle<mirror::Class> I(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LInterfaces$I;", class_loader)));
  Handle<mirror::Class> J(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LInterfaces$J;", class_loader)));
  Handle<mirror::Class> K(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LInterfaces$K;", class_loader)));
  Handle<mirror::Class> A(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LInterfaces$A;", class_loader)));
  Handle<mirror::Class> B(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LInterfaces$B;", class_loader)));
  EXPECT_TRUE(I->IsAssignableFrom(A.Get()));
  EXPECT_TRUE(J->IsAssignableFrom(A.Get()));
  EXPECT_TRUE(J->IsAssignableFrom(K.Get()));
  EXPECT_TRUE(K->IsAssignableFrom(B.Get()));
  EXPECT_TRUE(J->IsAssignableFrom(B.Get()));

  const Signature void_sig = I->GetDexCache()->GetDexFile()->CreateSignature("()V");
  ArtMethod* Ii = I->FindVirtualMethod("i", void_sig, sizeof(void*));
  ArtMethod* Jj1 = J->FindVirtualMethod("j1", void_sig, sizeof(void*));
  ArtMethod* Jj2 = J->FindVirtualMethod("j2", void_sig, sizeof(void*));
  ArtMethod* Kj1 = K->FindInterfaceMethod("j1", void_sig, sizeof(void*));
  ArtMethod* Kj2 = K->FindInterfaceMethod("j2", void_sig, sizeof(void*));
  ArtMethod* Kk = K->FindInterfaceMethod("k", void_sig, sizeof(void*));
  ArtMethod* Ai = A->FindVirtualMethod("i", void_sig, sizeof(void*));
  ArtMethod* Aj1 = A->FindVirtualMethod("j1", void_sig, sizeof(void*));
  ArtMethod* Aj2 = A->FindVirtualMethod("j2", void_sig, sizeof(void*));
  ASSERT_TRUE(Ii != nullptr);
  ASSERT_TRUE(Jj1 != nullptr);
  ASSERT_TRUE(Jj2 != nullptr);
  ASSERT_TRUE(Kj1 != nullptr);
  ASSERT_TRUE(Kj2 != nullptr);
  ASSERT_TRUE(Kk != nullptr);
  ASSERT_TRUE(Ai != nullptr);
  ASSERT_TRUE(Aj1 != nullptr);
  ASSERT_TRUE(Aj2 != nullptr);
  EXPECT_NE(Ii, Ai);
  EXPECT_NE(Jj1, Aj1);
  EXPECT_NE(Jj2, Aj2);
  EXPECT_EQ(Kj1, Jj1);
  EXPECT_EQ(Kj2, Jj2);
  EXPECT_EQ(Ai, A->FindVirtualMethodForInterface(Ii, sizeof(void*)));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForInterface(Jj1, sizeof(void*)));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForInterface(Jj2, sizeof(void*)));
  EXPECT_EQ(Ai, A->FindVirtualMethodForVirtualOrInterface(Ii, sizeof(void*)));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForVirtualOrInterface(Jj1, sizeof(void*)));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForVirtualOrInterface(Jj2, sizeof(void*)));

  ArtField* Afoo = mirror::Class::FindStaticField(soa.Self(), A, "foo", "Ljava/lang/String;");
  ArtField* Bfoo = mirror::Class::FindStaticField(soa.Self(), B, "foo", "Ljava/lang/String;");
  ArtField* Jfoo = mirror::Class::FindStaticField(soa.Self(), J, "foo", "Ljava/lang/String;");
  ArtField* Kfoo = mirror::Class::FindStaticField(soa.Self(), K, "foo", "Ljava/lang/String;");
  ASSERT_TRUE(Afoo != nullptr);
  EXPECT_EQ(Afoo, Bfoo);
  EXPECT_EQ(Afoo, Jfoo);
  EXPECT_EQ(Afoo, Kfoo);
}

TEST_F(ClassLinkerTest, ResolveVerifyAndClinit) {
  // pretend we are trying to get the static storage for the StaticsFromCode class.

  // case 1, get the uninitialized storage from StaticsFromCode.<clinit>
  // case 2, get the initialized storage from StaticsFromCode.getS0

  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("StaticsFromCode");
  const DexFile* dex_file = GetFirstDexFile(jclass_loader);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  mirror::Class* klass = class_linker_->FindClass(soa.Self(), "LStaticsFromCode;", class_loader);
  ArtMethod* clinit = klass->FindClassInitializer(sizeof(void*));
  ArtMethod* getS0 = klass->FindDirectMethod("getS0", "()Ljava/lang/Object;", sizeof(void*));
  const DexFile::TypeId* type_id = dex_file->FindTypeId("LStaticsFromCode;");
  ASSERT_TRUE(type_id != nullptr);
  uint32_t type_idx = dex_file->GetIndexForTypeId(*type_id);
  mirror::Class* uninit = ResolveVerifyAndClinit(type_idx, clinit, soa.Self(), true, false);
  EXPECT_TRUE(uninit != nullptr);
  EXPECT_FALSE(uninit->IsInitialized());
  mirror::Class* init = ResolveVerifyAndClinit(type_idx, getS0, soa.Self(), true, false);
  EXPECT_TRUE(init != nullptr);
  EXPECT_TRUE(init->IsInitialized());
}

TEST_F(ClassLinkerTest, FinalizableBit) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* c;

  // Object has a finalize method, but we know it's empty.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  EXPECT_FALSE(c->IsFinalizable());

  // Enum has a finalize method to prevent its subclasses from implementing one.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Enum;");
  EXPECT_FALSE(c->IsFinalizable());

  // RoundingMode is an enum.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/math/RoundingMode;");
  EXPECT_FALSE(c->IsFinalizable());

  // RandomAccessFile extends Object and overrides finalize.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/RandomAccessFile;");
  EXPECT_TRUE(c->IsFinalizable());

  // FileInputStream is finalizable and extends InputStream which isn't.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/InputStream;");
  EXPECT_FALSE(c->IsFinalizable());
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/FileInputStream;");
  EXPECT_TRUE(c->IsFinalizable());

  // ScheduledThreadPoolExecutor doesn't have a finalize method but
  // extends ThreadPoolExecutor which does.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/util/concurrent/ThreadPoolExecutor;");
  EXPECT_TRUE(c->IsFinalizable());
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/util/concurrent/ScheduledThreadPoolExecutor;");
  EXPECT_TRUE(c->IsFinalizable());
}

TEST_F(ClassLinkerTest, ClassRootDescriptors) {
  ScopedObjectAccess soa(Thread::Current());
  std::string temp;
  for (int i = 0; i < ClassLinker::kClassRootsMax; i++) {
    mirror::Class* klass = class_linker_->GetClassRoot(ClassLinker::ClassRoot(i));
    EXPECT_GT(strlen(klass->GetDescriptor(&temp)), 0U);
    EXPECT_STREQ(klass->GetDescriptor(&temp),
                 class_linker_->GetClassRootDescriptor(ClassLinker::ClassRoot(i))) << " i = " << i;
  }
}

TEST_F(ClassLinkerTest, ValidatePredefinedClassSizes) {
  ScopedObjectAccess soa(Thread::Current());
  ScopedNullHandle<mirror::ClassLoader> class_loader;
  mirror::Class* c;

  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Class;", class_loader);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->GetClassSize(), mirror::Class::ClassClassSize(sizeof(void*)));

  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Object;", class_loader);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->GetClassSize(), mirror::Object::ClassSize(sizeof(void*)));

  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/String;", class_loader);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->GetClassSize(), mirror::String::ClassSize(sizeof(void*)));

  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/DexCache;", class_loader);
  ASSERT_TRUE(c != nullptr);
  EXPECT_EQ(c->GetClassSize(), mirror::DexCache::ClassSize(sizeof(void*)));
}

static void CheckMethod(ArtMethod* method, bool verified)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (!method->IsNative() && !method->IsAbstract()) {
    EXPECT_EQ((method->GetAccessFlags() & kAccSkipAccessChecks) != 0U, verified)
        << PrettyMethod(method, true);
  }
}

static void CheckVerificationAttempted(mirror::Class* c, bool preverified)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  EXPECT_EQ((c->GetAccessFlags() & kAccVerificationAttempted) != 0U, preverified)
      << "Class " << PrettyClass(c) << " not as expected";
  for (auto& m : c->GetMethods(sizeof(void*))) {
    CheckMethod(&m, preverified);
  }
}

TEST_F(ClassLinkerTest, Preverified_InitializedBoot) {
  ScopedObjectAccess soa(Thread::Current());

  mirror::Class* JavaLangObject = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  ASSERT_TRUE(JavaLangObject != nullptr);
  EXPECT_TRUE(JavaLangObject->IsInitialized()) << "Not testing already initialized class from the "
                                                  "core";
  CheckVerificationAttempted(JavaLangObject, true);
}

TEST_F(ClassLinkerTest, Preverified_UninitializedBoot) {
  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<1> hs(soa.Self());

  Handle<mirror::Class> security_manager(hs.NewHandle(class_linker_->FindSystemClass(
      soa.Self(), "Ljava/lang/SecurityManager;")));
  EXPECT_FALSE(security_manager->IsInitialized()) << "Not testing uninitialized class from the "
                                                     "core";

  CheckVerificationAttempted(security_manager.Get(), false);

  class_linker_->EnsureInitialized(soa.Self(), security_manager, true, true);
  CheckVerificationAttempted(security_manager.Get(), true);
}

TEST_F(ClassLinkerTest, Preverified_App) {
  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Statics"))));
  Handle<mirror::Class> statics(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LStatics;", class_loader)));

  CheckVerificationAttempted(statics.Get(), false);

  class_linker_->EnsureInitialized(soa.Self(), statics, true, true);
  CheckVerificationAttempted(statics.Get(), true);
}

TEST_F(ClassLinkerTest, IsBootStrapClassLoaded) {
  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Statics"))));

  // java.lang.Object is a bootstrap class.
  Handle<mirror::Class> jlo_class(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(jlo_class.Get() != nullptr);
  EXPECT_TRUE(jlo_class.Get()->IsBootStrapClassLoaded());

  // Statics is not a bootstrap class.
  Handle<mirror::Class> statics(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LStatics;", class_loader)));
  ASSERT_TRUE(statics.Get() != nullptr);
  EXPECT_FALSE(statics.Get()->IsBootStrapClassLoaded());
}

// Regression test for b/26799552.
TEST_F(ClassLinkerTest, RegisterDexFileName) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  MutableHandle<mirror::DexCache> dex_cache(hs.NewHandle<mirror::DexCache>(nullptr));
  {
    ReaderMutexLock mu(soa.Self(), *class_linker->DexLock());
    for (const ClassLinker::DexCacheData& data : class_linker->GetDexCachesData()) {
      dex_cache.Assign(down_cast<mirror::DexCache*>(soa.Self()->DecodeJObject(data.weak_root)));
      if (dex_cache.Get() != nullptr) {
        break;
      }
    }
    ASSERT_TRUE(dex_cache.Get() != nullptr);
  }
  // Make a copy of the dex cache and change the name.
  dex_cache.Assign(dex_cache->Clone(soa.Self())->AsDexCache());
  const uint16_t data[] = { 0x20AC, 0x20A1 };
  Handle<mirror::String> location(hs.NewHandle(mirror::String::AllocFromUtf16(soa.Self(),
                                                                              arraysize(data),
                                                                              data)));
  dex_cache->SetLocation(location.Get());
  const DexFile* old_dex_file = dex_cache->GetDexFile();

  std::unique_ptr<DexFile> dex_file(new DexFile(old_dex_file->Begin(),
                                                old_dex_file->Size(),
                                                location->ToModifiedUtf8(),
                                                0u,
                                                nullptr,
                                                nullptr));
  {
    WriterMutexLock mu(soa.Self(), *class_linker->DexLock());
    // Check that inserting with a UTF16 name works.
    class_linker->RegisterDexFileLocked(*dex_file, dex_cache);
  }
}

}  // namespace art
