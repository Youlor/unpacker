/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "dalvik_system_VMRuntime.h"

#ifdef __ANDROID__
extern "C" void android_set_application_target_sdk_version(uint32_t version);
#endif
#include <limits.h>
#include <ScopedUtfChars.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "toStringArray.h"
#pragma GCC diagnostic pop

#include "art_method-inl.h"
#include "arch/instruction_set.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/allocator/dlmalloc.h"
#include "gc/heap.h"
#include "gc/space/dlmalloc_space.h"
#include "gc/space/image_space.h"
#include "gc/task_processor.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "runtime.h"
#include "scoped_fast_native_object_access.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"

namespace art {

static jfloat VMRuntime_getTargetHeapUtilization(JNIEnv*, jobject) {
  return Runtime::Current()->GetHeap()->GetTargetHeapUtilization();
}

static void VMRuntime_nativeSetTargetHeapUtilization(JNIEnv*, jobject, jfloat target) {
  Runtime::Current()->GetHeap()->SetTargetHeapUtilization(target);
}

static void VMRuntime_startJitCompilation(JNIEnv*, jobject) {
}

static void VMRuntime_disableJitCompilation(JNIEnv*, jobject) {
}

static jobject VMRuntime_newNonMovableArray(JNIEnv* env, jobject, jclass javaElementClass,
                                            jint length) {
  ScopedFastNativeObjectAccess soa(env);
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return nullptr;
  }
  mirror::Class* element_class = soa.Decode<mirror::Class*>(javaElementClass);
  if (UNLIKELY(element_class == nullptr)) {
    ThrowNullPointerException("element class == null");
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  mirror::Class* array_class =
      runtime->GetClassLinker()->FindArrayClass(soa.Self(), &element_class);
  if (UNLIKELY(array_class == nullptr)) {
    return nullptr;
  }
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentNonMovingAllocator();
  mirror::Array* result = mirror::Array::Alloc<true>(soa.Self(), array_class, length,
                                                     array_class->GetComponentSizeShift(),
                                                     allocator);
  return soa.AddLocalReference<jobject>(result);
}

static jobject VMRuntime_newUnpaddedArray(JNIEnv* env, jobject, jclass javaElementClass,
                                          jint length) {
  ScopedFastNativeObjectAccess soa(env);
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return nullptr;
  }
  mirror::Class* element_class = soa.Decode<mirror::Class*>(javaElementClass);
  if (UNLIKELY(element_class == nullptr)) {
    ThrowNullPointerException("element class == null");
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  mirror::Class* array_class = runtime->GetClassLinker()->FindArrayClass(soa.Self(),
                                                                         &element_class);
  if (UNLIKELY(array_class == nullptr)) {
    return nullptr;
  }
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  mirror::Array* result = mirror::Array::Alloc<true, true>(soa.Self(), array_class, length,
                                                           array_class->GetComponentSizeShift(),
                                                           allocator);
  return soa.AddLocalReference<jobject>(result);
}

static jlong VMRuntime_addressOf(JNIEnv* env, jobject, jobject javaArray) {
  if (javaArray == nullptr) {  // Most likely allocation failed
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  mirror::Array* array = soa.Decode<mirror::Array*>(javaArray);
  if (!array->IsArrayInstance()) {
    ThrowIllegalArgumentException("not an array");
    return 0;
  }
  if (Runtime::Current()->GetHeap()->IsMovableObject(array)) {
    ThrowRuntimeException("Trying to get address of movable array object");
    return 0;
  }
  return reinterpret_cast<uintptr_t>(array->GetRawData(array->GetClass()->GetComponentSize(), 0));
}

static void VMRuntime_clearGrowthLimit(JNIEnv*, jobject) {
  Runtime::Current()->GetHeap()->ClearGrowthLimit();
}

static void VMRuntime_clampGrowthLimit(JNIEnv*, jobject) {
  Runtime::Current()->GetHeap()->ClampGrowthLimit();
}

static jboolean VMRuntime_isDebuggerActive(JNIEnv*, jobject) {
  return Dbg::IsDebuggerActive();
}

static jboolean VMRuntime_isNativeDebuggable(JNIEnv*, jobject) {
  return Runtime::Current()->IsNativeDebuggable();
}

static jobjectArray VMRuntime_properties(JNIEnv* env, jobject) {
  return toStringArray(env, Runtime::Current()->GetProperties());
}

// This is for backward compatibility with dalvik which returned the
// meaningless "." when no boot classpath or classpath was
// specified. Unfortunately, some tests were using java.class.path to
// lookup relative file locations, so they are counting on this to be
// ".", presumably some applications or libraries could have as well.
static const char* DefaultToDot(const std::string& class_path) {
  return class_path.empty() ? "." : class_path.c_str();
}

static jstring VMRuntime_bootClassPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetBootClassPathString()));
}

static jstring VMRuntime_classPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetClassPathString()));
}

static jstring VMRuntime_vmVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(Runtime::GetVersion());
}

static jstring VMRuntime_vmLibrary(JNIEnv* env, jobject) {
  return env->NewStringUTF(kIsDebugBuild ? "libartd.so" : "libart.so");
}

static jstring VMRuntime_vmInstructionSet(JNIEnv* env, jobject) {
  InstructionSet isa = Runtime::Current()->GetInstructionSet();
  const char* isa_string = GetInstructionSetString(isa);
  return env->NewStringUTF(isa_string);
}

static jboolean VMRuntime_is64Bit(JNIEnv*, jobject) {
  bool is64BitMode = (sizeof(void*) == sizeof(uint64_t));
  return is64BitMode ? JNI_TRUE : JNI_FALSE;
}

static jboolean VMRuntime_isCheckJniEnabled(JNIEnv* env, jobject) {
  return down_cast<JNIEnvExt*>(env)->vm->IsCheckJniEnabled() ? JNI_TRUE : JNI_FALSE;
}

static void VMRuntime_setTargetSdkVersionNative(JNIEnv*, jobject, jint target_sdk_version) {
  // This is the target SDK version of the app we're about to run. It is intended that this a place
  // where workarounds can be enabled.
  // Note that targetSdkVersion may be CUR_DEVELOPMENT (10000).
  // Note that targetSdkVersion may be 0, meaning "current".
  Runtime::Current()->SetTargetSdkVersion(target_sdk_version);

#ifdef __ANDROID__
  // This part is letting libc/dynamic linker know about current app's
  // target sdk version to enable compatibility workarounds.
  android_set_application_target_sdk_version(static_cast<uint32_t>(target_sdk_version));
#endif
}

static void VMRuntime_registerNativeAllocation(JNIEnv* env, jobject, jint bytes) {
  if (UNLIKELY(bytes < 0)) {
    ScopedObjectAccess soa(env);
    ThrowRuntimeException("allocation size negative %d", bytes);
    return;
  }
  Runtime::Current()->GetHeap()->RegisterNativeAllocation(env, static_cast<size_t>(bytes));
}

static void VMRuntime_registerSensitiveThread(JNIEnv*, jobject) {
  Runtime::Current()->RegisterSensitiveThread();
}

static void VMRuntime_registerNativeFree(JNIEnv* env, jobject, jint bytes) {
  if (UNLIKELY(bytes < 0)) {
    ScopedObjectAccess soa(env);
    ThrowRuntimeException("allocation size negative %d", bytes);
    return;
  }
  Runtime::Current()->GetHeap()->RegisterNativeFree(env, static_cast<size_t>(bytes));
}

static void VMRuntime_updateProcessState(JNIEnv*, jobject, jint process_state) {
  Runtime* runtime = Runtime::Current();
  runtime->UpdateProcessState(static_cast<ProcessState>(process_state));
}

static void VMRuntime_trimHeap(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->Trim(ThreadForEnv(env));
}

static void VMRuntime_concurrentGC(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->ConcurrentGC(ThreadForEnv(env), true);
}

static void VMRuntime_requestHeapTrim(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->RequestTrim(ThreadForEnv(env));
}

static void VMRuntime_requestConcurrentGC(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->RequestConcurrentGC(ThreadForEnv(env), true);
}

static void VMRuntime_startHeapTaskProcessor(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->GetTaskProcessor()->Start(ThreadForEnv(env));
}

static void VMRuntime_stopHeapTaskProcessor(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->GetTaskProcessor()->Stop(ThreadForEnv(env));
}

static void VMRuntime_runHeapTasks(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->GetTaskProcessor()->RunAllTasks(ThreadForEnv(env));
}

typedef std::map<std::string, mirror::String*> StringTable;

class PreloadDexCachesStringsVisitor : public SingleRootVisitor {
 public:
  explicit PreloadDexCachesStringsVisitor(StringTable* table) : table_(table) { }

  void VisitRoot(mirror::Object* root, const RootInfo& info ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    mirror::String* string = root->AsString();
    table_->operator[](string->ToModifiedUtf8()) = string;
  }

 private:
  StringTable* const table_;
};

// Based on ClassLinker::ResolveString.
static void PreloadDexCachesResolveString(
    Handle<mirror::DexCache> dex_cache, uint32_t string_idx, StringTable& strings)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::String* string = dex_cache->GetResolvedString(string_idx);
  if (string != nullptr) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const char* utf8 = dex_file->StringDataByIdx(string_idx);
  string = strings[utf8];
  if (string == nullptr) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved string=" << utf8;
  dex_cache->SetResolvedString(string_idx, string);
}

// Based on ClassLinker::ResolveType.
static void PreloadDexCachesResolveType(
    Thread* self, mirror::DexCache* dex_cache, uint32_t type_idx)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Class* klass = dex_cache->GetResolvedType(type_idx);
  if (klass != nullptr) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const char* class_name = dex_file->StringByTypeIdx(type_idx);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  if (class_name[1] == '\0') {
    klass = linker->FindPrimitiveClass(class_name[0]);
  } else {
    klass = linker->LookupClass(self, class_name, ComputeModifiedUtf8Hash(class_name), nullptr);
  }
  if (klass == nullptr) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved klass=" << class_name;
  dex_cache->SetResolvedType(type_idx, klass);
  // Skip uninitialized classes because filled static storage entry implies it is initialized.
  if (!klass->IsInitialized()) {
    // LOG(INFO) << "VMRuntime.preloadDexCaches uninitialized klass=" << class_name;
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches static storage klass=" << class_name;
}

// Based on ClassLinker::ResolveField.
static void PreloadDexCachesResolveField(Handle<mirror::DexCache> dex_cache, uint32_t field_idx,
                                         bool is_static)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* field = dex_cache->GetResolvedField(field_idx, sizeof(void*));
  if (field != nullptr) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const DexFile::FieldId& field_id = dex_file->GetFieldId(field_idx);
  Thread* const self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(dex_cache->GetResolvedType(field_id.class_idx_)));
  if (klass.Get() == nullptr) {
    return;
  }
  if (is_static) {
    field = mirror::Class::FindStaticField(self, klass, dex_cache.Get(), field_idx);
  } else {
    field = klass->FindInstanceField(dex_cache.Get(), field_idx);
  }
  if (field == nullptr) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved field " << PrettyField(field);
  dex_cache->SetResolvedField(field_idx, field, sizeof(void*));
}

// Based on ClassLinker::ResolveMethod.
static void PreloadDexCachesResolveMethod(Handle<mirror::DexCache> dex_cache, uint32_t method_idx,
                                          InvokeType invoke_type)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtMethod* method = dex_cache->GetResolvedMethod(method_idx, sizeof(void*));
  if (method != nullptr) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
  mirror::Class* klass = dex_cache->GetResolvedType(method_id.class_idx_);
  if (klass == nullptr) {
    return;
  }
  switch (invoke_type) {
    case kDirect:
    case kStatic:
      method = klass->FindDirectMethod(dex_cache.Get(), method_idx, sizeof(void*));
      break;
    case kInterface:
      method = klass->FindInterfaceMethod(dex_cache.Get(), method_idx, sizeof(void*));
      break;
    case kSuper:
    case kVirtual:
      method = klass->FindVirtualMethod(dex_cache.Get(), method_idx, sizeof(void*));
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << invoke_type;
      UNREACHABLE();
  }
  if (method == nullptr) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved method " << PrettyMethod(method);
  dex_cache->SetResolvedMethod(method_idx, method, sizeof(void*));
}

struct DexCacheStats {
    uint32_t num_strings;
    uint32_t num_types;
    uint32_t num_fields;
    uint32_t num_methods;
    DexCacheStats() : num_strings(0),
                      num_types(0),
                      num_fields(0),
                      num_methods(0) {}
};

static const bool kPreloadDexCachesEnabled = true;

// Disabled because it takes a long time (extra half second) but
// gives almost no benefit in terms of saving private dirty pages.
static const bool kPreloadDexCachesStrings = false;

static const bool kPreloadDexCachesTypes = true;
static const bool kPreloadDexCachesFieldsAndMethods = true;

static const bool kPreloadDexCachesCollectStats = true;

static void PreloadDexCachesStatsTotal(DexCacheStats* total) {
  if (!kPreloadDexCachesCollectStats) {
    return;
  }

  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != nullptr);
    total->num_strings += dex_file->NumStringIds();
    total->num_fields += dex_file->NumFieldIds();
    total->num_methods += dex_file->NumMethodIds();
    total->num_types += dex_file->NumTypeIds();
  }
}

static void PreloadDexCachesStatsFilled(DexCacheStats* filled)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (!kPreloadDexCachesCollectStats) {
    return;
  }
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  Thread* const self = Thread::Current();
  for (const DexFile* dex_file : class_linker->GetBootClassPath()) {
    CHECK(dex_file != nullptr);
    mirror::DexCache* const dex_cache = class_linker->FindDexCache(self, *dex_file, true);
    // If dex cache was deallocated, just continue.
    if (dex_cache == nullptr) {
      continue;
    }
    for (size_t j = 0; j < dex_cache->NumStrings(); j++) {
      mirror::String* string = dex_cache->GetResolvedString(j);
      if (string != nullptr) {
        filled->num_strings++;
      }
    }
    for (size_t j = 0; j < dex_cache->NumResolvedTypes(); j++) {
      mirror::Class* klass = dex_cache->GetResolvedType(j);
      if (klass != nullptr) {
        filled->num_types++;
      }
    }
    for (size_t j = 0; j < dex_cache->NumResolvedFields(); j++) {
      ArtField* field = class_linker->GetResolvedField(j, dex_cache);
      if (field != nullptr) {
        filled->num_fields++;
      }
    }
    for (size_t j = 0; j < dex_cache->NumResolvedMethods(); j++) {
      ArtMethod* method = dex_cache->GetResolvedMethod(j, sizeof(void*));
      if (method != nullptr) {
        filled->num_methods++;
      }
    }
  }
}

// TODO: http://b/11309598 This code was ported over based on the
// Dalvik version. However, ART has similar code in other places such
// as the CompilerDriver. This code could probably be refactored to
// serve both uses.
static void VMRuntime_preloadDexCaches(JNIEnv* env, jobject) {
  if (!kPreloadDexCachesEnabled) {
    return;
  }

  ScopedObjectAccess soa(env);

  DexCacheStats total;
  DexCacheStats before;
  if (kPreloadDexCachesCollectStats) {
    LOG(INFO) << "VMRuntime.preloadDexCaches starting";
    PreloadDexCachesStatsTotal(&total);
    PreloadDexCachesStatsFilled(&before);
  }

  Runtime* runtime = Runtime::Current();
  ClassLinker* linker = runtime->GetClassLinker();

  // We use a std::map to avoid heap allocating StringObjects to lookup in gDvm.literalStrings
  StringTable strings;
  if (kPreloadDexCachesStrings) {
    PreloadDexCachesStringsVisitor visitor(&strings);
    runtime->GetInternTable()->VisitRoots(&visitor, kVisitRootFlagAllRoots);
  }

  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i < boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != nullptr);
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(linker->RegisterDexFile(*dex_file, nullptr)));

    if (kPreloadDexCachesStrings) {
      for (size_t j = 0; j < dex_cache->NumStrings(); j++) {
        PreloadDexCachesResolveString(dex_cache, j, strings);
      }
    }

    if (kPreloadDexCachesTypes) {
      for (size_t j = 0; j < dex_cache->NumResolvedTypes(); j++) {
        PreloadDexCachesResolveType(soa.Self(), dex_cache.Get(), j);
      }
    }

    if (kPreloadDexCachesFieldsAndMethods) {
      for (size_t class_def_index = 0;
           class_def_index < dex_file->NumClassDefs();
           class_def_index++) {
        const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        const uint8_t* class_data = dex_file->GetClassData(class_def);
        if (class_data == nullptr) {
          continue;
        }
        ClassDataItemIterator it(*dex_file, class_data);
        for (; it.HasNextStaticField(); it.Next()) {
          uint32_t field_idx = it.GetMemberIndex();
          PreloadDexCachesResolveField(dex_cache, field_idx, true);
        }
        for (; it.HasNextInstanceField(); it.Next()) {
          uint32_t field_idx = it.GetMemberIndex();
          PreloadDexCachesResolveField(dex_cache, field_idx, false);
        }
        for (; it.HasNextDirectMethod(); it.Next()) {
          uint32_t method_idx = it.GetMemberIndex();
          InvokeType invoke_type = it.GetMethodInvokeType(class_def);
          PreloadDexCachesResolveMethod(dex_cache, method_idx, invoke_type);
        }
        for (; it.HasNextVirtualMethod(); it.Next()) {
          uint32_t method_idx = it.GetMemberIndex();
          InvokeType invoke_type = it.GetMethodInvokeType(class_def);
          PreloadDexCachesResolveMethod(dex_cache, method_idx, invoke_type);
        }
      }
    }
  }

  if (kPreloadDexCachesCollectStats) {
    DexCacheStats after;
    PreloadDexCachesStatsFilled(&after);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches strings total=%d before=%d after=%d",
                              total.num_strings, before.num_strings, after.num_strings);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches types total=%d before=%d after=%d",
                              total.num_types, before.num_types, after.num_types);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches fields total=%d before=%d after=%d",
                              total.num_fields, before.num_fields, after.num_fields);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches methods total=%d before=%d after=%d",
                              total.num_methods, before.num_methods, after.num_methods);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches finished");
  }
}


/*
 * This is called by the framework when it knows the application directory and
 * process name.
 */
static void VMRuntime_registerAppInfo(JNIEnv* env,
                                      jclass clazz ATTRIBUTE_UNUSED,
                                      jstring profile_file,
                                      jstring app_dir,
                                      jobjectArray code_paths,
                                      jstring foreign_dex_profile_path) {
  std::vector<std::string> code_paths_vec;
  int code_paths_length = env->GetArrayLength(code_paths);
  for (int i = 0; i < code_paths_length; i++) {
    jstring code_path = reinterpret_cast<jstring>(env->GetObjectArrayElement(code_paths, i));
    const char* raw_code_path = env->GetStringUTFChars(code_path, nullptr);
    code_paths_vec.push_back(raw_code_path);
    env->ReleaseStringUTFChars(code_path, raw_code_path);
  }

  const char* raw_profile_file = env->GetStringUTFChars(profile_file, nullptr);
  std::string profile_file_str(raw_profile_file);
  env->ReleaseStringUTFChars(profile_file, raw_profile_file);

  std::string foreign_dex_profile_path_str = "";
  if (foreign_dex_profile_path != nullptr) {
    const char* raw_foreign_dex_profile_path =
        env->GetStringUTFChars(foreign_dex_profile_path, nullptr);
    foreign_dex_profile_path_str.assign(raw_foreign_dex_profile_path);
    env->ReleaseStringUTFChars(foreign_dex_profile_path, raw_foreign_dex_profile_path);
  }

  const char* raw_app_dir = env->GetStringUTFChars(app_dir, nullptr);
  std::string app_dir_str(raw_app_dir);
  env->ReleaseStringUTFChars(app_dir, raw_app_dir);

  Runtime::Current()->RegisterAppInfo(code_paths_vec,
                                      profile_file_str,
                                      foreign_dex_profile_path_str,
                                      app_dir_str);
}

static jboolean VMRuntime_isBootClassPathOnDisk(JNIEnv* env, jclass, jstring java_instruction_set) {
  ScopedUtfChars instruction_set(env, java_instruction_set);
  if (instruction_set.c_str() == nullptr) {
    return JNI_FALSE;
  }
  InstructionSet isa = GetInstructionSetFromString(instruction_set.c_str());
  if (isa == kNone) {
    ScopedLocalRef<jclass> iae(env, env->FindClass("java/lang/IllegalArgumentException"));
    std::string message(StringPrintf("Instruction set %s is invalid.", instruction_set.c_str()));
    env->ThrowNew(iae.get(), message.c_str());
    return JNI_FALSE;
  }
  std::string error_msg;
  std::unique_ptr<ImageHeader> image_header(gc::space::ImageSpace::ReadImageHeader(
      Runtime::Current()->GetImageLocation().c_str(), isa, &error_msg));
  return image_header.get() != nullptr;
}

static jstring VMRuntime_getCurrentInstructionSet(JNIEnv* env, jclass) {
  return env->NewStringUTF(GetInstructionSetString(kRuntimeISA));
}

static jboolean VMRuntime_didPruneDalvikCache(JNIEnv* env ATTRIBUTE_UNUSED,
                                              jclass klass ATTRIBUTE_UNUSED) {
  return Runtime::Current()->GetPrunedDalvikCache() ? JNI_TRUE : JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMRuntime, addressOf, "!(Ljava/lang/Object;)J"),
  NATIVE_METHOD(VMRuntime, bootClassPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, clampGrowthLimit, "()V"),
  NATIVE_METHOD(VMRuntime, classPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, clearGrowthLimit, "()V"),
  NATIVE_METHOD(VMRuntime, concurrentGC, "()V"),
  NATIVE_METHOD(VMRuntime, disableJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, getTargetHeapUtilization, "()F"),
  NATIVE_METHOD(VMRuntime, isDebuggerActive, "!()Z"),
  NATIVE_METHOD(VMRuntime, isNativeDebuggable, "!()Z"),
  NATIVE_METHOD(VMRuntime, nativeSetTargetHeapUtilization, "(F)V"),
  NATIVE_METHOD(VMRuntime, newNonMovableArray, "!(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(VMRuntime, newUnpaddedArray, "!(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(VMRuntime, properties, "()[Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, setTargetSdkVersionNative, "(I)V"),
  NATIVE_METHOD(VMRuntime, registerNativeAllocation, "(I)V"),
  NATIVE_METHOD(VMRuntime, registerSensitiveThread, "()V"),
  NATIVE_METHOD(VMRuntime, registerNativeFree, "(I)V"),
  NATIVE_METHOD(VMRuntime, requestConcurrentGC, "()V"),
  NATIVE_METHOD(VMRuntime, requestHeapTrim, "()V"),
  NATIVE_METHOD(VMRuntime, runHeapTasks, "()V"),
  NATIVE_METHOD(VMRuntime, updateProcessState, "(I)V"),
  NATIVE_METHOD(VMRuntime, startHeapTaskProcessor, "()V"),
  NATIVE_METHOD(VMRuntime, startJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, stopHeapTaskProcessor, "()V"),
  NATIVE_METHOD(VMRuntime, trimHeap, "()V"),
  NATIVE_METHOD(VMRuntime, vmVersion, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, vmLibrary, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, vmInstructionSet, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, is64Bit, "!()Z"),
  NATIVE_METHOD(VMRuntime, isCheckJniEnabled, "!()Z"),
  NATIVE_METHOD(VMRuntime, preloadDexCaches, "()V"),
  NATIVE_METHOD(VMRuntime, registerAppInfo,
                "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)V"),
  NATIVE_METHOD(VMRuntime, isBootClassPathOnDisk, "(Ljava/lang/String;)Z"),
  NATIVE_METHOD(VMRuntime, getCurrentInstructionSet, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, didPruneDalvikCache, "()Z"),
};

void register_dalvik_system_VMRuntime(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/VMRuntime");
}

}  // namespace art
