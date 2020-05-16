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

#include "dex_file.h"

#include <memory>

#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "common_runtime_test.h"
#include "dex_file-inl.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

class DexFileTest : public CommonRuntimeTest {};

TEST_F(DexFileTest, Open) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("Nested"));
  ASSERT_TRUE(dex.get() != nullptr);
}

static const uint8_t kBase64Map[256] = {
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
  52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255,
  255, 254, 255, 255, 255,   0,   1,   2,   3,   4,   5,   6,
    7,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,  18,  // NOLINT
   19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,  // NOLINT
  255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
   37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  // NOLINT
   49,  50,  51, 255, 255, 255, 255, 255, 255, 255, 255, 255,  // NOLINT
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255
};

static inline uint8_t* DecodeBase64(const char* src, size_t* dst_size) {
  std::vector<uint8_t> tmp;
  uint32_t t = 0, y = 0;
  int g = 3;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    uint8_t c = kBase64Map[src[i] & 0xFF];
    if (c == 255) continue;
    // the final = symbols are read and used to trim the remaining bytes
    if (c == 254) {
      c = 0;
      // prevent g < 0 which would potentially allow an overflow later
      if (--g < 0) {
        *dst_size = 0;
        return nullptr;
      }
    } else if (g != 3) {
      // we only allow = to be at the end
      *dst_size = 0;
      return nullptr;
    }
    t = (t << 6) | c;
    if (++y == 4) {
      tmp.push_back((t >> 16) & 255);
      if (g > 1) {
        tmp.push_back((t >> 8) & 255);
      }
      if (g > 2) {
        tmp.push_back(t & 255);
      }
      y = t = 0;
    }
  }
  if (y != 0) {
    *dst_size = 0;
    return nullptr;
  }
  std::unique_ptr<uint8_t[]> dst(new uint8_t[tmp.size()]);
  if (dst_size != nullptr) {
    *dst_size = tmp.size();
  } else {
    *dst_size = 0;
  }
  std::copy(tmp.begin(), tmp.end(), dst.get());
  return dst.release();
}

// Although this is the same content logically as the Nested test dex,
// the DexFileHeader test is sensitive to subtle changes in the
// contents due to the checksum etc, so we embed the exact input here.
//
// class Nested {
//     class Inner {
//     }
// }
static const char kRawDex[] =
  "ZGV4CjAzNQAQedgAe7gM1B/WHsWJ6L7lGAISGC7yjD2IAwAAcAAAAHhWNBIAAAAAAAAAAMQCAAAP"
  "AAAAcAAAAAcAAACsAAAAAgAAAMgAAAABAAAA4AAAAAMAAADoAAAAAgAAAAABAABIAgAAQAEAAK4B"
  "AAC2AQAAvQEAAM0BAADXAQAA+wEAABsCAAA+AgAAUgIAAF8CAABiAgAAZgIAAHMCAAB5AgAAgQIA"
  "AAIAAAADAAAABAAAAAUAAAAGAAAABwAAAAkAAAAJAAAABgAAAAAAAAAKAAAABgAAAKgBAAAAAAEA"
  "DQAAAAAAAQAAAAAAAQAAAAAAAAAFAAAAAAAAAAAAAAAAAAAABQAAAAAAAAAIAAAAiAEAAKsCAAAA"
  "AAAAAQAAAAAAAAAFAAAAAAAAAAgAAACYAQAAuAIAAAAAAAACAAAAlAIAAJoCAAABAAAAowIAAAIA"
  "AgABAAAAiAIAAAYAAABbAQAAcBACAAAADgABAAEAAQAAAI4CAAAEAAAAcBACAAAADgBAAQAAAAAA"
  "AAAAAAAAAAAATAEAAAAAAAAAAAAAAAAAAAEAAAABAAY8aW5pdD4ABUlubmVyAA5MTmVzdGVkJElu"
  "bmVyOwAITE5lc3RlZDsAIkxkYWx2aWsvYW5ub3RhdGlvbi9FbmNsb3NpbmdDbGFzczsAHkxkYWx2"
  "aWsvYW5ub3RhdGlvbi9Jbm5lckNsYXNzOwAhTGRhbHZpay9hbm5vdGF0aW9uL01lbWJlckNsYXNz"
  "ZXM7ABJMamF2YS9sYW5nL09iamVjdDsAC05lc3RlZC5qYXZhAAFWAAJWTAALYWNjZXNzRmxhZ3MA"
  "BG5hbWUABnRoaXMkMAAFdmFsdWUAAgEABw4AAQAHDjwAAgIBDhgBAgMCCwQADBcBAgQBDhwBGAAA"
  "AQEAAJAgAICABNQCAAABAAGAgATwAgAAEAAAAAAAAAABAAAAAAAAAAEAAAAPAAAAcAAAAAIAAAAH"
  "AAAArAAAAAMAAAACAAAAyAAAAAQAAAABAAAA4AAAAAUAAAADAAAA6AAAAAYAAAACAAAAAAEAAAMQ"
  "AAACAAAAQAEAAAEgAAACAAAAVAEAAAYgAAACAAAAiAEAAAEQAAABAAAAqAEAAAIgAAAPAAAArgEA"
  "AAMgAAACAAAAiAIAAAQgAAADAAAAlAIAAAAgAAACAAAAqwIAAAAQAAABAAAAxAIAAA==";

static std::unique_ptr<const DexFile> OpenDexFileBase64(const char* base64,
                                                        const char* location) {
  // decode base64
  CHECK(base64 != nullptr);
  size_t length;
  std::unique_ptr<uint8_t[]> dex_bytes(DecodeBase64(base64, &length));
  CHECK(dex_bytes.get() != nullptr);

  // write to provided file
  std::unique_ptr<File> file(OS::CreateEmptyFile(location));
  CHECK(file.get() != nullptr);
  if (!file->WriteFully(dex_bytes.get(), length)) {
    PLOG(FATAL) << "Failed to write base64 as dex file";
  }
  if (file->FlushCloseOrErase() != 0) {
    PLOG(FATAL) << "Could not flush and close test file.";
  }
  file.reset();

  // read dex file
  ScopedObjectAccess soa(Thread::Current());
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> tmp;
  bool success = DexFile::Open(location, location, &error_msg, &tmp);
  CHECK(success) << error_msg;
  EXPECT_EQ(1U, tmp.size());
  std::unique_ptr<const DexFile> dex_file = std::move(tmp[0]);
  EXPECT_EQ(PROT_READ, dex_file->GetPermissions());
  EXPECT_TRUE(dex_file->IsReadOnly());
  return dex_file;
}

TEST_F(DexFileTest, Header) {
  ScratchFile tmp;
  std::unique_ptr<const DexFile> raw(OpenDexFileBase64(kRawDex, tmp.GetFilename().c_str()));
  ASSERT_TRUE(raw.get() != nullptr);

  const DexFile::Header& header = raw->GetHeader();
  // TODO: header.magic_
  EXPECT_EQ(0x00d87910U, header.checksum_);
  // TODO: header.signature_
  EXPECT_EQ(904U, header.file_size_);
  EXPECT_EQ(112U, header.header_size_);
  EXPECT_EQ(0U, header.link_size_);
  EXPECT_EQ(0U, header.link_off_);
  EXPECT_EQ(15U, header.string_ids_size_);
  EXPECT_EQ(112U, header.string_ids_off_);
  EXPECT_EQ(7U, header.type_ids_size_);
  EXPECT_EQ(172U, header.type_ids_off_);
  EXPECT_EQ(2U, header.proto_ids_size_);
  EXPECT_EQ(200U, header.proto_ids_off_);
  EXPECT_EQ(1U, header.field_ids_size_);
  EXPECT_EQ(224U, header.field_ids_off_);
  EXPECT_EQ(3U, header.method_ids_size_);
  EXPECT_EQ(232U, header.method_ids_off_);
  EXPECT_EQ(2U, header.class_defs_size_);
  EXPECT_EQ(256U, header.class_defs_off_);
  EXPECT_EQ(584U, header.data_size_);
  EXPECT_EQ(320U, header.data_off_);

  EXPECT_EQ(header.checksum_, raw->GetLocationChecksum());
}

TEST_F(DexFileTest, GetLocationChecksum) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> raw(OpenTestDexFile("Main"));
  EXPECT_NE(raw->GetHeader().checksum_, raw->GetLocationChecksum());
}

TEST_F(DexFileTest, GetChecksum) {
  uint32_t checksum;
  ScopedObjectAccess soa(Thread::Current());
  std::string error_msg;
  EXPECT_TRUE(DexFile::GetChecksum(GetLibCoreDexFileNames()[0].c_str(), &checksum, &error_msg))
      << error_msg;
  EXPECT_EQ(java_lang_dex_file_->GetLocationChecksum(), checksum);
}

TEST_F(DexFileTest, ClassDefs) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> raw(OpenTestDexFile("Nested"));
  ASSERT_TRUE(raw.get() != nullptr);
  EXPECT_EQ(2U, raw->NumClassDefs());

  const DexFile::ClassDef& c0 = raw->GetClassDef(0);
  EXPECT_STREQ("LNested$Inner;", raw->GetClassDescriptor(c0));

  const DexFile::ClassDef& c1 = raw->GetClassDef(1);
  EXPECT_STREQ("LNested;", raw->GetClassDescriptor(c1));
}

TEST_F(DexFileTest, GetMethodSignature) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> raw(OpenTestDexFile("GetMethodSignature"));
  ASSERT_TRUE(raw.get() != nullptr);
  EXPECT_EQ(1U, raw->NumClassDefs());

  const DexFile::ClassDef& class_def = raw->GetClassDef(0);
  ASSERT_STREQ("LGetMethodSignature;", raw->GetClassDescriptor(class_def));

  const uint8_t* class_data = raw->GetClassData(class_def);
  ASSERT_TRUE(class_data != nullptr);
  ClassDataItemIterator it(*raw, class_data);

  EXPECT_EQ(1u, it.NumDirectMethods());

  // Check the signature for the static initializer.
  {
    ASSERT_EQ(1U, it.NumDirectMethods());
    const DexFile::MethodId& method_id = raw->GetMethodId(it.GetMemberIndex());
    const char* name = raw->StringDataByIdx(method_id.name_idx_);
    ASSERT_STREQ("<init>", name);
    std::string signature(raw->GetMethodSignature(method_id).ToString());
    ASSERT_EQ("()V", signature);
  }

  // Check both virtual methods.
  ASSERT_EQ(2U, it.NumVirtualMethods());
  {
    it.Next();
    const DexFile::MethodId& method_id = raw->GetMethodId(it.GetMemberIndex());

    const char* name = raw->StringDataByIdx(method_id.name_idx_);
    ASSERT_STREQ("m1", name);

    std::string signature(raw->GetMethodSignature(method_id).ToString());
    ASSERT_EQ("(IDJLjava/lang/Object;)Ljava/lang/Float;", signature);
  }

  {
    it.Next();
    const DexFile::MethodId& method_id = raw->GetMethodId(it.GetMemberIndex());

    const char* name = raw->StringDataByIdx(method_id.name_idx_);
    ASSERT_STREQ("m2", name);

    std::string signature(raw->GetMethodSignature(method_id).ToString());
    ASSERT_EQ("(ZSC)LGetMethodSignature;", signature);
  }
}

TEST_F(DexFileTest, FindStringId) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> raw(OpenTestDexFile("GetMethodSignature"));
  ASSERT_TRUE(raw.get() != nullptr);
  EXPECT_EQ(1U, raw->NumClassDefs());

  const char* strings[] = { "LGetMethodSignature;", "Ljava/lang/Float;", "Ljava/lang/Object;",
      "D", "I", "J", nullptr };
  for (size_t i = 0; strings[i] != nullptr; i++) {
    const char* str = strings[i];
    const DexFile::StringId* str_id = raw->FindStringId(str);
    const char* dex_str = raw->GetStringData(*str_id);
    EXPECT_STREQ(dex_str, str);
  }
}

TEST_F(DexFileTest, FindTypeId) {
  for (size_t i = 0; i < java_lang_dex_file_->NumTypeIds(); i++) {
    const char* type_str = java_lang_dex_file_->StringByTypeIdx(i);
    const DexFile::StringId* type_str_id = java_lang_dex_file_->FindStringId(type_str);
    ASSERT_TRUE(type_str_id != nullptr);
    uint32_t type_str_idx = java_lang_dex_file_->GetIndexForStringId(*type_str_id);
    const DexFile::TypeId* type_id = java_lang_dex_file_->FindTypeId(type_str_idx);
    ASSERT_EQ(type_id, java_lang_dex_file_->FindTypeId(type_str));
    ASSERT_TRUE(type_id != nullptr);
    EXPECT_EQ(java_lang_dex_file_->GetIndexForTypeId(*type_id), i);
  }
}

TEST_F(DexFileTest, FindProtoId) {
  for (size_t i = 0; i < java_lang_dex_file_->NumProtoIds(); i++) {
    const DexFile::ProtoId& to_find = java_lang_dex_file_->GetProtoId(i);
    const DexFile::TypeList* to_find_tl = java_lang_dex_file_->GetProtoParameters(to_find);
    std::vector<uint16_t> to_find_types;
    if (to_find_tl != nullptr) {
      for (size_t j = 0; j < to_find_tl->Size(); j++) {
        to_find_types.push_back(to_find_tl->GetTypeItem(j).type_idx_);
      }
    }
    const DexFile::ProtoId* found =
        java_lang_dex_file_->FindProtoId(to_find.return_type_idx_, to_find_types);
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(java_lang_dex_file_->GetIndexForProtoId(*found), i);
  }
}

TEST_F(DexFileTest, FindMethodId) {
  for (size_t i = 0; i < java_lang_dex_file_->NumMethodIds(); i++) {
    const DexFile::MethodId& to_find = java_lang_dex_file_->GetMethodId(i);
    const DexFile::TypeId& klass = java_lang_dex_file_->GetTypeId(to_find.class_idx_);
    const DexFile::StringId& name = java_lang_dex_file_->GetStringId(to_find.name_idx_);
    const DexFile::ProtoId& signature = java_lang_dex_file_->GetProtoId(to_find.proto_idx_);
    const DexFile::MethodId* found = java_lang_dex_file_->FindMethodId(klass, name, signature);
    ASSERT_TRUE(found != nullptr) << "Didn't find method " << i << ": "
        << java_lang_dex_file_->StringByTypeIdx(to_find.class_idx_) << "."
        << java_lang_dex_file_->GetStringData(name)
        << java_lang_dex_file_->GetMethodSignature(to_find);
    EXPECT_EQ(java_lang_dex_file_->GetIndexForMethodId(*found), i);
  }
}

TEST_F(DexFileTest, FindFieldId) {
  for (size_t i = 0; i < java_lang_dex_file_->NumFieldIds(); i++) {
    const DexFile::FieldId& to_find = java_lang_dex_file_->GetFieldId(i);
    const DexFile::TypeId& klass = java_lang_dex_file_->GetTypeId(to_find.class_idx_);
    const DexFile::StringId& name = java_lang_dex_file_->GetStringId(to_find.name_idx_);
    const DexFile::TypeId& type = java_lang_dex_file_->GetTypeId(to_find.type_idx_);
    const DexFile::FieldId* found = java_lang_dex_file_->FindFieldId(klass, name, type);
    ASSERT_TRUE(found != nullptr) << "Didn't find field " << i << ": "
        << java_lang_dex_file_->StringByTypeIdx(to_find.type_idx_) << " "
        << java_lang_dex_file_->StringByTypeIdx(to_find.class_idx_) << "."
        << java_lang_dex_file_->GetStringData(name);
    EXPECT_EQ(java_lang_dex_file_->GetIndexForFieldId(*found), i);
  }
}

TEST_F(DexFileTest, GetMultiDexClassesDexName) {
  ASSERT_EQ("classes.dex", DexFile::GetMultiDexClassesDexName(0));
  ASSERT_EQ("classes2.dex", DexFile::GetMultiDexClassesDexName(1));
  ASSERT_EQ("classes3.dex", DexFile::GetMultiDexClassesDexName(2));
  ASSERT_EQ("classes100.dex", DexFile::GetMultiDexClassesDexName(99));
}

TEST_F(DexFileTest, GetMultiDexLocation) {
  std::string dex_location_str = "/system/app/framework.jar";
  const char* dex_location = dex_location_str.c_str();
  ASSERT_EQ("/system/app/framework.jar", DexFile::GetMultiDexLocation(0, dex_location));
  ASSERT_EQ("/system/app/framework.jar:classes2.dex",
            DexFile::GetMultiDexLocation(1, dex_location));
  ASSERT_EQ("/system/app/framework.jar:classes101.dex",
            DexFile::GetMultiDexLocation(100, dex_location));
}

TEST_F(DexFileTest, GetDexCanonicalLocation) {
  ScratchFile file;
  UniqueCPtr<const char[]> dex_location_real(realpath(file.GetFilename().c_str(), nullptr));
  std::string dex_location(dex_location_real.get());

  ASSERT_EQ(dex_location, DexFile::GetDexCanonicalLocation(dex_location.c_str()));
  std::string multidex_location = DexFile::GetMultiDexLocation(1, dex_location.c_str());
  ASSERT_EQ(multidex_location, DexFile::GetDexCanonicalLocation(multidex_location.c_str()));

  std::string dex_location_sym = dex_location + "symlink";
  ASSERT_EQ(0, symlink(dex_location.c_str(), dex_location_sym.c_str()));

  ASSERT_EQ(dex_location, DexFile::GetDexCanonicalLocation(dex_location_sym.c_str()));

  std::string multidex_location_sym = DexFile::GetMultiDexLocation(1, dex_location_sym.c_str());
  ASSERT_EQ(multidex_location, DexFile::GetDexCanonicalLocation(multidex_location_sym.c_str()));

  ASSERT_EQ(0, unlink(dex_location_sym.c_str()));
}

TEST(DexFileUtilsTest, GetBaseLocationAndMultiDexSuffix) {
  EXPECT_EQ("/foo/bar/baz.jar", DexFile::GetBaseLocation("/foo/bar/baz.jar"));
  EXPECT_EQ("/foo/bar/baz.jar", DexFile::GetBaseLocation("/foo/bar/baz.jar:classes2.dex"));
  EXPECT_EQ("/foo/bar/baz.jar", DexFile::GetBaseLocation("/foo/bar/baz.jar:classes8.dex"));
  EXPECT_EQ("", DexFile::GetMultiDexSuffix("/foo/bar/baz.jar"));
  EXPECT_EQ(":classes2.dex", DexFile::GetMultiDexSuffix("/foo/bar/baz.jar:classes2.dex"));
  EXPECT_EQ(":classes8.dex", DexFile::GetMultiDexSuffix("/foo/bar/baz.jar:classes8.dex"));
}

}  // namespace art
