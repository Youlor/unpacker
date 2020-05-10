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

#include "object.h"

#include <stdint.h>
#include <stdio.h>
#include <memory>

#include "array-inl.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "asm_support.h"
#include "class-inl.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "handle_scope-inl.h"
#include "iftable-inl.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "scoped_thread_state_change.h"
#include "string-inl.h"

namespace art {
namespace mirror {

class ObjectTest : public CommonRuntimeTest {
 protected:
  void AssertString(int32_t expected_utf16_length,
                    const char* utf8_in,
                    const char* utf16_expected_le,
                    int32_t expected_hash)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::unique_ptr<uint16_t[]> utf16_expected(new uint16_t[expected_utf16_length]);
    for (int32_t i = 0; i < expected_utf16_length; i++) {
      uint16_t ch = (((utf16_expected_le[i*2 + 0] & 0xff) << 8) |
                     ((utf16_expected_le[i*2 + 1] & 0xff) << 0));
      utf16_expected[i] = ch;
    }

    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    Handle<String> string(
        hs.NewHandle(String::AllocFromModifiedUtf8(self, expected_utf16_length, utf8_in)));
    ASSERT_EQ(expected_utf16_length, string->GetLength());
    ASSERT_TRUE(string->GetValue() != nullptr);
    // strlen is necessary because the 1-character string "\x00\x00" is interpreted as ""
    ASSERT_TRUE(string->Equals(utf8_in) || (expected_utf16_length == 1 && strlen(utf8_in) == 0));
    ASSERT_TRUE(string->Equals(StringPiece(utf8_in)) ||
                (expected_utf16_length == 1 && strlen(utf8_in) == 0));
    for (int32_t i = 0; i < expected_utf16_length; i++) {
      EXPECT_EQ(utf16_expected[i], string->CharAt(i));
    }
    EXPECT_EQ(expected_hash, string->GetHashCode());
  }
};

// Keep constants in sync.
TEST_F(ObjectTest, Constants) {
  EXPECT_EQ(kObjectReferenceSize, sizeof(HeapReference<Object>));
  EXPECT_EQ(kObjectHeaderSize, sizeof(Object));
  EXPECT_EQ(ART_METHOD_QUICK_CODE_OFFSET_32,
            ArtMethod::EntryPointFromQuickCompiledCodeOffset(4).Int32Value());
  EXPECT_EQ(ART_METHOD_QUICK_CODE_OFFSET_64,
            ArtMethod::EntryPointFromQuickCompiledCodeOffset(8).Int32Value());
}

TEST_F(ObjectTest, IsInSamePackage) {
  // Matches
  EXPECT_TRUE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/Class;"));
  EXPECT_TRUE(Class::IsInSamePackage("LFoo;", "LBar;"));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/io/File;"));
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/reflect/Method;"));
}

TEST_F(ObjectTest, Clone) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<ObjectArray<Object>> a1(
      hs.NewHandle(class_linker_->AllocObjectArray<Object>(soa.Self(), 256)));
  size_t s1 = a1->SizeOf();
  Object* clone = a1->Clone(soa.Self());
  EXPECT_EQ(s1, clone->SizeOf());
  EXPECT_TRUE(clone->GetClass() == a1->GetClass());
}

TEST_F(ObjectTest, AllocObjectArray) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<ObjectArray<Object>> oa(
      hs.NewHandle(class_linker_->AllocObjectArray<Object>(soa.Self(), 2)));
  EXPECT_EQ(2, oa->GetLength());
  EXPECT_TRUE(oa->Get(0) == nullptr);
  EXPECT_TRUE(oa->Get(1) == nullptr);
  oa->Set<false>(0, oa.Get());
  EXPECT_TRUE(oa->Get(0) == oa.Get());
  EXPECT_TRUE(oa->Get(1) == nullptr);
  oa->Set<false>(1, oa.Get());
  EXPECT_TRUE(oa->Get(0) == oa.Get());
  EXPECT_TRUE(oa->Get(1) == oa.Get());

  Class* aioobe = class_linker_->FindSystemClass(soa.Self(),
                                                 "Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_TRUE(oa->Get(-1) == nullptr);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_TRUE(oa->Get(2) == nullptr);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  ASSERT_TRUE(oa->GetClass() != nullptr);
  Handle<mirror::Class> klass(hs.NewHandle(oa->GetClass()));
  ASSERT_EQ(2U, klass->NumDirectInterfaces());
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Cloneable;"),
            mirror::Class::GetDirectInterface(soa.Self(), klass, 0));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;"),
            mirror::Class::GetDirectInterface(soa.Self(), klass, 1));
}

TEST_F(ObjectTest, AllocArray) {
  ScopedObjectAccess soa(Thread::Current());
  Class* c = class_linker_->FindSystemClass(soa.Self(), "[I");
  StackHandleScope<1> hs(soa.Self());
  MutableHandle<Array> a(
      hs.NewHandle(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSizeShift(),
                                      Runtime::Current()->GetHeap()->GetCurrentAllocator())));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_EQ(1, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSizeShift(),
                              Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_EQ(1, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSizeShift(),
                              Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_EQ(1, a->GetLength());
}

TEST_F(ObjectTest, AllocArray_FillUsable) {
  ScopedObjectAccess soa(Thread::Current());
  Class* c = class_linker_->FindSystemClass(soa.Self(), "[B");
  StackHandleScope<1> hs(soa.Self());
  MutableHandle<Array> a(
      hs.NewHandle(Array::Alloc<true, true>(soa.Self(), c, 1, c->GetComponentSizeShift(),
                                            Runtime::Current()->GetHeap()->GetCurrentAllocator())));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(1, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[I");
  a.Assign(Array::Alloc<true, true>(soa.Self(), c, 2, c->GetComponentSizeShift(),
                                    Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(2, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true, true>(soa.Self(), c, 2, c->GetComponentSizeShift(),
                                    Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(2, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true, true>(soa.Self(), c, 2, c->GetComponentSizeShift(),
                                    Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(2, a->GetLength());
}

template<typename ArrayT>
void TestPrimitiveArray(ClassLinker* cl) {
  ScopedObjectAccess soa(Thread::Current());
  typedef typename ArrayT::ElementType T;

  ArrayT* a = ArrayT::Alloc(soa.Self(), 2);
  EXPECT_EQ(2, a->GetLength());
  EXPECT_EQ(0, a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(T(321), a->Get(1));

  Class* aioobe = cl->FindSystemClass(soa.Self(), "Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();
}

TEST_F(ObjectTest, PrimitiveArray_Boolean_Alloc) {
  TestPrimitiveArray<BooleanArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Byte_Alloc) {
  TestPrimitiveArray<ByteArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Char_Alloc) {
  TestPrimitiveArray<CharArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Int_Alloc) {
  TestPrimitiveArray<IntArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Long_Alloc) {
  TestPrimitiveArray<LongArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Short_Alloc) {
  TestPrimitiveArray<ShortArray>(class_linker_);
}

TEST_F(ObjectTest, PrimitiveArray_Double_Alloc) {
  typedef DoubleArray ArrayT;
  ScopedObjectAccess soa(Thread::Current());
  typedef typename ArrayT::ElementType T;

  ArrayT* a = ArrayT::Alloc(soa.Self(), 2);
  EXPECT_EQ(2, a->GetLength());
  EXPECT_DOUBLE_EQ(0, a->Get(0));
  EXPECT_DOUBLE_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_DOUBLE_EQ(T(123), a->Get(0));
  EXPECT_DOUBLE_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_DOUBLE_EQ(T(123), a->Get(0));
  EXPECT_DOUBLE_EQ(T(321), a->Get(1));

  Class* aioobe = class_linker_->FindSystemClass(soa.Self(),
                                                 "Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_DOUBLE_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_DOUBLE_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();
}

TEST_F(ObjectTest, PrimitiveArray_Float_Alloc) {
  typedef FloatArray ArrayT;
  ScopedObjectAccess soa(Thread::Current());
  typedef typename ArrayT::ElementType T;

  ArrayT* a = ArrayT::Alloc(soa.Self(), 2);
  EXPECT_FLOAT_EQ(2, a->GetLength());
  EXPECT_FLOAT_EQ(0, a->Get(0));
  EXPECT_FLOAT_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_FLOAT_EQ(T(123), a->Get(0));
  EXPECT_FLOAT_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_FLOAT_EQ(T(123), a->Get(0));
  EXPECT_FLOAT_EQ(T(321), a->Get(1));

  Class* aioobe = class_linker_->FindSystemClass(soa.Self(),
                                                 "Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_FLOAT_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_FLOAT_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();
}


TEST_F(ObjectTest, CheckAndAllocArrayFromCode) {
  // pretend we are trying to call 'new char[3]' from String.toCharArray
  ScopedObjectAccess soa(Thread::Current());
  Class* java_util_Arrays = class_linker_->FindSystemClass(soa.Self(), "Ljava/util/Arrays;");
  ArtMethod* sort = java_util_Arrays->FindDirectMethod("sort", "([I)V", sizeof(void*));
  const DexFile::TypeId* type_id = java_lang_dex_file_->FindTypeId("[I");
  ASSERT_TRUE(type_id != nullptr);
  uint32_t type_idx = java_lang_dex_file_->GetIndexForTypeId(*type_id);
  Object* array = CheckAndAllocArrayFromCodeInstrumented(
      type_idx, 3, sort, Thread::Current(), false,
      Runtime::Current()->GetHeap()->GetCurrentAllocator());
  EXPECT_TRUE(array->IsArrayInstance());
  EXPECT_EQ(3, array->AsArray()->GetLength());
  EXPECT_TRUE(array->GetClass()->IsArrayClass());
  EXPECT_TRUE(array->GetClass()->GetComponentType()->IsPrimitive());
}

TEST_F(ObjectTest, CreateMultiArray) {
  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<2> hs(soa.Self());
  Handle<Class> c(hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "I")));
  MutableHandle<IntArray> dims(hs.NewHandle(IntArray::Alloc(soa.Self(), 1)));
  dims->Set<false>(0, 1);
  Array* multi = Array::CreateMultiArray(soa.Self(), c, dims);
  EXPECT_TRUE(multi->GetClass() == class_linker_->FindSystemClass(soa.Self(), "[I"));
  EXPECT_EQ(1, multi->GetLength());

  dims->Set<false>(0, -1);
  multi = Array::CreateMultiArray(soa.Self(), c, dims);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(PrettyDescriptor(soa.Self()->GetException()->GetClass()),
            "java.lang.NegativeArraySizeException");
  soa.Self()->ClearException();

  dims.Assign(IntArray::Alloc(soa.Self(), 2));
  for (int i = 1; i < 20; ++i) {
    for (int j = 0; j < 20; ++j) {
      dims->Set<false>(0, i);
      dims->Set<false>(1, j);
      multi = Array::CreateMultiArray(soa.Self(), c, dims);
      EXPECT_TRUE(multi->GetClass() == class_linker_->FindSystemClass(soa.Self(), "[[I"));
      EXPECT_EQ(i, multi->GetLength());
      for (int k = 0; k < i; ++k) {
        Array* outer = multi->AsObjectArray<Array>()->Get(k);
        EXPECT_TRUE(outer->GetClass() == class_linker_->FindSystemClass(soa.Self(), "[I"));
        EXPECT_EQ(j, outer->GetLength());
      }
    }
  }
}

TEST_F(ObjectTest, StaticFieldFromCode) {
  // pretend we are trying to access 'Static.s0' from StaticsFromCode.<clinit>
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("StaticsFromCode");
  const DexFile* dex_file = GetFirstDexFile(class_loader);

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<ClassLoader*>(class_loader)));
  Class* klass = class_linker_->FindClass(soa.Self(), "LStaticsFromCode;", loader);
  ArtMethod* clinit = klass->FindClassInitializer(sizeof(void*));
  const DexFile::TypeId* klass_type_id = dex_file->FindTypeId("LStaticsFromCode;");
  ASSERT_TRUE(klass_type_id != nullptr);

  const DexFile::TypeId* type_type_id = dex_file->FindTypeId("Ljava/lang/Object;");
  ASSERT_TRUE(type_type_id != nullptr);

  const DexFile::StringId* name_str_id = dex_file->FindStringId("s0");
  ASSERT_TRUE(name_str_id != nullptr);

  const DexFile::FieldId* field_id = dex_file->FindFieldId(
      *klass_type_id, *name_str_id, *type_type_id);
  ASSERT_TRUE(field_id != nullptr);
  uint32_t field_idx = dex_file->GetIndexForFieldId(*field_id);

  ArtField* field = FindFieldFromCode<StaticObjectRead, true>(field_idx, clinit, Thread::Current(),
                                                              sizeof(HeapReference<Object>));
  Object* s0 = field->GetObj(klass);
  EXPECT_TRUE(s0 != nullptr);

  Handle<CharArray> char_array(hs.NewHandle(CharArray::Alloc(soa.Self(), 0)));
  field->SetObj<false>(field->GetDeclaringClass(), char_array.Get());
  EXPECT_EQ(char_array.Get(), field->GetObj(klass));

  field->SetObj<false>(field->GetDeclaringClass(), nullptr);
  EXPECT_EQ(nullptr, field->GetObj(klass));

  // TODO: more exhaustive tests of all 6 cases of ArtField::*FromCode
}

TEST_F(ObjectTest, String) {
  ScopedObjectAccess soa(Thread::Current());
  // Test the empty string.
  AssertString(0, "",     "", 0);

  // Test one-byte characters.
  AssertString(1, " ",    "\x00\x20",         0x20);
  AssertString(1, "",     "\x00\x00",         0);
  AssertString(1, "\x7f", "\x00\x7f",         0x7f);
  AssertString(2, "hi",   "\x00\x68\x00\x69", (31 * 0x68) + 0x69);

  // Test two-byte characters.
  AssertString(1, "\xc2\x80",   "\x00\x80",                 0x80);
  AssertString(1, "\xd9\xa6",   "\x06\x66",                 0x0666);
  AssertString(1, "\xdf\xbf",   "\x07\xff",                 0x07ff);
  AssertString(3, "h\xd9\xa6i", "\x00\x68\x06\x66\x00\x69",
               (31 * ((31 * 0x68) + 0x0666)) + 0x69);

  // Test three-byte characters.
  AssertString(1, "\xe0\xa0\x80",   "\x08\x00",                 0x0800);
  AssertString(1, "\xe1\x88\xb4",   "\x12\x34",                 0x1234);
  AssertString(1, "\xef\xbf\xbf",   "\xff\xff",                 0xffff);
  AssertString(3, "h\xe1\x88\xb4i", "\x00\x68\x12\x34\x00\x69",
               (31 * ((31 * 0x68) + 0x1234)) + 0x69);

  // Test four-byte characters.
  AssertString(2, "\xf0\x9f\x8f\xa0",  "\xd8\x3c\xdf\xe0", (31 * 0xd83c) + 0xdfe0);
  AssertString(2, "\xf0\x9f\x9a\x80",  "\xd8\x3d\xde\x80", (31 * 0xd83d) + 0xde80);
  AssertString(4, "h\xf0\x9f\x9a\x80i", "\x00\x68\xd8\x3d\xde\x80\x00\x69",
               (31 * (31 * (31 * 0x68 +  0xd83d) + 0xde80) + 0x69));
}

TEST_F(ObjectTest, StringEqualsUtf8) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_TRUE(string->Equals("android"));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringEquals) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_2(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_TRUE(string->Equals(string_2.Get()));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringCompareTo) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_2(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_3(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "Android")));
  Handle<String> string_4(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "and")));
  Handle<String> string_5(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_EQ(0, string->CompareTo(string_2.Get()));
  EXPECT_LT(0, string->CompareTo(string_3.Get()));
  EXPECT_GT(0, string_3->CompareTo(string.Get()));
  EXPECT_LT(0, string->CompareTo(string_4.Get()));
  EXPECT_GT(0, string_4->CompareTo(string.Get()));
  EXPECT_LT(0, string->CompareTo(string_5.Get()));
  EXPECT_GT(0, string_5->CompareTo(string.Get()));
}

TEST_F(ObjectTest, StringLength) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_EQ(string->GetLength(), 7);
  EXPECT_EQ(string->GetUtfLength(), 7);
}

TEST_F(ObjectTest, DescriptorCompare) {
  // Two classloaders conflicts in compile_time_class_paths_.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* linker = class_linker_;

  jobject jclass_loader_1 = LoadDex("ProtoCompare");
  jobject jclass_loader_2 = LoadDex("ProtoCompare2");
  StackHandleScope<4> hs(soa.Self());
  Handle<ClassLoader> class_loader_1(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader_1)));
  Handle<ClassLoader> class_loader_2(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader_2)));

  Class* klass1 = linker->FindClass(soa.Self(), "LProtoCompare;", class_loader_1);
  ASSERT_TRUE(klass1 != nullptr);
  Class* klass2 = linker->FindClass(soa.Self(), "LProtoCompare2;", class_loader_2);
  ASSERT_TRUE(klass2 != nullptr);

  ArtMethod* m1_1 = klass1->GetVirtualMethod(0, sizeof(void*));
  EXPECT_STREQ(m1_1->GetName(), "m1");
  ArtMethod* m2_1 = klass1->GetVirtualMethod(1, sizeof(void*));
  EXPECT_STREQ(m2_1->GetName(), "m2");
  ArtMethod* m3_1 = klass1->GetVirtualMethod(2, sizeof(void*));
  EXPECT_STREQ(m3_1->GetName(), "m3");
  ArtMethod* m4_1 = klass1->GetVirtualMethod(3, sizeof(void*));
  EXPECT_STREQ(m4_1->GetName(), "m4");

  ArtMethod* m1_2 = klass2->GetVirtualMethod(0, sizeof(void*));
  EXPECT_STREQ(m1_2->GetName(), "m1");
  ArtMethod* m2_2 = klass2->GetVirtualMethod(1, sizeof(void*));
  EXPECT_STREQ(m2_2->GetName(), "m2");
  ArtMethod* m3_2 = klass2->GetVirtualMethod(2, sizeof(void*));
  EXPECT_STREQ(m3_2->GetName(), "m3");
  ArtMethod* m4_2 = klass2->GetVirtualMethod(3, sizeof(void*));
  EXPECT_STREQ(m4_2->GetName(), "m4");
}

TEST_F(ObjectTest, StringHashCode) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  Handle<String> A(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "A")));
  Handle<String> ABC(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));

  EXPECT_EQ(0, empty->GetHashCode());
  EXPECT_EQ(65, A->GetHashCode());
  EXPECT_EQ(64578, ABC->GetHashCode());
}

TEST_F(ObjectTest, InstanceOf) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<3> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader)));

  Class* X = class_linker_->FindClass(soa.Self(), "LX;", class_loader);
  Class* Y = class_linker_->FindClass(soa.Self(), "LY;", class_loader);
  ASSERT_TRUE(X != nullptr);
  ASSERT_TRUE(Y != nullptr);

  Handle<Object> x(hs.NewHandle(X->AllocObject(soa.Self())));
  Handle<Object> y(hs.NewHandle(Y->AllocObject(soa.Self())));
  ASSERT_TRUE(x.Get() != nullptr);
  ASSERT_TRUE(y.Get() != nullptr);

  EXPECT_TRUE(x->InstanceOf(X));
  EXPECT_FALSE(x->InstanceOf(Y));
  EXPECT_TRUE(y->InstanceOf(X));
  EXPECT_TRUE(y->InstanceOf(Y));

  Class* java_lang_Class = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Class;");
  Class* Object_array_class = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");

  EXPECT_FALSE(java_lang_Class->InstanceOf(Object_array_class));
  EXPECT_TRUE(Object_array_class->InstanceOf(java_lang_Class));

  // All array classes implement Cloneable and Serializable.
  Object* array = ObjectArray<Object>::Alloc(soa.Self(), Object_array_class, 1);
  Class* java_lang_Cloneable =
      class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Cloneable;");
  Class* java_io_Serializable =
      class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;");
  EXPECT_TRUE(array->InstanceOf(java_lang_Cloneable));
  EXPECT_TRUE(array->InstanceOf(java_io_Serializable));
}

TEST_F(ObjectTest, IsAssignableFrom) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<1> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader)));
  Class* X = class_linker_->FindClass(soa.Self(), "LX;", class_loader);
  Class* Y = class_linker_->FindClass(soa.Self(), "LY;", class_loader);

  EXPECT_TRUE(X->IsAssignableFrom(X));
  EXPECT_TRUE(X->IsAssignableFrom(Y));
  EXPECT_FALSE(Y->IsAssignableFrom(X));
  EXPECT_TRUE(Y->IsAssignableFrom(Y));

  // class final String implements CharSequence, ..
  Class* string = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/String;");
  Class* charseq = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/CharSequence;");
  // Can String be assigned to CharSequence without a cast?
  EXPECT_TRUE(charseq->IsAssignableFrom(string));
  // Can CharSequence be assigned to String without a cast?
  EXPECT_FALSE(string->IsAssignableFrom(charseq));

  // Primitive types are only assignable to themselves
  const char* prims = "ZBCSIJFD";
  Class* prim_types[strlen(prims)];
  for (size_t i = 0; i < strlen(prims); i++) {
    prim_types[i] = class_linker_->FindPrimitiveClass(prims[i]);
  }
  for (size_t i = 0; i < strlen(prims); i++) {
    for (size_t j = 0; i < strlen(prims); i++) {
      if (i == j) {
        EXPECT_TRUE(prim_types[i]->IsAssignableFrom(prim_types[j]));
      } else {
        EXPECT_FALSE(prim_types[i]->IsAssignableFrom(prim_types[j]));
      }
    }
  }
}

TEST_F(ObjectTest, IsAssignableFromArray) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<1> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader)));
  Class* X = class_linker_->FindClass(soa.Self(), "LX;", class_loader);
  Class* Y = class_linker_->FindClass(soa.Self(), "LY;", class_loader);
  ASSERT_TRUE(X != nullptr);
  ASSERT_TRUE(Y != nullptr);

  Class* YA = class_linker_->FindClass(soa.Self(), "[LY;", class_loader);
  Class* YAA = class_linker_->FindClass(soa.Self(), "[[LY;", class_loader);
  ASSERT_TRUE(YA != nullptr);
  ASSERT_TRUE(YAA != nullptr);

  Class* XAA = class_linker_->FindClass(soa.Self(), "[[LX;", class_loader);
  ASSERT_TRUE(XAA != nullptr);

  Class* O = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  Class* OA = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");
  Class* OAA = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;");
  Class* OAAA = class_linker_->FindSystemClass(soa.Self(), "[[[Ljava/lang/Object;");
  ASSERT_TRUE(O != nullptr);
  ASSERT_TRUE(OA != nullptr);
  ASSERT_TRUE(OAA != nullptr);
  ASSERT_TRUE(OAAA != nullptr);

  Class* S = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;");
  Class* SA = class_linker_->FindSystemClass(soa.Self(), "[Ljava/io/Serializable;");
  Class* SAA = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/io/Serializable;");
  ASSERT_TRUE(S != nullptr);
  ASSERT_TRUE(SA != nullptr);
  ASSERT_TRUE(SAA != nullptr);

  Class* IA = class_linker_->FindSystemClass(soa.Self(), "[I");
  ASSERT_TRUE(IA != nullptr);

  EXPECT_TRUE(YAA->IsAssignableFrom(YAA));  // identity
  EXPECT_TRUE(XAA->IsAssignableFrom(YAA));  // element superclass
  EXPECT_FALSE(YAA->IsAssignableFrom(XAA));
  EXPECT_FALSE(Y->IsAssignableFrom(YAA));
  EXPECT_FALSE(YA->IsAssignableFrom(YAA));
  EXPECT_TRUE(O->IsAssignableFrom(YAA));  // everything is an Object
  EXPECT_TRUE(OA->IsAssignableFrom(YAA));
  EXPECT_TRUE(OAA->IsAssignableFrom(YAA));
  EXPECT_TRUE(S->IsAssignableFrom(YAA));  // all arrays are Serializable
  EXPECT_TRUE(SA->IsAssignableFrom(YAA));
  EXPECT_FALSE(SAA->IsAssignableFrom(YAA));  // unless Y was Serializable

  EXPECT_FALSE(IA->IsAssignableFrom(OA));
  EXPECT_FALSE(OA->IsAssignableFrom(IA));
  EXPECT_TRUE(O->IsAssignableFrom(IA));
}

TEST_F(ObjectTest, FindInstanceField) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> s(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));
  ASSERT_TRUE(s.Get() != nullptr);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != nullptr);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "J") == nullptr);
  EXPECT_TRUE(c->FindInstanceField("count", "J") == nullptr);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredInstanceField("Count", "I") == nullptr);
  EXPECT_TRUE(c->FindInstanceField("Count", "I") == nullptr);

  // Right name and type.
  ArtField* f1 = c->FindDeclaredInstanceField("count", "I");
  ArtField* f2 = c->FindInstanceField("count", "I");
  EXPECT_TRUE(f1 != nullptr);
  EXPECT_TRUE(f2 != nullptr);
  EXPECT_EQ(f1, f2);

  // TODO: check that s.count == 3.

  // Ensure that we handle superclass fields correctly...
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/StringBuilder;");
  ASSERT_TRUE(c != nullptr);
  // No StringBuilder.count...
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "I") == nullptr);
  // ...but there is an AbstractStringBuilder.count.
  EXPECT_TRUE(c->FindInstanceField("count", "I") != nullptr);
}

TEST_F(ObjectTest, FindStaticField) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<String> s(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));
  ASSERT_TRUE(s.Get() != nullptr);
  Handle<Class> c(hs.NewHandle(s->GetClass()));
  ASSERT_TRUE(c.Get() != nullptr);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "I") == nullptr);
  EXPECT_TRUE(mirror::Class::FindStaticField(
      soa.Self(), c, "CASE_INSENSITIVE_ORDER", "I") == nullptr);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredStaticField(
      "cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == nullptr);
  EXPECT_TRUE(
      mirror::Class::FindStaticField(soa.Self(), c, "cASE_INSENSITIVE_ORDER",
                                     "Ljava/util/Comparator;") == nullptr);

  // Right name and type.
  ArtField* f1 = c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  ArtField* f2 = mirror::Class::FindStaticField(soa.Self(), c, "CASE_INSENSITIVE_ORDER",
                                                "Ljava/util/Comparator;");
  EXPECT_TRUE(f1 != nullptr);
  EXPECT_TRUE(f2 != nullptr);
  EXPECT_EQ(f1, f2);

  // TODO: test static fields via superclasses.
  // TODO: test static fields via interfaces.
  // TODO: test that interfaces trump superclasses.
}

TEST_F(ObjectTest, IdentityHashCode) {
  // Regression test for b/19046417 which had an infinite loop if the
  // (seed & LockWord::kHashMask) == 0. seed 0 triggered the infinite loop since we did the check
  // before the CAS which resulted in the same seed the next loop iteration.
  mirror::Object::SetHashCodeSeed(0);
  int32_t hash_code = mirror::Object::GenerateIdentityHashCode();
  EXPECT_NE(hash_code, 0);
}

}  // namespace mirror
}  // namespace art
