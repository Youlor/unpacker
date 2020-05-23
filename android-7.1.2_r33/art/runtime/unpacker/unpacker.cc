#include "unpacker.h"
#include "base/macros.h"
#include "globals.h"
#include "instrumentation.h"
#include "art_method-inl.h"
#include "thread.h"
#include "reflection.h"
#include "object_lock.h"

#include <android/log.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string>

#define ULOG_TAG "unpacker"
#define TOSTR(fmt) #fmt
#define UFMT TOSTR([%s:%d])

#define ULOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, ULOG_TAG, UFMT fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define ULOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, ULOG_TAG, UFMT fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define ULOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, ULOG_TAG, UFMT fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define ULOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, ULOG_TAG, UFMT fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define ULOGV(fmt, ...) __android_log_print(ANDROID_LOG_VERBOSE, ULOG_TAG, UFMT fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)


#define UNPACKER_WORKSPACE "unpacker"

namespace art {

static bool Unpacker_fake_invoke_ = false;
static bool Unpacker_real_invoke_ = false;
static Thread* Unpacker_self_ = nullptr;
static std::string Unpacker_dump_dir_;
static std::string Unpacker_dex_dir_;
static std::string Unpacker_method_dir_;
static std::string Unpacker_json_path_;
static int Unpacker_json_fd_ = -1;
static cJSON* Unpacker_json_ = nullptr;
static std::list<const DexFile*> Unpacker_dex_files_;
static mirror::ClassLoader* Unpacker_class_loader_ = nullptr;
static std::map<std::string, int> Unpacker_method_fds_;

std::string Unpacker::getDumpDir() {
  Thread* const self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();
  jclass cls_ActivityThread = env->FindClass("android/app/ActivityThread");
  jmethodID mid_currentActivityThread = env->GetStaticMethodID(cls_ActivityThread, "currentActivityThread", "()Landroid/app/ActivityThread;");
  jobject obj_ActivityThread = env->CallStaticObjectMethod(cls_ActivityThread, mid_currentActivityThread);
  jfieldID fid_mInitialApplication = env->GetFieldID(cls_ActivityThread, "mInitialApplication", "Landroid/app/Application;");
  jobject obj_mInitialApplication = env->GetObjectField(obj_ActivityThread, fid_mInitialApplication);
  jclass cls_Context = env->FindClass("android/content/Context");
  jmethodID mid_getApplicationInfo = env->GetMethodID(cls_Context, "getApplicationInfo",
                                                      "()Landroid/content/pm/ApplicationInfo;");
  jobject obj_app_info = env->CallObjectMethod(obj_mInitialApplication, mid_getApplicationInfo);
  jclass cls_ApplicationInfo = env->FindClass("android/content/pm/ApplicationInfo");
  jfieldID fid_dataDir = env->GetFieldID(cls_ApplicationInfo, "dataDir", "Ljava/lang/String;");
  jstring dataDir = (jstring)env->GetObjectField(obj_app_info, fid_dataDir);
  const char *cstr_dataDir = env->GetStringUTFChars(dataDir, nullptr);
  std::string dump_dir(cstr_dataDir);
  dump_dir += "/";
  dump_dir += UNPACKER_WORKSPACE;
  env->ReleaseStringUTFChars(dataDir, cstr_dataDir);
  return dump_dir;
}

std::string Unpacker::getDexDumpPath(const DexFile* dex_file) {
  std::string dex_location = dex_file->GetLocation();
  size_t size = dex_file->Size();
  //替换windows文件不支持的字符
  for (size_t i = 0; i < dex_location.length(); i++) {
    if (dex_location[i] == '/' || dex_location[i] == ':') {
      dex_location[i] = '_';
    }
  }
  std::string dump_path = Unpacker_dex_dir_ + "/" + dex_location;
  dump_path += StringPrintf("_%zu.dex", size);
  return dump_path;
}

std::string Unpacker::getMethodDumpPath(ArtMethod* method) {
  CHECK(method->GetDeclaringClass() != nullptr) << method;
  const DexFile& dex_file = method->GetDeclaringClass()->GetDexFile();
  std::string dex_location = dex_file.GetLocation();
  size_t size = dex_file.Size();
  //替换windows文件不支持的字符
  for (size_t i = 0; i < dex_location.length(); i++) {
    if (dex_location[i] == '/' || dex_location[i] == ':') {
      dex_location[i] = '_';
    }
  }
  std::string dump_path = Unpacker_method_dir_ + "/" + dex_location;
  dump_path += StringPrintf("_%zu_codeitem.bin", size);
  return dump_path;
}

cJSON* Unpacker::createJson() {
  cJSON *json;
  cJSON *dexes;

  json = cJSON_CreateObject();
  if (json == nullptr) {
      goto bail;
  }
  dexes = cJSON_AddArrayToObject(json, "dexes");
  if (dexes == nullptr) {
      goto bail;
  }
bail:
  return json;
}

cJSON* Unpacker::parseJson() {
  if (Unpacker_json_fd_ == -1) {
    return nullptr;
  }

  lseek(Unpacker_json_fd_, 0, SEEK_SET);
  struct stat json_stat = {};
  if (fstat(Unpacker_json_fd_, &json_stat)) {
    ULOGE("fstat error: %s", strerror(errno));
    return nullptr;
  }
  int size = (int)json_stat.st_size;
  if (size == 0) {
    return nullptr;
  }

  char* buf = new char[size];
  ssize_t read_size = read(Unpacker_json_fd_, buf, size);
  if (read_size != (ssize_t)size) {
    ULOGW("fread %s %zd/%d error: %s", Unpacker_json_path_.c_str(), read_size, size, strerror(errno));
  }
  cJSON *json = cJSON_Parse(buf);
  if (json == nullptr) {
      const char *error_ptr = cJSON_GetErrorPtr();
      if (error_ptr != nullptr) {
        ULOGE("cJSON_Parse error: %s", error_ptr);
      }
  }
  delete[] buf;
  return json;
}

void Unpacker::writeJson() {
  if (Unpacker_json_fd_ == -1) {
    return;
  }
  lseek(Unpacker_json_fd_, 0, SEEK_SET);
  int fail = ftruncate(Unpacker_json_fd_, 0);
  if (fail) {
    ULOGW("ftruncate %s error: %s", Unpacker_json_path_.c_str(), strerror(errno));
  }
  char* json_str = cJSON_Print(Unpacker_json_);
  CHECK(json_str != nullptr);
  ssize_t written_size = write(Unpacker_json_fd_, json_str, strlen(json_str));
  if (written_size != (ssize_t)strlen(json_str)) {
    ULOGW("fwrite %s %zd/%zu error: %s", Unpacker_json_path_.c_str(), written_size, strlen(json_str), strerror(errno));
  }
  free(json_str);
}

std::list<const DexFile*> Unpacker::getDexFiles() {
  std::list<const DexFile*> dex_files;
  Thread* const self = Thread::Current();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ReaderMutexLock mu(self, *class_linker->DexLock());
  const std::list<ClassLinker::DexCacheData>& dex_caches = class_linker->GetDexCachesData();
  for (auto it = dex_caches.begin(); it != dex_caches.end(); ++it) {
    ClassLinker::DexCacheData data = *it;
    const DexFile* dex_file = data.dex_file;
    const std::string& dex_location = dex_file->GetLocation();
    if (dex_location.rfind("/system/", 0) == 0) {
      continue;
    }
    dex_files.push_back(dex_file);
  }
  return dex_files;
}

mirror::ClassLoader* Unpacker::getAppClassLoader() {
  Thread* const self = Thread::Current();
  ScopedObjectAccessUnchecked soa(self);
  JNIEnv* env = self->GetJniEnv();
  jclass cls_ActivityThread = env->FindClass("android/app/ActivityThread");
  jmethodID mid_currentActivityThread = env->GetStaticMethodID(cls_ActivityThread, "currentActivityThread", "()Landroid/app/ActivityThread;");
  jobject obj_ActivityThread = env->CallStaticObjectMethod(cls_ActivityThread, mid_currentActivityThread);
  jfieldID fid_mInitialApplication = env->GetFieldID(cls_ActivityThread, "mInitialApplication", "Landroid/app/Application;");
  jobject obj_mInitialApplication = env->GetObjectField(obj_ActivityThread, fid_mInitialApplication);
  jclass cls_Context = env->FindClass("android/content/Context");
  jmethodID mid_getClassLoader = env->GetMethodID(cls_Context, "getClassLoader", "()Ljava/lang/ClassLoader;");
  jobject obj_classLoader = env->CallObjectMethod(obj_mInitialApplication, mid_getClassLoader);
  return soa.Decode<mirror::ClassLoader*>(obj_classLoader);
}

void Unpacker::invokeAllMethods() {
  //dump类的六种status: 
  //Ready: 该类准备dump
  //Resolved: ResolveClass成功
  //ResolveClassFailed: ResolveClass失败
  //Inited: EnsureInitialized成功
  //EnsureInitializedFailed: EnsureInitialized失败
  //Dumped: dump所有method成功

  Thread* const self = Thread::Current();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  for (const DexFile* dex_file : Unpacker_dex_files_) {
    uint32_t class_idx = 0;
    bool skip_clinit = false;
    cJSON* dex = nullptr;
    cJSON* current = nullptr;
    cJSON* failures = nullptr;
    cJSON* dexes = cJSON_GetObjectItemCaseSensitive(Unpacker_json_, "dexes");
    CHECK(dexes != nullptr);
    cJSON_ArrayForEach(dex, dexes) {
      cJSON *location = cJSON_GetObjectItemCaseSensitive(dex, "location");
      cJSON *dump_path = cJSON_GetObjectItemCaseSensitive(dex, "dump_path");
      cJSON *class_size = cJSON_GetObjectItemCaseSensitive(dex, "class_size");
      char* location_str = cJSON_GetStringValue(location);
      char* dump_path_str = cJSON_GetStringValue(dump_path);
      uint32_t class_size_num = cJSON_GetNumberValue(class_size);
      if (strcmp(location_str, dex_file->GetLocation().c_str()) == 0
        && strcmp(dump_path_str, getDexDumpPath(dex_file).c_str()) == 0
        && class_size_num == dex_file->NumClassDefs()) {
        // 已经处理过的dex
        current = cJSON_GetObjectItemCaseSensitive(dex, "current");
        failures = cJSON_GetObjectItemCaseSensitive(dex, "failures");
        cJSON *index = cJSON_GetObjectItemCaseSensitive(current, "index");
        cJSON *descriptor = cJSON_GetObjectItemCaseSensitive(current, "descriptor");
        cJSON *status = cJSON_GetObjectItemCaseSensitive(current, "status");
        uint32_t index_num = cJSON_GetNumberValue(index);
        char* descriptor_str = cJSON_GetStringValue(descriptor);
        char* status_str = cJSON_GetStringValue(status);
        CHECK(strcmp(descriptor_str, dex_file->GetClassDescriptor(dex_file->GetClassDef(index_num))) == 0);
        
        if (strcmp(status_str, "Resolved") == 0) {
          //如果status为Resolved, 说明进程在EnsureInitialized时结束了, 很大可能是<clinit>调用时进程崩溃/退出, 则不调用<clinit>而直接dump method
          skip_clinit = true;
          class_idx = index_num;
        } else if (strcmp(status_str, "Ready") == 0) {
          class_idx = index_num;
        } else {
          class_idx = index_num + 1;
        }
        break;
      }
    }

    if (dex == nullptr) {
      dex = cJSON_CreateObject();
      cJSON_AddStringToObject(dex, "location", dex_file->GetLocation().c_str());
      cJSON_AddStringToObject(dex, "dump_path", getDexDumpPath(dex_file).c_str());
      cJSON_AddNumberToObject(dex, "class_size", dex_file->NumClassDefs());
      current = cJSON_AddObjectToObject(dex, "current");
      cJSON_AddNumberToObject(current, "index", class_idx);
      cJSON_AddStringToObject(current, "descriptor", dex_file->GetClassDescriptor(dex_file->GetClassDef(class_idx)));
      cJSON_AddStringToObject(current, "status", "Ready");
      failures = cJSON_AddArrayToObject(dex, "failures");
      cJSON_AddItemToArray(dexes, dex);
    }
    CHECK(current != nullptr);

    mirror::DexCache* dex_cache = class_linker->FindDexCache(self, *dex_file, false);
    StackHandleScope<2> hs(self);
    Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(Unpacker_class_loader_));
    Handle<mirror::DexCache> h_dex_cache(hs.NewHandle(dex_cache));
    
    for (; class_idx < dex_file->NumClassDefs(); class_idx++) {
      const char* class_descriptor = dex_file->GetClassDescriptor(dex_file->GetClassDef(class_idx));
      ULOGI("dumping class %s %u/%u in %s", class_descriptor, 
            class_idx, dex_file->NumClassDefs(), dex_file->GetLocation().c_str());

      //Ready
      cJSON_ReplaceItemInObject(current, "index", cJSON_CreateNumber(class_idx));
      cJSON_ReplaceItemInObject(current, "descriptor", cJSON_CreateString(class_descriptor));
      cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("Ready"));
      writeJson();

      mirror::Class* klass = class_linker->ResolveType(*dex_file, dex_file->GetClassDef(class_idx).class_idx_, h_dex_cache, h_class_loader);
      if (klass == nullptr) {
        cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("ResolveClassFailed"));
        std::string reason = StringPrintf("ResolveClass error: %s", self->GetException()->Dump().c_str());
        cJSON *failure = cJSON_CreateObject();
        cJSON_AddNumberToObject(failure, "index", class_idx);
        cJSON_AddStringToObject(failure, "descriptor", dex_file->GetClassDescriptor(dex_file->GetClassDef(class_idx)));
        cJSON_AddStringToObject(failure, "reason", reason.c_str());
        cJSON_AddItemToArray(failures, failure);
        writeJson();
        self->ClearException();
        skip_clinit = false;
        continue;
      }
      cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("Resolved"));
      writeJson();
      StackHandleScope<1> hs2(self);
      Handle<mirror::Class> h_class(hs2.NewHandle(klass));
      if (!skip_clinit) {
        bool suc = class_linker->EnsureInitialized(self, h_class, true, true);
        if (!suc) {
          cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("EnsureInitializedFailed"));
          writeJson();
          self->ClearException();
          ObjectLock<mirror::Class> lock(self, h_class);
          mirror::Class::SetStatus(h_class, mirror::Class::kStatusInitialized, self);
        } else {
          cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("Inited"));
          writeJson();
        }
      } else {
        ObjectLock<mirror::Class> lock(self, h_class);
        mirror::Class::SetStatus(h_class, mirror::Class::kStatusInitialized, self);
        skip_clinit = false;
        cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("Inited"));
        writeJson();
      }
      
      size_t pointer_size = class_linker->GetImagePointerSize();
      auto methods = klass->GetDeclaredMethods(pointer_size);

      Unpacker::enableFakeInvoke();
      for (auto& m : methods) {
        ArtMethod* method = &m;
        if (!method->IsProxyMethod() && method->IsInvokable()) {
          uint32_t args_size = (uint32_t)ArtMethod::NumArgRegisters(method->GetShorty());
          if (!method->IsStatic()) {
            args_size += 1;
          }
          
          JValue result;
          std::vector<uint32_t> args(args_size, 0);
          if (!method->IsStatic()) {
            mirror::Object* thiz = klass->AllocObject(self);
            args[0] = StackReference<mirror::Object>::FromMirrorPtr(thiz).AsVRegValue();  
          }
          method->Invoke(self, args.data(), args_size, &result, method->GetShorty());
        }
      }
      Unpacker::disableFakeInvoke();

      cJSON_ReplaceItemInObject(current, "status", cJSON_CreateString("Dumped"));
      writeJson();
    }
  }
}

void Unpacker::dumpAllDexes() {
  for (const DexFile* dex_file : Unpacker_dex_files_) {
    std::string dump_path = getDexDumpPath(dex_file);
    if (access(dump_path.c_str(), F_OK) != -1) {
      ULOGI("%s already dumped, ignored", dump_path.c_str());
      continue;
    }
    const uint8_t* begin = dex_file->Begin();
    size_t size = dex_file->Size();
    int fd = open(dump_path.c_str(), O_RDWR | O_CREAT, 0777);
    if (fd == -1) {
      ULOGE("open %s error: %s", dump_path.c_str(), strerror(errno));
      continue;
    }

    std::vector<uint8_t> data(size);
    memcpy(data.data(), "dex\n035", 8);
    memcpy(data.data() + 8, begin + 8, size - 8);

    size_t written_size = write(fd, data.data(), size);
    if (written_size < size) {
      ULOGW("fwrite %s %zu/%zu error: %s", dump_path.c_str(), written_size, size, strerror(errno));
    }
    close(fd);
    ULOGI("dump dex %s to %s successful!", dex_file->GetLocation().c_str(), dump_path.c_str());
  }
}

void Unpacker::init() {
  Unpacker_fake_invoke_ = false;
  Unpacker_self_ = Thread::Current();
  Unpacker_dump_dir_ = getDumpDir();
  mkdir(Unpacker_dump_dir_.c_str(), 0777);
  Unpacker_dex_dir_ = getDumpDir() + "/dex";
  mkdir(Unpacker_dex_dir_.c_str(), 0777);
  Unpacker_method_dir_ = getDumpDir() + "/method";
  mkdir(Unpacker_method_dir_.c_str(), 0777);
  Unpacker_json_path_ = getDumpDir() + "/unpacker.json";
  Unpacker_json_fd_ = -1;
  Unpacker_json_fd_ = open(Unpacker_json_path_.c_str(), O_RDWR | O_CREAT, 0777);
  if (Unpacker_json_fd_ == -1) {
    ULOGE("open %s error: %s", Unpacker_json_path_.c_str(), strerror(errno));
  }
  Unpacker_json_ = parseJson();
  if (Unpacker_json_ == nullptr) {
    Unpacker_json_ = createJson();
  }
  CHECK(Unpacker_json_ != nullptr);

  Unpacker_dex_files_ = getDexFiles();
  Unpacker_class_loader_ = getAppClassLoader();
}

void Unpacker::fini() {
  Unpacker_fake_invoke_ = false;
  Unpacker_real_invoke_ = false;
  Unpacker_self_ = nullptr;
  if (Unpacker_json_fd_ != -1) {
    close(Unpacker_json_fd_);
  }
  for(auto iter = Unpacker_method_fds_.begin(); iter != Unpacker_method_fds_.end(); iter++) {
    close(iter->second);
  }
  cJSON_Delete(Unpacker_json_);
}

void Unpacker::unpack() {
  ScopedObjectAccess soa(Thread::Current());
  ULOGI("%s", "unpack begin!");
  //1. 初始化
  init();
  //2. dump所有dex
  dumpAllDexes();
  //3. 主动调用所有方法
  invokeAllMethods();
  //4. 还原
  fini();
  ULOGI("%s", "unpack end!");
}

void Unpacker::enableFakeInvoke() {
  Unpacker_fake_invoke_ = true;
}

void Unpacker::disableFakeInvoke() {
  Unpacker_fake_invoke_ = false;
}

bool Unpacker::isFakeInvoke(Thread *self, ArtMethod */*method*/) {
  if (Unpacker_fake_invoke_ && self == Unpacker_self_) {
      return true;
  }
  return false;
}

void Unpacker::enableRealInvoke() {
  Unpacker_real_invoke_ = true;
}

void Unpacker::disableRealInvoke() {
  Unpacker_real_invoke_ = false;
}

bool Unpacker::isRealInvoke(Thread *self, ArtMethod */*method*/) {
  if (Unpacker_real_invoke_ && self == Unpacker_self_) {
      return true;
  }
  return false;
}

size_t Unpacker::getCodeItemSize(ArtMethod* method) {
  const DexFile::CodeItem* code_item = method->GetCodeItem();
  size_t size = offsetof(DexFile::CodeItem, insns_);
  size += code_item->insns_size_in_code_units_ * sizeof(uint16_t);

  if (code_item->tries_size_ != 0) {
    if (code_item->insns_size_in_code_units_ % 2 != 0) {
      //使 tries 实现四字节对齐的两字节填充. 仅当 tries_size 为非零值且 insns_size 为奇数时, 此元素才会存在
      uint16_t padding = 2;
      size += padding;
    }
    size += sizeof(DexFile::TryItem) * code_item->tries_size_;
    const uint8_t* data = (uint8_t *)code_item + size;
  
    uint32_t handlers_size = DecodeUnsignedLeb128(&data);
    size += UnsignedLeb128Size(handlers_size);
    for (uint32_t handler_index = 0; handler_index < handlers_size; handler_index++) {
      data = (uint8_t *)code_item + size;
      int32_t handler_data_size = DecodeSignedLeb128(&data);
      size += SignedLeb128Size(handler_data_size);
      for (int32_t handler_data_index = 0; handler_data_index < abs(handler_data_size); handler_data_index++) {
        data = (uint8_t *)code_item + size;
        size += UnsignedLeb128Size(DecodeUnsignedLeb128(&data));
        data = (uint8_t *)code_item + size;
        size += UnsignedLeb128Size(DecodeUnsignedLeb128(&data));
      }
      if (handler_data_size <= 0) {
        data = (uint8_t *)code_item + size;
        size += UnsignedLeb128Size(DecodeUnsignedLeb128(&data));
      }
    }
  }

  return size;
}

void Unpacker::dumpMethod(ArtMethod *method, int nop_size) {
  std::string dump_path = Unpacker::getMethodDumpPath(method);
  int fd = -1;
  if (Unpacker_method_fds_.find(dump_path) != Unpacker_method_fds_.end()) {
    fd = Unpacker_method_fds_[dump_path];
  }
  else {
    fd = open(dump_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0777);
    if (fd == -1) {
      ULOGE("open %s error: %s", dump_path.c_str(), strerror(errno));
      return;
    }
    Unpacker_method_fds_[dump_path] = fd;
  }

  uint32_t index = method->GetDexMethodIndex();
  std::string str_name = PrettyMethod(method);
  const char* name = str_name.c_str();
  const DexFile::CodeItem* code_item = method->GetCodeItem();
  uint32_t code_item_size = (uint32_t)Unpacker::getCodeItemSize(method);

  size_t total_size = 4 + strlen(name) + 1 + 4 + code_item_size;
  std::vector<uint8_t> data(total_size);
  uint8_t* buf = data.data();
  memcpy(buf, &index, 4);
  buf += 4;
  memcpy(buf, name, strlen(name) + 1);
  buf += strlen(name) + 1;
  memcpy(buf, &code_item_size, 4);
  buf += 4;
  memcpy(buf, code_item, code_item_size);
  if (nop_size != 0) {
    memset(buf + offsetof(DexFile::CodeItem, insns_), 0, nop_size);
  }

  ssize_t written_size = write(fd, data.data(), total_size);
  if (written_size > (ssize_t)total_size) {
    ULOGW("write %s in %s %zd/%zu error: %s", PrettyMethod(method).c_str(), dump_path.c_str(), written_size, total_size, strerror(errno));
  }
}

//继续解释执行返回false, dump完成返回true
bool Unpacker::beforeInstructionExecute(Thread *self, ArtMethod *method, uint32_t dex_pc, int inst_count) {
  if (Unpacker::isFakeInvoke(self, method)) {
    const uint16_t* const insns = method->GetCodeItem()->insns_;
    const Instruction* inst = Instruction::At(insns + dex_pc);
    uint16_t inst_data = inst->Fetch16(0);
    Instruction::Code opcode = inst->Opcode(inst_data);

    //对于一般的方法抽取(非ijiami, najia), 直接在第一条指令处dump即可
    if (inst_count == 0 && opcode != Instruction::GOTO && opcode != Instruction::GOTO_16 && opcode != Instruction::GOTO_32) {
      Unpacker::dumpMethod(method);
      return true;
    }
    //ijiami, najia的特征为: goto: goto_decrypt; nop; ... ; return; const vx, n; invoke-static xxx; goto: goto_origin;
    else if (inst_count == 0 && opcode >= Instruction::GOTO && opcode <= Instruction::GOTO_32) {
      return false;
    } else if (inst_count == 1 && opcode >= Instruction::CONST_4 && opcode <= Instruction::CONST_WIDE_HIGH16) {
      return false;
    } else if (inst_count == 2 && (opcode == Instruction::INVOKE_STATIC || opcode == Instruction::INVOKE_STATIC_RANGE)) {
      //让这条指令真正的执行
      Unpacker::disableFakeInvoke();
      Unpacker::enableRealInvoke();
      return false;
    } else if (inst_count == 3) {
      if (opcode >= Instruction::GOTO && opcode <= Instruction::GOTO_32) {
        //写入时将第一条GOTO用nop填充
        const Instruction* inst_first = Instruction::At(insns);
        Instruction::Code first_opcode = inst_first->Opcode(inst->Fetch16(0));
        CHECK(first_opcode >= Instruction::GOTO && first_opcode <= Instruction::GOTO_32);
        ULOGD("found najia/ijiami %s", PrettyMethod(method).c_str());
        switch (first_opcode)
        {
        case Instruction::GOTO:
          Unpacker::dumpMethod(method, 2);
          break;
        case Instruction::GOTO_16:
          Unpacker::dumpMethod(method, 4);
          break;
        case Instruction::GOTO_32:
          Unpacker::dumpMethod(method, 8);
          break;
        default:
          break;
        }
      } else {
        Unpacker::dumpMethod(method);
      }
      return true;
    }
    Unpacker::dumpMethod(method);
    return true;
  }
  return false;
}

bool Unpacker::afterInstructionExecute(Thread *self, ArtMethod *method, uint32_t dex_pc, int inst_count) {
  const uint16_t* const insns = method->GetCodeItem()->insns_;
  const Instruction* inst = Instruction::At(insns + dex_pc);
  uint16_t inst_data = inst->Fetch16(0);
  Instruction::Code opcode = inst->Opcode(inst_data);
  if (inst_count == 2 && (opcode == Instruction::INVOKE_STATIC || opcode == Instruction::INVOKE_STATIC_RANGE) 
      && Unpacker::isRealInvoke(self, method)) {
    Unpacker::enableFakeInvoke();
    Unpacker::disableRealInvoke();
  }
  return false;
}

//注册native方法

static void Unpacker_unpackNative(JNIEnv*, jclass) {
  Unpacker::unpack();
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Unpacker, unpackNative, "()V")
};

void Unpacker::register_cn_youlor_Unpacker(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("cn/youlor/Unpacker");
}

}

