/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include <gtest/gtest.h>

#include "base/unix_file/fd_file.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "method_reference.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "handle_scope-inl.h"
#include "jit/offline_profiling_info.h"
#include "scoped_thread_state_change.h"

namespace art {

class ProfileCompilationInfoTest : public CommonRuntimeTest {
 protected:
  std::vector<ArtMethod*> GetVirtualMethods(jobject class_loader,
                                            const std::string& clazz) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Thread* self = Thread::Current();
    ScopedObjectAccess soa(self);
    StackHandleScope<1> hs(self);
    Handle<mirror::ClassLoader> h_loader(hs.NewHandle(
        reinterpret_cast<mirror::ClassLoader*>(self->DecodeJObject(class_loader))));
    mirror::Class* klass = class_linker->FindClass(self, clazz.c_str(), h_loader);

    const auto pointer_size = class_linker->GetImagePointerSize();
    std::vector<ArtMethod*> methods;
    for (auto& m : klass->GetVirtualMethods(pointer_size)) {
      methods.push_back(&m);
    }
    return methods;
  }

  bool AddMethod(const std::string& dex_location,
                 uint32_t checksum,
                 uint16_t method_index,
                 ProfileCompilationInfo* info) {
    return info->AddMethodIndex(dex_location, checksum, method_index);
  }

  bool AddClass(const std::string& dex_location,
                uint32_t checksum,
                uint16_t class_index,
                ProfileCompilationInfo* info) {
    return info->AddMethodIndex(dex_location, checksum, class_index);
  }

  uint32_t GetFd(const ScratchFile& file) {
    return static_cast<uint32_t>(file.GetFd());
  }

  bool SaveProfilingInfo(
      const std::string& filename,
      const std::vector<ArtMethod*>& methods,
      const std::set<DexCacheResolvedClasses>& resolved_classes) {
    ProfileCompilationInfo info;
    std::vector<MethodReference> method_refs;
    ScopedObjectAccess soa(Thread::Current());
    for (ArtMethod* method : methods) {
      method_refs.emplace_back(method->GetDexFile(), method->GetDexMethodIndex());
    }
    if (!info.AddMethodsAndClasses(method_refs, resolved_classes)) {
      return false;
    }
    return info.MergeAndSave(filename, nullptr, false);
  }

  // Cannot sizeof the actual arrays so hardcode the values here.
  // They should not change anyway.
  static constexpr int kProfileMagicSize = 4;
  static constexpr int kProfileVersionSize = 4;
};

TEST_F(ProfileCompilationInfoTest, SaveArtMethods) {
  ScratchFile profile;

  Thread* self = Thread::Current();
  jobject class_loader;
  {
    ScopedObjectAccess soa(self);
    class_loader = LoadDex("ProfileTestMultiDex");
  }
  ASSERT_NE(class_loader, nullptr);

  // Save virtual methods from Main.
  std::set<DexCacheResolvedClasses> resolved_classes;
  std::vector<ArtMethod*> main_methods = GetVirtualMethods(class_loader, "LMain;");
  ASSERT_TRUE(SaveProfilingInfo(profile.GetFilename(), main_methods, resolved_classes));

  // Check that what we saved is in the profile.
  ProfileCompilationInfo info1;
  ASSERT_TRUE(info1.Load(GetFd(profile)));
  ASSERT_EQ(info1.GetNumberOfMethods(), main_methods.size());
  {
    ScopedObjectAccess soa(self);
    for (ArtMethod* m : main_methods) {
      ASSERT_TRUE(info1.ContainsMethod(MethodReference(m->GetDexFile(), m->GetDexMethodIndex())));
    }
  }

  // Save virtual methods from Second.
  std::vector<ArtMethod*> second_methods = GetVirtualMethods(class_loader, "LSecond;");
  ASSERT_TRUE(SaveProfilingInfo(profile.GetFilename(), second_methods, resolved_classes));

  // Check that what we saved is in the profile (methods form Main and Second).
  ProfileCompilationInfo info2;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(info2.Load(GetFd(profile)));
  ASSERT_EQ(info2.GetNumberOfMethods(), main_methods.size() + second_methods.size());
  {
    ScopedObjectAccess soa(self);
    for (ArtMethod* m : main_methods) {
      ASSERT_TRUE(info2.ContainsMethod(MethodReference(m->GetDexFile(), m->GetDexMethodIndex())));
    }
    for (ArtMethod* m : second_methods) {
      ASSERT_TRUE(info2.ContainsMethod(MethodReference(m->GetDexFile(), m->GetDexMethodIndex())));
    }
  }
}

TEST_F(ProfileCompilationInfoTest, SaveFd) {
  ScratchFile profile;

  ProfileCompilationInfo saved_info;
  // Save a few methods.
  for (uint16_t i = 0; i < 10; i++) {
    ASSERT_TRUE(AddMethod("dex_location1", /* checksum */ 1, /* method_idx */ i, &saved_info));
    ASSERT_TRUE(AddMethod("dex_location2", /* checksum */ 2, /* method_idx */ i, &saved_info));
  }
  ASSERT_TRUE(saved_info.Save(GetFd(profile)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  // Check that we get back what we saved.
  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(loaded_info.Load(GetFd(profile)));
  ASSERT_TRUE(loaded_info.Equals(saved_info));

  // Save more methods.
  for (uint16_t i = 0; i < 100; i++) {
    ASSERT_TRUE(AddMethod("dex_location1", /* checksum */ 1, /* method_idx */ i, &saved_info));
    ASSERT_TRUE(AddMethod("dex_location2", /* checksum */ 2, /* method_idx */ i, &saved_info));
    ASSERT_TRUE(AddMethod("dex_location3", /* checksum */ 3, /* method_idx */ i, &saved_info));
  }
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(saved_info.Save(GetFd(profile)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  // Check that we get back everything we saved.
  ProfileCompilationInfo loaded_info2;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(loaded_info2.Load(GetFd(profile)));
  ASSERT_TRUE(loaded_info2.Equals(saved_info));
}

TEST_F(ProfileCompilationInfoTest, AddMethodsAndClassesFail) {
  ScratchFile profile;

  ProfileCompilationInfo info;
  ASSERT_TRUE(AddMethod("dex_location", /* checksum */ 1, /* method_idx */ 1, &info));
  // Trying to add info for an existing file but with a different checksum.
  ASSERT_FALSE(AddMethod("dex_location", /* checksum */ 2, /* method_idx */ 2, &info));
}

TEST_F(ProfileCompilationInfoTest, MergeFail) {
  ScratchFile profile;

  ProfileCompilationInfo info1;
  ASSERT_TRUE(AddMethod("dex_location", /* checksum */ 1, /* method_idx */ 1, &info1));
  // Use the same file, change the checksum.
  ProfileCompilationInfo info2;
  ASSERT_TRUE(AddMethod("dex_location", /* checksum */ 2, /* method_idx */ 2, &info2));

  ASSERT_FALSE(info1.MergeWith(info2));
}

TEST_F(ProfileCompilationInfoTest, SaveMaxMethods) {
  ScratchFile profile;

  ProfileCompilationInfo saved_info;
  // Save the maximum number of methods
  for (uint16_t i = 0; i < std::numeric_limits<uint16_t>::max(); i++) {
    ASSERT_TRUE(AddMethod("dex_location1", /* checksum */ 1, /* method_idx */ i, &saved_info));
    ASSERT_TRUE(AddMethod("dex_location2", /* checksum */ 2, /* method_idx */ i, &saved_info));
  }
  // Save the maximum number of classes
  for (uint16_t i = 0; i < std::numeric_limits<uint16_t>::max(); i++) {
    ASSERT_TRUE(AddClass("dex_location1", /* checksum */ 1, /* class_idx */ i, &saved_info));
    ASSERT_TRUE(AddClass("dex_location2", /* checksum */ 2, /* class_idx */ i, &saved_info));
  }

  ASSERT_TRUE(saved_info.Save(GetFd(profile)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  // Check that we get back what we saved.
  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(loaded_info.Load(GetFd(profile)));
  ASSERT_TRUE(loaded_info.Equals(saved_info));
}

TEST_F(ProfileCompilationInfoTest, SaveEmpty) {
  ScratchFile profile;

  ProfileCompilationInfo saved_info;
  ASSERT_TRUE(saved_info.Save(GetFd(profile)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  // Check that we get back what we saved.
  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(loaded_info.Load(GetFd(profile)));
  ASSERT_TRUE(loaded_info.Equals(saved_info));
}

TEST_F(ProfileCompilationInfoTest, LoadEmpty) {
  ScratchFile profile;

  ProfileCompilationInfo empyt_info;

  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_TRUE(loaded_info.Load(GetFd(profile)));
  ASSERT_TRUE(loaded_info.Equals(empyt_info));
}

TEST_F(ProfileCompilationInfoTest, BadMagic) {
  ScratchFile profile;
  uint8_t buffer[] = { 1, 2, 3, 4 };
  ASSERT_TRUE(profile.GetFile()->WriteFully(buffer, sizeof(buffer)));
  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_FALSE(loaded_info.Load(GetFd(profile)));
}

TEST_F(ProfileCompilationInfoTest, BadVersion) {
  ScratchFile profile;

  ASSERT_TRUE(profile.GetFile()->WriteFully(
      ProfileCompilationInfo::kProfileMagic, kProfileMagicSize));
  uint8_t version[] = { 'v', 'e', 'r', 's', 'i', 'o', 'n' };
  ASSERT_TRUE(profile.GetFile()->WriteFully(version, sizeof(version)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_FALSE(loaded_info.Load(GetFd(profile)));
}

TEST_F(ProfileCompilationInfoTest, Incomplete) {
  ScratchFile profile;
  ASSERT_TRUE(profile.GetFile()->WriteFully(
      ProfileCompilationInfo::kProfileMagic, kProfileMagicSize));
  ASSERT_TRUE(profile.GetFile()->WriteFully(
      ProfileCompilationInfo::kProfileVersion, kProfileVersionSize));
  // Write that we have at least one line.
  uint8_t line_number[] = { 0, 1 };
  ASSERT_TRUE(profile.GetFile()->WriteFully(line_number, sizeof(line_number)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_FALSE(loaded_info.Load(GetFd(profile)));
}

TEST_F(ProfileCompilationInfoTest, TooLongDexLocation) {
  ScratchFile profile;
  ASSERT_TRUE(profile.GetFile()->WriteFully(
      ProfileCompilationInfo::kProfileMagic, kProfileMagicSize));
  ASSERT_TRUE(profile.GetFile()->WriteFully(
      ProfileCompilationInfo::kProfileVersion, kProfileVersionSize));
  // Write that we have at least one line.
  uint8_t line_number[] = { 0, 1 };
  ASSERT_TRUE(profile.GetFile()->WriteFully(line_number, sizeof(line_number)));

  // dex_location_size, methods_size, classes_size, checksum.
  // Dex location size is too big and should be rejected.
  uint8_t line[] = { 255, 255, 0, 1, 0, 1, 0, 0, 0, 0 };
  ASSERT_TRUE(profile.GetFile()->WriteFully(line, sizeof(line)));
  ASSERT_EQ(0, profile.GetFile()->Flush());

  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_FALSE(loaded_info.Load(GetFd(profile)));
}

TEST_F(ProfileCompilationInfoTest, UnexpectedContent) {
  ScratchFile profile;

  ProfileCompilationInfo saved_info;
  // Save the maximum number of methods
  for (uint16_t i = 0; i < 10; i++) {
    ASSERT_TRUE(AddMethod("dex_location1", /* checksum */ 1, /* method_idx */ i, &saved_info));
  }
  ASSERT_TRUE(saved_info.Save(GetFd(profile)));

  uint8_t random_data[] = { 1, 2, 3};
  ASSERT_TRUE(profile.GetFile()->WriteFully(random_data, sizeof(random_data)));

  ASSERT_EQ(0, profile.GetFile()->Flush());

  // Check that we fail because of unexpected data at the end of the file.
  ProfileCompilationInfo loaded_info;
  ASSERT_TRUE(profile.GetFile()->ResetOffset());
  ASSERT_FALSE(loaded_info.Load(GetFd(profile)));
}

}  // namespace art
