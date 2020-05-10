/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "transaction.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "mirror/array-inl.h"
#include "scoped_thread_state_change.h"

namespace art {

class TransactionTest : public CommonRuntimeTest {
 public:
  // Tests failing class initialization due to native call with transaction rollback.
  void testTransactionAbort(const char* tested_class_signature) {
    ScopedObjectAccess soa(Thread::Current());
    jobject jclass_loader = LoadDex("Transaction");
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
    ASSERT_TRUE(class_loader.Get() != nullptr);

    // Load and initialize java.lang.ExceptionInInitializerError and the exception class used
    // to abort transaction so they can be thrown during class initialization if the transaction
    // aborts.
    MutableHandle<mirror::Class> h_klass(
        hs.NewHandle(class_linker_->FindSystemClass(soa.Self(),
                                                    "Ljava/lang/ExceptionInInitializerError;")));
    ASSERT_TRUE(h_klass.Get() != nullptr);
    class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
    ASSERT_TRUE(h_klass->IsInitialized());

    h_klass.Assign(class_linker_->FindSystemClass(soa.Self(),
                                                  Transaction::kAbortExceptionSignature));
    ASSERT_TRUE(h_klass.Get() != nullptr);
    class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
    ASSERT_TRUE(h_klass->IsInitialized());

    // Load and verify utility class.
    h_klass.Assign(class_linker_->FindClass(soa.Self(), "LTransaction$AbortHelperClass;",
                                            class_loader));
    ASSERT_TRUE(h_klass.Get() != nullptr);
    class_linker_->VerifyClass(soa.Self(), h_klass);
    ASSERT_TRUE(h_klass->IsVerified());

    // Load and verify tested class.
    h_klass.Assign(class_linker_->FindClass(soa.Self(), tested_class_signature, class_loader));
    ASSERT_TRUE(h_klass.Get() != nullptr);
    class_linker_->VerifyClass(soa.Self(), h_klass);
    ASSERT_TRUE(h_klass->IsVerified());

    mirror::Class::Status old_status = h_klass->GetStatus();
    LockWord old_lock_word = h_klass->GetLockWord(false);

    Transaction transaction;
    Runtime::Current()->EnterTransactionMode(&transaction);
    bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
    Runtime::Current()->ExitTransactionMode();
    ASSERT_FALSE(success);
    ASSERT_TRUE(h_klass->IsErroneous());
    ASSERT_TRUE(soa.Self()->IsExceptionPending());
    ASSERT_TRUE(transaction.IsAborted());

    // Check class's monitor get back to its original state without rolling back changes.
    LockWord new_lock_word = h_klass->GetLockWord(false);
    EXPECT_TRUE(LockWord::Equal<false>(old_lock_word, new_lock_word));

    // Check class status is rolled back properly.
    soa.Self()->ClearException();
    transaction.Rollback();
    ASSERT_EQ(old_status, h_klass->GetStatus());
  }
};

// Tests object's class is preserved after transaction rollback.
TEST_F(TransactionTest, Object_class) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(h_klass.Get() != nullptr);

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj.Get() != nullptr);
  ASSERT_EQ(h_obj->GetClass(), h_klass.Get());
  Runtime::Current()->ExitTransactionMode();

  // Rolling back transaction's changes must not clear the Object::class field.
  transaction.Rollback();
  EXPECT_EQ(h_obj->GetClass(), h_klass.Get());
}

// Tests object's monitor state is preserved after transaction rollback.
TEST_F(TransactionTest, Object_monitor) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(h_klass.Get() != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj.Get() != nullptr);
  ASSERT_EQ(h_obj->GetClass(), h_klass.Get());

  // Lock object's monitor outside the transaction.
  h_obj->MonitorEnter(soa.Self());
  LockWord old_lock_word = h_obj->GetLockWord(false);

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  // Unlock object's monitor inside the transaction.
  h_obj->MonitorExit(soa.Self());
  LockWord new_lock_word = h_obj->GetLockWord(false);
  Runtime::Current()->ExitTransactionMode();

  // Rolling back transaction's changes must not change monitor's state.
  transaction.Rollback();
  LockWord aborted_lock_word = h_obj->GetLockWord(false);
  EXPECT_FALSE(LockWord::Equal<false>(old_lock_word, new_lock_word));
  EXPECT_TRUE(LockWord::Equal<false>(aborted_lock_word, new_lock_word));
}

// Tests array's length is preserved after transaction rollback.
TEST_F(TransactionTest, Array_length) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));
  ASSERT_TRUE(h_klass.Get() != nullptr);

  constexpr int32_t kArraySize = 2;

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);

  // Allocate an array during transaction.
  Handle<mirror::Array> h_obj(
      hs.NewHandle(
          mirror::Array::Alloc<true>(soa.Self(), h_klass.Get(), kArraySize,
                                     h_klass->GetComponentSizeShift(),
                                     Runtime::Current()->GetHeap()->GetCurrentAllocator())));
  ASSERT_TRUE(h_obj.Get() != nullptr);
  ASSERT_EQ(h_obj->GetClass(), h_klass.Get());
  Runtime::Current()->ExitTransactionMode();

  // Rolling back transaction's changes must not reset array's length.
  transaction.Rollback();
  EXPECT_EQ(h_obj->GetLength(), kArraySize);
}

// Tests static fields are reset to their default value after transaction rollback.
TEST_F(TransactionTest, StaticFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader.Get() != nullptr);

  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LStaticFieldsTest;", class_loader)));
  ASSERT_TRUE(h_klass.Get() != nullptr);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());

  // Lookup fields.
  ArtField* booleanField = h_klass->FindDeclaredStaticField("booleanField", "Z");
  ASSERT_TRUE(booleanField != nullptr);
  ASSERT_EQ(booleanField->GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  ASSERT_EQ(booleanField->GetBoolean(h_klass.Get()), false);

  ArtField* byteField = h_klass->FindDeclaredStaticField("byteField", "B");
  ASSERT_TRUE(byteField != nullptr);
  ASSERT_EQ(byteField->GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  ASSERT_EQ(byteField->GetByte(h_klass.Get()), 0);

  ArtField* charField = h_klass->FindDeclaredStaticField("charField", "C");
  ASSERT_TRUE(charField != nullptr);
  ASSERT_EQ(charField->GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  ASSERT_EQ(charField->GetChar(h_klass.Get()), 0u);

  ArtField* shortField = h_klass->FindDeclaredStaticField("shortField", "S");
  ASSERT_TRUE(shortField != nullptr);
  ASSERT_EQ(shortField->GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  ASSERT_EQ(shortField->GetShort(h_klass.Get()), 0);

  ArtField* intField = h_klass->FindDeclaredStaticField("intField", "I");
  ASSERT_TRUE(intField != nullptr);
  ASSERT_EQ(intField->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  ASSERT_EQ(intField->GetInt(h_klass.Get()), 0);

  ArtField* longField = h_klass->FindDeclaredStaticField("longField", "J");
  ASSERT_TRUE(longField != nullptr);
  ASSERT_EQ(longField->GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  ASSERT_EQ(longField->GetLong(h_klass.Get()), static_cast<int64_t>(0));

  ArtField* floatField = h_klass->FindDeclaredStaticField("floatField", "F");
  ASSERT_TRUE(floatField != nullptr);
  ASSERT_EQ(floatField->GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  ASSERT_FLOAT_EQ(floatField->GetFloat(h_klass.Get()), static_cast<float>(0.0f));

  ArtField* doubleField = h_klass->FindDeclaredStaticField("doubleField", "D");
  ASSERT_TRUE(doubleField != nullptr);
  ASSERT_EQ(doubleField->GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  ASSERT_DOUBLE_EQ(doubleField->GetDouble(h_klass.Get()), static_cast<double>(0.0));

  ArtField* objectField = h_klass->FindDeclaredStaticField("objectField",
                                                                   "Ljava/lang/Object;");
  ASSERT_TRUE(objectField != nullptr);
  ASSERT_EQ(objectField->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  ASSERT_EQ(objectField->GetObject(h_klass.Get()), nullptr);

  // Create a java.lang.Object instance to set objectField.
  Handle<mirror::Class> object_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(object_klass.Get() != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj.Get() != nullptr);
  ASSERT_EQ(h_obj->GetClass(), h_klass.Get());

  // Modify fields inside transaction then rollback changes.
  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  booleanField->SetBoolean<true>(h_klass.Get(), true);
  byteField->SetByte<true>(h_klass.Get(), 1);
  charField->SetChar<true>(h_klass.Get(), 1u);
  shortField->SetShort<true>(h_klass.Get(), 1);
  intField->SetInt<true>(h_klass.Get(), 1);
  longField->SetLong<true>(h_klass.Get(), 1);
  floatField->SetFloat<true>(h_klass.Get(), 1.0);
  doubleField->SetDouble<true>(h_klass.Get(), 1.0);
  objectField->SetObject<true>(h_klass.Get(), h_obj.Get());
  Runtime::Current()->ExitTransactionMode();
  transaction.Rollback();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanField->GetBoolean(h_klass.Get()), false);
  EXPECT_EQ(byteField->GetByte(h_klass.Get()), 0);
  EXPECT_EQ(charField->GetChar(h_klass.Get()), 0u);
  EXPECT_EQ(shortField->GetShort(h_klass.Get()), 0);
  EXPECT_EQ(intField->GetInt(h_klass.Get()), 0);
  EXPECT_EQ(longField->GetLong(h_klass.Get()), static_cast<int64_t>(0));
  EXPECT_FLOAT_EQ(floatField->GetFloat(h_klass.Get()), static_cast<float>(0.0f));
  EXPECT_DOUBLE_EQ(doubleField->GetDouble(h_klass.Get()), static_cast<double>(0.0));
  EXPECT_EQ(objectField->GetObject(h_klass.Get()), nullptr);
}

// Tests instance fields are reset to their default value after transaction rollback.
TEST_F(TransactionTest, InstanceFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader.Get() != nullptr);

  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LInstanceFieldsTest;", class_loader)));
  ASSERT_TRUE(h_klass.Get() != nullptr);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());

  // Allocate an InstanceFieldTest object.
  Handle<mirror::Object> h_instance(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_instance.Get() != nullptr);

  // Lookup fields.
  ArtField* booleanField = h_klass->FindDeclaredInstanceField("booleanField", "Z");
  ASSERT_TRUE(booleanField != nullptr);
  ASSERT_EQ(booleanField->GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  ASSERT_EQ(booleanField->GetBoolean(h_instance.Get()), false);

  ArtField* byteField = h_klass->FindDeclaredInstanceField("byteField", "B");
  ASSERT_TRUE(byteField != nullptr);
  ASSERT_EQ(byteField->GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  ASSERT_EQ(byteField->GetByte(h_instance.Get()), 0);

  ArtField* charField = h_klass->FindDeclaredInstanceField("charField", "C");
  ASSERT_TRUE(charField != nullptr);
  ASSERT_EQ(charField->GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  ASSERT_EQ(charField->GetChar(h_instance.Get()), 0u);

  ArtField* shortField = h_klass->FindDeclaredInstanceField("shortField", "S");
  ASSERT_TRUE(shortField != nullptr);
  ASSERT_EQ(shortField->GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  ASSERT_EQ(shortField->GetShort(h_instance.Get()), 0);

  ArtField* intField = h_klass->FindDeclaredInstanceField("intField", "I");
  ASSERT_TRUE(intField != nullptr);
  ASSERT_EQ(intField->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  ASSERT_EQ(intField->GetInt(h_instance.Get()), 0);

  ArtField* longField = h_klass->FindDeclaredInstanceField("longField", "J");
  ASSERT_TRUE(longField != nullptr);
  ASSERT_EQ(longField->GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  ASSERT_EQ(longField->GetLong(h_instance.Get()), static_cast<int64_t>(0));

  ArtField* floatField = h_klass->FindDeclaredInstanceField("floatField", "F");
  ASSERT_TRUE(floatField != nullptr);
  ASSERT_EQ(floatField->GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  ASSERT_FLOAT_EQ(floatField->GetFloat(h_instance.Get()), static_cast<float>(0.0f));

  ArtField* doubleField = h_klass->FindDeclaredInstanceField("doubleField", "D");
  ASSERT_TRUE(doubleField != nullptr);
  ASSERT_EQ(doubleField->GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  ASSERT_DOUBLE_EQ(doubleField->GetDouble(h_instance.Get()), static_cast<double>(0.0));

  ArtField* objectField = h_klass->FindDeclaredInstanceField("objectField",
                                                                        "Ljava/lang/Object;");
  ASSERT_TRUE(objectField != nullptr);
  ASSERT_EQ(objectField->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  ASSERT_EQ(objectField->GetObject(h_instance.Get()), nullptr);

  // Create a java.lang.Object instance to set objectField.
  Handle<mirror::Class> object_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(object_klass.Get() != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj.Get() != nullptr);
  ASSERT_EQ(h_obj->GetClass(), h_klass.Get());

  // Modify fields inside transaction then rollback changes.
  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  booleanField->SetBoolean<true>(h_instance.Get(), true);
  byteField->SetByte<true>(h_instance.Get(), 1);
  charField->SetChar<true>(h_instance.Get(), 1u);
  shortField->SetShort<true>(h_instance.Get(), 1);
  intField->SetInt<true>(h_instance.Get(), 1);
  longField->SetLong<true>(h_instance.Get(), 1);
  floatField->SetFloat<true>(h_instance.Get(), 1.0);
  doubleField->SetDouble<true>(h_instance.Get(), 1.0);
  objectField->SetObject<true>(h_instance.Get(), h_obj.Get());
  Runtime::Current()->ExitTransactionMode();
  transaction.Rollback();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanField->GetBoolean(h_instance.Get()), false);
  EXPECT_EQ(byteField->GetByte(h_instance.Get()), 0);
  EXPECT_EQ(charField->GetChar(h_instance.Get()), 0u);
  EXPECT_EQ(shortField->GetShort(h_instance.Get()), 0);
  EXPECT_EQ(intField->GetInt(h_instance.Get()), 0);
  EXPECT_EQ(longField->GetLong(h_instance.Get()), static_cast<int64_t>(0));
  EXPECT_FLOAT_EQ(floatField->GetFloat(h_instance.Get()), static_cast<float>(0.0f));
  EXPECT_DOUBLE_EQ(doubleField->GetDouble(h_instance.Get()), static_cast<double>(0.0));
  EXPECT_EQ(objectField->GetObject(h_instance.Get()), nullptr);
}

// Tests static array fields are reset to their default value after transaction rollback.
TEST_F(TransactionTest, StaticArrayFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader.Get() != nullptr);

  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LStaticArrayFieldsTest;", class_loader)));
  ASSERT_TRUE(h_klass.Get() != nullptr);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());

  // Lookup fields.
  ArtField* booleanArrayField = h_klass->FindDeclaredStaticField("booleanArrayField", "[Z");
  ASSERT_TRUE(booleanArrayField != nullptr);
  mirror::BooleanArray* booleanArray = booleanArrayField->GetObject(h_klass.Get())->AsBooleanArray();
  ASSERT_TRUE(booleanArray != nullptr);
  ASSERT_EQ(booleanArray->GetLength(), 1);
  ASSERT_EQ(booleanArray->GetWithoutChecks(0), false);

  ArtField* byteArrayField = h_klass->FindDeclaredStaticField("byteArrayField", "[B");
  ASSERT_TRUE(byteArrayField != nullptr);
  mirror::ByteArray* byteArray = byteArrayField->GetObject(h_klass.Get())->AsByteArray();
  ASSERT_TRUE(byteArray != nullptr);
  ASSERT_EQ(byteArray->GetLength(), 1);
  ASSERT_EQ(byteArray->GetWithoutChecks(0), 0);

  ArtField* charArrayField = h_klass->FindDeclaredStaticField("charArrayField", "[C");
  ASSERT_TRUE(charArrayField != nullptr);
  mirror::CharArray* charArray = charArrayField->GetObject(h_klass.Get())->AsCharArray();
  ASSERT_TRUE(charArray != nullptr);
  ASSERT_EQ(charArray->GetLength(), 1);
  ASSERT_EQ(charArray->GetWithoutChecks(0), 0u);

  ArtField* shortArrayField = h_klass->FindDeclaredStaticField("shortArrayField", "[S");
  ASSERT_TRUE(shortArrayField != nullptr);
  mirror::ShortArray* shortArray = shortArrayField->GetObject(h_klass.Get())->AsShortArray();
  ASSERT_TRUE(shortArray != nullptr);
  ASSERT_EQ(shortArray->GetLength(), 1);
  ASSERT_EQ(shortArray->GetWithoutChecks(0), 0);

  ArtField* intArrayField = h_klass->FindDeclaredStaticField("intArrayField", "[I");
  ASSERT_TRUE(intArrayField != nullptr);
  mirror::IntArray* intArray = intArrayField->GetObject(h_klass.Get())->AsIntArray();
  ASSERT_TRUE(intArray != nullptr);
  ASSERT_EQ(intArray->GetLength(), 1);
  ASSERT_EQ(intArray->GetWithoutChecks(0), 0);

  ArtField* longArrayField = h_klass->FindDeclaredStaticField("longArrayField", "[J");
  ASSERT_TRUE(longArrayField != nullptr);
  mirror::LongArray* longArray = longArrayField->GetObject(h_klass.Get())->AsLongArray();
  ASSERT_TRUE(longArray != nullptr);
  ASSERT_EQ(longArray->GetLength(), 1);
  ASSERT_EQ(longArray->GetWithoutChecks(0), static_cast<int64_t>(0));

  ArtField* floatArrayField = h_klass->FindDeclaredStaticField("floatArrayField", "[F");
  ASSERT_TRUE(floatArrayField != nullptr);
  mirror::FloatArray* floatArray = floatArrayField->GetObject(h_klass.Get())->AsFloatArray();
  ASSERT_TRUE(floatArray != nullptr);
  ASSERT_EQ(floatArray->GetLength(), 1);
  ASSERT_FLOAT_EQ(floatArray->GetWithoutChecks(0), static_cast<float>(0.0f));

  ArtField* doubleArrayField = h_klass->FindDeclaredStaticField("doubleArrayField", "[D");
  ASSERT_TRUE(doubleArrayField != nullptr);
  mirror::DoubleArray* doubleArray = doubleArrayField->GetObject(h_klass.Get())->AsDoubleArray();
  ASSERT_TRUE(doubleArray != nullptr);
  ASSERT_EQ(doubleArray->GetLength(), 1);
  ASSERT_DOUBLE_EQ(doubleArray->GetWithoutChecks(0), static_cast<double>(0.0f));

  ArtField* objectArrayField = h_klass->FindDeclaredStaticField("objectArrayField",
                                                                           "[Ljava/lang/Object;");
  ASSERT_TRUE(objectArrayField != nullptr);
  mirror::ObjectArray<mirror::Object>* objectArray =
      objectArrayField->GetObject(h_klass.Get())->AsObjectArray<mirror::Object>();
  ASSERT_TRUE(objectArray != nullptr);
  ASSERT_EQ(objectArray->GetLength(), 1);
  ASSERT_EQ(objectArray->GetWithoutChecks(0), nullptr);

  // Create a java.lang.Object instance to set objectField.
  Handle<mirror::Class> object_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(object_klass.Get() != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj.Get() != nullptr);
  ASSERT_EQ(h_obj->GetClass(), h_klass.Get());

  // Modify fields inside transaction then rollback changes.
  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  booleanArray->SetWithoutChecks<true>(0, true);
  byteArray->SetWithoutChecks<true>(0, 1);
  charArray->SetWithoutChecks<true>(0, 1u);
  shortArray->SetWithoutChecks<true>(0, 1);
  intArray->SetWithoutChecks<true>(0, 1);
  longArray->SetWithoutChecks<true>(0, 1);
  floatArray->SetWithoutChecks<true>(0, 1.0);
  doubleArray->SetWithoutChecks<true>(0, 1.0);
  objectArray->SetWithoutChecks<true>(0, h_obj.Get());
  Runtime::Current()->ExitTransactionMode();
  transaction.Rollback();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanArray->GetWithoutChecks(0), false);
  EXPECT_EQ(byteArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(charArray->GetWithoutChecks(0), 0u);
  EXPECT_EQ(shortArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(intArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(longArray->GetWithoutChecks(0), static_cast<int64_t>(0));
  EXPECT_FLOAT_EQ(floatArray->GetWithoutChecks(0), static_cast<float>(0.0f));
  EXPECT_DOUBLE_EQ(doubleArray->GetWithoutChecks(0), static_cast<double>(0.0f));
  EXPECT_EQ(objectArray->GetWithoutChecks(0), nullptr);
}

// Tests successful class initialization without class initializer.
TEST_F(TransactionTest, EmptyClass) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader.Get() != nullptr);

  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LTransaction$EmptyStatic;",
                                            class_loader)));
  ASSERT_TRUE(h_klass.Get() != nullptr);
  class_linker_->VerifyClass(soa.Self(), h_klass);
  ASSERT_TRUE(h_klass->IsVerified());

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  Runtime::Current()->ExitTransactionMode();
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

// Tests successful class initialization with class initializer.
TEST_F(TransactionTest, StaticFieldClass) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader.Get() != nullptr);

  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LTransaction$StaticFieldClass;",
                                            class_loader)));
  ASSERT_TRUE(h_klass.Get() != nullptr);
  class_linker_->VerifyClass(soa.Self(), h_klass);
  ASSERT_TRUE(h_klass->IsVerified());

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  Runtime::Current()->ExitTransactionMode();
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

// Tests failing class initialization due to native call.
TEST_F(TransactionTest, NativeCallAbortClass) {
  testTransactionAbort("LTransaction$NativeCallAbortClass;");
}

// Tests failing class initialization due to native call in a "synchronized" statement
// (which must catch any exception, do the monitor-exit then re-throw the caught exception).
TEST_F(TransactionTest, SynchronizedNativeCallAbortClass) {
  testTransactionAbort("LTransaction$SynchronizedNativeCallAbortClass;");
}

// Tests failing class initialization due to native call, even if an "all" catch handler
// catches the exception thrown when aborting the transaction.
TEST_F(TransactionTest, CatchNativeCallAbortClass) {
  testTransactionAbort("LTransaction$CatchNativeCallAbortClass;");
}

// Tests failing class initialization with multiple transaction aborts.
TEST_F(TransactionTest, MultipleNativeCallAbortClass) {
  testTransactionAbort("LTransaction$MultipleNativeCallAbortClass;");
}

// Tests failing class initialization due to allocating instance of finalizable class.
TEST_F(TransactionTest, FinalizableAbortClass) {
  testTransactionAbort("LTransaction$FinalizableAbortClass;");
}
}  // namespace art
