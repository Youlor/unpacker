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

#include "dex_cache.h"

#include <stdio.h>

#include "class_linker.h"
#include "common_runtime_test.h"
#include "linear_alloc.h"
#include "mirror/class_loader-inl.h"
#include "handle_scope-inl.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace mirror {

class DexCacheTest : public CommonRuntimeTest {};

TEST_F(DexCacheTest, Open) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  Handle<DexCache> dex_cache(
      hs.NewHandle(class_linker_->AllocDexCache(soa.Self(),
                                                *java_lang_dex_file_,
                                                Runtime::Current()->GetLinearAlloc())));
  ASSERT_TRUE(dex_cache.Get() != nullptr);

  EXPECT_EQ(java_lang_dex_file_->NumStringIds(), dex_cache->NumStrings());
  EXPECT_EQ(java_lang_dex_file_->NumTypeIds(),   dex_cache->NumResolvedTypes());
  EXPECT_EQ(java_lang_dex_file_->NumMethodIds(), dex_cache->NumResolvedMethods());
  EXPECT_EQ(java_lang_dex_file_->NumFieldIds(),  dex_cache->NumResolvedFields());
}

TEST_F(DexCacheTest, LinearAlloc) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader(LoadDex("Main"));
  ASSERT_TRUE(jclass_loader != nullptr);
  Runtime* const runtime = Runtime::Current();
  ClassLinker* const class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  mirror::Class* klass = class_linker->FindClass(soa.Self(), "LMain;", class_loader);
  ASSERT_TRUE(klass != nullptr);
  LinearAlloc* const linear_alloc = klass->GetClassLoader()->GetAllocator();
  EXPECT_NE(linear_alloc, runtime->GetLinearAlloc());
  EXPECT_TRUE(linear_alloc->Contains(klass->GetDexCache()->GetResolvedMethods()));
}

TEST_F(DexCacheTest, TestResolvedFieldAccess) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader(LoadDex("Packages"));
  ASSERT_TRUE(jclass_loader != nullptr);
  Runtime* const runtime = Runtime::Current();
  ClassLinker* const class_linker = runtime->GetClassLinker();
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  Handle<mirror::Class> klass1 =
      hs.NewHandle(class_linker->FindClass(soa.Self(), "Lpackage1/Package1;", class_loader));
  ASSERT_TRUE(klass1.Get() != nullptr);
  Handle<mirror::Class> klass2 =
      hs.NewHandle(class_linker->FindClass(soa.Self(), "Lpackage2/Package2;", class_loader));
  ASSERT_TRUE(klass2.Get() != nullptr);
  EXPECT_EQ(klass1->GetDexCache(), klass2->GetDexCache());

  EXPECT_NE(klass1->NumStaticFields(), 0u);
  for (ArtField& field : klass2->GetSFields()) {
    EXPECT_FALSE((
        klass1->ResolvedFieldAccessTest</*throw_on_failure*/ false,
            /*use_referrers_cache*/ false>(klass2.Get(),
                                           &field,
                                           field.GetDexFieldIndex(),
                                           klass1->GetDexCache())));
  }
}

}  // namespace mirror
}  // namespace art
