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

#include "zip_archive.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>
#include <memory>

#include "base/unix_file/fd_file.h"
#include "common_runtime_test.h"
#include "os.h"

namespace art {

class ZipArchiveTest : public CommonRuntimeTest {};

TEST_F(ZipArchiveTest, FindAndExtract) {
  std::string error_msg;
  std::unique_ptr<ZipArchive> zip_archive(ZipArchive::Open(GetLibCoreDexFileNames()[0].c_str(), &error_msg));
  ASSERT_TRUE(zip_archive.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find("classes.dex", &error_msg));
  ASSERT_TRUE(zip_entry.get() != nullptr);
  ASSERT_TRUE(error_msg.empty());

  ScratchFile tmp;
  ASSERT_NE(-1, tmp.GetFd());
  std::unique_ptr<File> file(new File(tmp.GetFd(), tmp.GetFilename(), false));
  ASSERT_TRUE(file.get() != nullptr);
  bool success = zip_entry->ExtractToFile(*file, &error_msg);
  ASSERT_TRUE(success) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  file.reset(nullptr);

  uint32_t computed_crc = crc32(0L, Z_NULL, 0);
  int fd = open(tmp.GetFilename().c_str(), O_RDONLY);
  ASSERT_NE(-1, fd);
  const size_t kBufSize = 32768;
  uint8_t buf[kBufSize];
  while (true) {
    ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd, buf, kBufSize));
    if (bytes_read == 0) {
      break;
    }
    computed_crc = crc32(computed_crc, buf, bytes_read);
  }
  EXPECT_EQ(zip_entry->GetCrc32(), computed_crc);
}

}  // namespace art
