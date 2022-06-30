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

#include "runtime.h"

// sys/mount.h has to come before linux/fs.h due to redefinition of MS_RDONLY, MS_BIND, etc
#include <sys/mount.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/prctl.h>
#endif

#include <signal.h>
#include <sys/syscall.h>
#include "base/memory_tool.h"
#if defined(__APPLE__)
#include <crt_externs.h>  // for _NSGetEnviron
#endif

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory_representation.h>
#include <vector>
#include <fcntl.h>

#include "JniConstants.h"
#include "ScopedLocalRef.h"
#include "arch/arm/quick_method_frame_info_arm.h"
#include "arch/arm/registers_arm.h"
#include "arch/arm64/quick_method_frame_info_arm64.h"
#include "arch/arm64/registers_arm64.h"
#include "arch/instruction_set_features.h"
#include "arch/mips/quick_method_frame_info_mips.h"
#include "arch/mips/registers_mips.h"
#include "arch/mips64/quick_method_frame_info_mips64.h"
#include "arch/mips64/registers_mips64.h"
#include "arch/x86/quick_method_frame_info_x86.h"
#include "arch/x86/registers_x86.h"
#include "arch/x86_64/quick_method_frame_info_x86_64.h"
#include "arch/x86_64/registers_x86_64.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "asm_support.h"
#include "atomic.h"
#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/unix_file/fd_file.h"
#include "class_linker-inl.h"
#include "compiler_callbacks.h"
#include "compiler_filter.h"
#include "debugger.h"
#include "elf_file.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "experimental_flags.h"
#include "fault_handler.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "handle_scope-inl.h"
#include "image-inl.h"
#include "instrumentation.h"
#include "intern_table.h"
#include "interpreter/interpreter.h"
#include "jit/jit.h"
#include "jni_internal.h"
#include "linear_alloc.h"
#include "lambda/box_table.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/field.h"
#include "mirror/method.h"
#include "mirror/stack_trace_element.h"
#include "mirror/throwable.h"
#include "monitor.h"
#include "native/dalvik_system_DexFile.h"
#include "native/dalvik_system_VMDebug.h"
#include "native/dalvik_system_VMRuntime.h"
#include "native/dalvik_system_VMStack.h"
#include "native/dalvik_system_ZygoteHooks.h"
#include "native/java_lang_Class.h"
#include "native/java_lang_DexCache.h"
#include "native/java_lang_Object.h"
#include "native/java_lang_String.h"
#include "native/java_lang_StringFactory.h"
#include "native/java_lang_System.h"
#include "native/java_lang_Thread.h"
#include "native/java_lang_Throwable.h"
#include "native/java_lang_VMClassLoader.h"
#include "native/java_lang_ref_FinalizerReference.h"
#include "native/java_lang_ref_Reference.h"
#include "native/java_lang_reflect_AbstractMethod.h"
#include "native/java_lang_reflect_Array.h"
#include "native/java_lang_reflect_Constructor.h"
#include "native/java_lang_reflect_Field.h"
#include "native/java_lang_reflect_Method.h"
#include "native/java_lang_reflect_Proxy.h"
#include "native/java_util_concurrent_atomic_AtomicLong.h"
#include "native/libcore_util_CharsetUtils.h"
#include "native/org_apache_harmony_dalvik_ddmc_DdmServer.h"
#include "native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.h"
#include "native/sun_misc_Unsafe.h"
#include "native_bridge_art_interface.h"
#include "oat_file.h"
#include "oat_file_manager.h"
#include "os.h"
#include "parsed_options.h"
#include "profiler.h"
#include "jit/profile_saver.h"
#include "quick/quick_method_frame_info.h"
#include "reflection.h"
#include "runtime_options.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sigchain.h"
#include "signal_catcher.h"
#include "signal_set.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"
#include "transaction.h"
#include "utils.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

//patch by Youlor
//++++++++++++++++++++++++++++
#include "unpacker/unpacker.h"
//++++++++++++++++++++++++++++

namespace art {

// If a signal isn't handled properly, enable a handler that attempts to dump the Java stack.
static constexpr bool kEnableJavaStackTraceHandler = false;
// Tuned by compiling GmsCore under perf and measuring time spent in DescriptorEquals for class
// linking.
static constexpr double kLowMemoryMinLoadFactor = 0.5;
static constexpr double kLowMemoryMaxLoadFactor = 0.8;
static constexpr double kNormalMinLoadFactor = 0.4;
static constexpr double kNormalMaxLoadFactor = 0.7;
Runtime* Runtime::instance_ = nullptr;

struct TraceConfig {
  Trace::TraceMode trace_mode;
  Trace::TraceOutputMode trace_output_mode;
  std::string trace_file;
  size_t trace_file_size;
};

namespace {
#ifdef __APPLE__
inline char** GetEnviron() {
  // When Google Test is built as a framework on MacOS X, the environ variable
  // is unavailable. Apple's documentation (man environ) recommends using
  // _NSGetEnviron() instead.
  return *_NSGetEnviron();
}
#else
// Some POSIX platforms expect you to declare environ. extern "C" makes
// it reside in the global namespace.
extern "C" char** environ;
inline char** GetEnviron() { return environ; }
#endif
}  // namespace

Runtime::Runtime()
    : resolution_method_(nullptr),
      imt_conflict_method_(nullptr),
      imt_unimplemented_method_(nullptr),
      instruction_set_(kNone),
      compiler_callbacks_(nullptr),
      is_zygote_(false),
      must_relocate_(false),
      is_concurrent_gc_enabled_(true),
      is_explicit_gc_disabled_(false),
      dex2oat_enabled_(true),
      image_dex2oat_enabled_(true),
      default_stack_size_(0),
      heap_(nullptr),
      max_spins_before_thin_lock_inflation_(Monitor::kDefaultMaxSpinsBeforeThinLockInflation),
      monitor_list_(nullptr),
      monitor_pool_(nullptr),
      thread_list_(nullptr),
      intern_table_(nullptr),
      class_linker_(nullptr),
      signal_catcher_(nullptr),
      java_vm_(nullptr),
      fault_message_lock_("Fault message lock"),
      fault_message_(""),
      threads_being_born_(0),
      shutdown_cond_(new ConditionVariable("Runtime shutdown", *Locks::runtime_shutdown_lock_)),
      shutting_down_(false),
      shutting_down_started_(false),
      started_(false),
      finished_starting_(false),
      vfprintf_(nullptr),
      exit_(nullptr),
      abort_(nullptr),
      stats_enabled_(false),
      is_running_on_memory_tool_(RUNNING_ON_MEMORY_TOOL),
      instrumentation_(),
      main_thread_group_(nullptr),
      system_thread_group_(nullptr),
      system_class_loader_(nullptr),
      dump_gc_performance_on_shutdown_(false),
      preinitialization_transaction_(nullptr),
      verify_(verifier::VerifyMode::kNone),
      allow_dex_file_fallback_(true),
      target_sdk_version_(0),
      implicit_null_checks_(false),
      implicit_so_checks_(false),
      implicit_suspend_checks_(false),
      no_sig_chain_(false),
      force_native_bridge_(false),
      is_native_bridge_loaded_(false),
      is_native_debuggable_(false),
      zygote_max_failed_boots_(0),
      experimental_flags_(ExperimentalFlags::kNone),
      oat_file_manager_(nullptr),
      is_low_memory_mode_(false),
      safe_mode_(false),
      dump_native_stack_on_sig_quit_(true),
      pruned_dalvik_cache_(false),
      // Initially assume we perceive jank in case the process state is never updated.
      process_state_(kProcessStateJankPerceptible),
      zygote_no_threads_(false) {
  CheckAsmSupportOffsetsAndSizes();
  std::fill(callee_save_methods_, callee_save_methods_ + arraysize(callee_save_methods_), 0u);
  interpreter::CheckInterpreterAsmConstants();
}

Runtime::~Runtime() {
  ScopedTrace trace("Runtime shutdown");
  if (is_native_bridge_loaded_) {
    UnloadNativeBridge();
  }

  if (dump_gc_performance_on_shutdown_) {
    // This can't be called from the Heap destructor below because it
    // could call RosAlloc::InspectAll() which needs the thread_list
    // to be still alive.
    heap_->DumpGcPerformanceInfo(LOG(INFO));
  }

  Thread* self = Thread::Current();
  const bool attach_shutdown_thread = self == nullptr;
  if (attach_shutdown_thread) {
    CHECK(AttachCurrentThread("Shutdown thread", false, nullptr, false));
    self = Thread::Current();
  } else {
    LOG(WARNING) << "Current thread not detached in Runtime shutdown";
  }

  {
    ScopedTrace trace2("Wait for shutdown cond");
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    shutting_down_started_ = true;
    while (threads_being_born_ > 0) {
      shutdown_cond_->Wait(self);
    }
    shutting_down_ = true;
  }
  // Shutdown and wait for the daemons.
  CHECK(self != nullptr);
  if (IsFinishedStarting()) {
    ScopedTrace trace2("Waiting for Daemons");
    self->ClearException();
    self->GetJniEnv()->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                                            WellKnownClasses::java_lang_Daemons_stop);
  }

  Trace::Shutdown();

  if (attach_shutdown_thread) {
    DetachCurrentThread();
    self = nullptr;
  }

  // Make sure to let the GC complete if it is running.
  heap_->WaitForGcToComplete(gc::kGcCauseBackground, self);
  heap_->DeleteThreadPool();
  if (jit_ != nullptr) {
    ScopedTrace trace2("Delete jit");
    VLOG(jit) << "Deleting jit thread pool";
    // Delete thread pool before the thread list since we don't want to wait forever on the
    // JIT compiler threads.
    jit_->DeleteThreadPool();
    // Similarly, stop the profile saver thread before deleting the thread list.
    jit_->StopProfileSaver();
  }

  // Make sure our internal threads are dead before we start tearing down things they're using.
  Dbg::StopJdwp();
  delete signal_catcher_;

  // Make sure all other non-daemon threads have terminated, and all daemon threads are suspended.
  {
    ScopedTrace trace2("Delete thread list");
    delete thread_list_;
  }
  // Delete the JIT after thread list to ensure that there is no remaining threads which could be
  // accessing the instrumentation when we delete it.
  if (jit_ != nullptr) {
    VLOG(jit) << "Deleting jit";
    jit_.reset(nullptr);
  }

  // Shutdown the fault manager if it was initialized.
  fault_manager.Shutdown();

  ScopedTrace trace2("Delete state");
  delete monitor_list_;
  delete monitor_pool_;
  delete class_linker_;
  delete heap_;
  delete intern_table_;
  delete java_vm_;
  delete oat_file_manager_;
  Thread::Shutdown();
  QuasiAtomic::Shutdown();
  verifier::MethodVerifier::Shutdown();

  // Destroy allocators before shutting down the MemMap because they may use it.
  linear_alloc_.reset();
  low_4gb_arena_pool_.reset();
  arena_pool_.reset();
  jit_arena_pool_.reset();
  MemMap::Shutdown();

  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == nullptr || instance_ == this);
  instance_ = nullptr;
}

struct AbortState {
  void Dump(std::ostream& os) const {
    if (gAborting > 1) {
      os << "Runtime aborting --- recursively, so no thread-specific detail!\n";
      return;
    }
    gAborting++;
    os << "Runtime aborting...\n";
    if (Runtime::Current() == nullptr) {
      os << "(Runtime does not yet exist!)\n";
      DumpNativeStack(os, GetTid(), nullptr, "  native: ", nullptr);
      return;
    }
    Thread* self = Thread::Current();
    if (self == nullptr) {
      os << "(Aborting thread was not attached to runtime!)\n";
      DumpKernelStack(os, GetTid(), "  kernel: ", false);
      DumpNativeStack(os, GetTid(), nullptr, "  native: ", nullptr);
    } else {
      os << "Aborting thread:\n";
      if (Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self)) {
        DumpThread(os, self);
      } else {
        if (Locks::mutator_lock_->SharedTryLock(self)) {
          DumpThread(os, self);
          Locks::mutator_lock_->SharedUnlock(self);
        }
      }
    }
    DumpAllThreads(os, self);
  }

  // No thread-safety analysis as we do explicitly test for holding the mutator lock.
  void DumpThread(std::ostream& os, Thread* self) const NO_THREAD_SAFETY_ANALYSIS {
    DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self));
    self->Dump(os);
    if (self->IsExceptionPending()) {
      mirror::Throwable* exception = self->GetException();
      os << "Pending exception " << exception->Dump();
    }
  }

  void DumpAllThreads(std::ostream& os, Thread* self) const {
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      ThreadList* thread_list = runtime->GetThreadList();
      if (thread_list != nullptr) {
        bool tll_already_held = Locks::thread_list_lock_->IsExclusiveHeld(self);
        bool ml_already_held = Locks::mutator_lock_->IsSharedHeld(self);
        if (!tll_already_held || !ml_already_held) {
          os << "Dumping all threads without appropriate locks held:"
              << (!tll_already_held ? " thread list lock" : "")
              << (!ml_already_held ? " mutator lock" : "")
              << "\n";
        }
        os << "All threads:\n";
        thread_list->Dump(os);
      }
    }
  }
};

void Runtime::Abort(const char* msg) {
  gAborting++;  // set before taking any locks

  // Ensure that we don't have multiple threads trying to abort at once,
  // which would result in significantly worse diagnostics.
  MutexLock mu(Thread::Current(), *Locks::abort_lock_);

  // Get any pending output out of the way.
  fflush(nullptr);

  // Many people have difficulty distinguish aborts from crashes,
  // so be explicit.
  AbortState state;
  LOG(INTERNAL_FATAL) << Dumpable<AbortState>(state);

  // Sometimes we dump long messages, and the Android abort message only retains the first line.
  // In those cases, just log the message again, to avoid logcat limits.
  if (msg != nullptr && strchr(msg, '\n') != nullptr) {
    LOG(INTERNAL_FATAL) << msg;
  }

  // Call the abort hook if we have one.
  if (Runtime::Current() != nullptr && Runtime::Current()->abort_ != nullptr) {
    LOG(INTERNAL_FATAL) << "Calling abort hook...";
    Runtime::Current()->abort_();
    // notreached
    LOG(INTERNAL_FATAL) << "Unexpectedly returned from abort hook!";
  }

#if defined(__GLIBC__)
  // TODO: we ought to be able to use pthread_kill(3) here (or abort(3),
  // which POSIX defines in terms of raise(3), which POSIX defines in terms
  // of pthread_kill(3)). On Linux, though, libcorkscrew can't unwind through
  // libpthread, which means the stacks we dump would be useless. Calling
  // tgkill(2) directly avoids that.
  syscall(__NR_tgkill, getpid(), GetTid(), SIGABRT);
  // TODO: LLVM installs it's own SIGABRT handler so exit to be safe... Can we disable that in LLVM?
  // If not, we could use sigaction(3) before calling tgkill(2) and lose this call to exit(3).
  exit(1);
#else
  abort();
#endif
  // notreached
}

void Runtime::PreZygoteFork() {
  heap_->PreZygoteFork();
}

void Runtime::CallExitHook(jint status) {
  if (exit_ != nullptr) {
    ScopedThreadStateChange tsc(Thread::Current(), kNative);
    exit_(status);
    LOG(WARNING) << "Exit hook returned instead of exiting!";
  }
}

void Runtime::SweepSystemWeaks(IsMarkedVisitor* visitor) {
  GetInternTable()->SweepInternTableWeaks(visitor);
  GetMonitorList()->SweepMonitorList(visitor);
  GetJavaVM()->SweepJniWeakGlobals(visitor);
  GetHeap()->SweepAllocationRecords(visitor);
  GetLambdaBoxTable()->SweepWeakBoxedLambdas(visitor);
}

bool Runtime::ParseOptions(const RuntimeOptions& raw_options,
                           bool ignore_unrecognized,
                           RuntimeArgumentMap* runtime_options) {
  InitLogging(/* argv */ nullptr);  // Calls Locks::Init() as a side effect.
  bool parsed = ParsedOptions::Parse(raw_options, ignore_unrecognized, runtime_options);
  if (!parsed) {
    LOG(ERROR) << "Failed to parse options";
    return false;
  }
  return true;
}

bool Runtime::Create(RuntimeArgumentMap&& runtime_options) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != nullptr) {
    return false;
  }
  instance_ = new Runtime;
  if (!instance_->Init(std::move(runtime_options))) {
    // TODO: Currently deleting the instance will abort the runtime on destruction. Now This will
    // leak memory, instead. Fix the destructor. b/19100793.
    // delete instance_;
    instance_ = nullptr;
    return false;
  }
  return true;
}

bool Runtime::Create(const RuntimeOptions& raw_options, bool ignore_unrecognized) {
  RuntimeArgumentMap runtime_options;
  return ParseOptions(raw_options, ignore_unrecognized, &runtime_options) &&
      Create(std::move(runtime_options));
}

static jobject CreateSystemClassLoader(Runtime* runtime) {
  if (runtime->IsAotCompiler() && !runtime->GetCompilerCallbacks()->IsBootImage()) {
    return nullptr;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  auto pointer_size = cl->GetImagePointerSize();

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> class_loader_class(
      hs.NewHandle(soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ClassLoader)));
  CHECK(cl->EnsureInitialized(soa.Self(), class_loader_class, true, true));

  ArtMethod* getSystemClassLoader = class_loader_class->FindDirectMethod(
      "getSystemClassLoader", "()Ljava/lang/ClassLoader;", pointer_size);
  CHECK(getSystemClassLoader != nullptr);

  JValue result = InvokeWithJValues(soa, nullptr, soa.EncodeMethod(getSystemClassLoader), nullptr);
  JNIEnv* env = soa.Self()->GetJniEnv();
  ScopedLocalRef<jobject> system_class_loader(env, soa.AddLocalReference<jobject>(result.GetL()));
  CHECK(system_class_loader.get() != nullptr);

  soa.Self()->SetClassLoaderOverride(system_class_loader.get());

  Handle<mirror::Class> thread_class(
      hs.NewHandle(soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread)));
  CHECK(cl->EnsureInitialized(soa.Self(), thread_class, true, true));

  ArtField* contextClassLoader =
      thread_class->FindDeclaredInstanceField("contextClassLoader", "Ljava/lang/ClassLoader;");
  CHECK(contextClassLoader != nullptr);

  // We can't run in a transaction yet.
  contextClassLoader->SetObject<false>(soa.Self()->GetPeer(),
                                       soa.Decode<mirror::ClassLoader*>(system_class_loader.get()));

  return env->NewGlobalRef(system_class_loader.get());
}

std::string Runtime::GetPatchoatExecutable() const {
  if (!patchoat_executable_.empty()) {
    return patchoat_executable_;
  }
  std::string patchoat_executable(GetAndroidRoot());
  patchoat_executable += (kIsDebugBuild ? "/bin/patchoatd" : "/bin/patchoat");
  return patchoat_executable;
}

std::string Runtime::GetCompilerExecutable() const {
  if (!compiler_executable_.empty()) {
    return compiler_executable_;
  }
  std::string compiler_executable(GetAndroidRoot());
  compiler_executable += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  return compiler_executable;
}

bool Runtime::Start() {
  VLOG(startup) << "Runtime::Start entering";

  CHECK(!no_sig_chain_) << "A started runtime should have sig chain enabled";

  // If a debug host build, disable ptrace restriction for debugging and test timeout thread dump.
  // Only 64-bit as prctl() may fail in 32 bit userspace on a 64-bit kernel.
#if defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)
  if (kIsDebugBuild) {
    CHECK_EQ(prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY), 0);
  }
#endif

  // Restore main thread state to kNative as expected by native code.
  Thread* self = Thread::Current();

  self->TransitionFromRunnableToSuspended(kNative);

  started_ = true;

  if (!IsImageDex2OatEnabled() || !GetHeap()->HasBootImageSpace()) {
    ScopedObjectAccess soa(self);
    StackHandleScope<2> hs(soa.Self());

    auto class_class(hs.NewHandle<mirror::Class>(mirror::Class::GetJavaLangClass()));
    auto field_class(hs.NewHandle<mirror::Class>(mirror::Field::StaticClass()));

    class_linker_->EnsureInitialized(soa.Self(), class_class, true, true);
    // Field class is needed for register_java_net_InetAddress in libcore, b/28153851.
    class_linker_->EnsureInitialized(soa.Self(), field_class, true, true);
  }

  // InitNativeMethods needs to be after started_ so that the classes
  // it touches will have methods linked to the oat file if necessary.
  {
    ScopedTrace trace2("InitNativeMethods");
    InitNativeMethods();
  }

  // Initialize well known thread group values that may be accessed threads while attaching.
  InitThreadGroups(self);

  Thread::FinishStartup();

  // Create the JIT either if we have to use JIT compilation or save profiling info. This is
  // done after FinishStartup as the JIT pool needs Java thread peers, which require the main
  // ThreadGroup to exist.
  //
  // TODO(calin): We use the JIT class as a proxy for JIT compilation and for
  // recoding profiles. Maybe we should consider changing the name to be more clear it's
  // not only about compiling. b/28295073.
  if (jit_options_->UseJitCompilation() || jit_options_->GetSaveProfilingInfo()) {
    std::string error_msg;
    if (!IsZygote()) {
    // If we are the zygote then we need to wait until after forking to create the code cache
    // due to SELinux restrictions on r/w/x memory regions.
      CreateJit();
    } else if (jit_options_->UseJitCompilation()) {
      if (!jit::Jit::LoadCompilerLibrary(&error_msg)) {
        // Try to load compiler pre zygote to reduce PSS. b/27744947
        LOG(WARNING) << "Failed to load JIT compiler with error " << error_msg;
      }
    }
  }

  system_class_loader_ = CreateSystemClassLoader(this);

  if (is_zygote_) {
    if (!InitZygote()) {
      return false;
    }
  } else {
    if (is_native_bridge_loaded_) {
      PreInitializeNativeBridge(".");
    }
    NativeBridgeAction action = force_native_bridge_
        ? NativeBridgeAction::kInitialize
        : NativeBridgeAction::kUnload;
    InitNonZygoteOrPostFork(self->GetJniEnv(),
                            /* is_system_server */ false,
                            action,
                            GetInstructionSetString(kRuntimeISA));
  }

  StartDaemonThreads();

  {
    ScopedObjectAccess soa(self);
    self->GetJniEnv()->locals.AssertEmpty();
  }

  VLOG(startup) << "Runtime::Start exiting";
  finished_starting_ = true;

  if (profiler_options_.IsEnabled() && !profile_output_filename_.empty()) {
    // User has asked for a profile using -Xenable-profiler.
    // Create the profile file if it doesn't exist.
    int fd = open(profile_output_filename_.c_str(), O_RDWR|O_CREAT|O_EXCL, 0660);
    if (fd >= 0) {
      close(fd);
    } else if (errno != EEXIST) {
      LOG(WARNING) << "Failed to access the profile file. Profiler disabled.";
    }
  }

  if (trace_config_.get() != nullptr && trace_config_->trace_file != "") {
    ScopedThreadStateChange tsc(self, kWaitingForMethodTracingStart);
    Trace::Start(trace_config_->trace_file.c_str(),
                 -1,
                 static_cast<int>(trace_config_->trace_file_size),
                 0,
                 trace_config_->trace_output_mode,
                 trace_config_->trace_mode,
                 0);
  }

  return true;
}

void Runtime::EndThreadBirth() REQUIRES(Locks::runtime_shutdown_lock_) {
  DCHECK_GT(threads_being_born_, 0U);
  threads_being_born_--;
  if (shutting_down_started_ && threads_being_born_ == 0) {
    shutdown_cond_->Broadcast(Thread::Current());
  }
}

// Do zygote-mode-only initialization.
bool Runtime::InitZygote() {
#ifdef __linux__
  // zygote goes into its own process group
  setpgid(0, 0);

  // See storage config details at http://source.android.com/tech/storage/
  // Create private mount namespace shared by all children
  if (unshare(CLONE_NEWNS) == -1) {
    PLOG(ERROR) << "Failed to unshare()";
    return false;
  }

  // Mark rootfs as being a slave so that changes from default
  // namespace only flow into our children.
  if (mount("rootfs", "/", nullptr, (MS_SLAVE | MS_REC), nullptr) == -1) {
    PLOG(ERROR) << "Failed to mount() rootfs as MS_SLAVE";
    return false;
  }

  // Create a staging tmpfs that is shared by our children; they will
  // bind mount storage into their respective private namespaces, which
  // are isolated from each other.
  const char* target_base = getenv("EMULATED_STORAGE_TARGET");
  if (target_base != nullptr) {
    if (mount("tmpfs", target_base, "tmpfs", MS_NOSUID | MS_NODEV,
              "uid=0,gid=1028,mode=0751") == -1) {
      PLOG(ERROR) << "Failed to mount tmpfs to " << target_base;
      return false;
    }
  }

  return true;
#else
  UNIMPLEMENTED(FATAL);
  return false;
#endif
}

void Runtime::InitNonZygoteOrPostFork(
    JNIEnv* env, bool is_system_server, NativeBridgeAction action, const char* isa) {
  is_zygote_ = false;

  if (is_native_bridge_loaded_) {
    switch (action) {
      case NativeBridgeAction::kUnload:
        UnloadNativeBridge();
        is_native_bridge_loaded_ = false;
        break;

      case NativeBridgeAction::kInitialize:
        InitializeNativeBridge(env, isa);
        break;
    }
  }

  // Create the thread pools.
  heap_->CreateThreadPool();
  // Reset the gc performance data at zygote fork so that the GCs
  // before fork aren't attributed to an app.
  heap_->ResetGcPerformanceInfo();


  if (!is_system_server &&
      !safe_mode_ &&
      (jit_options_->UseJitCompilation() || jit_options_->GetSaveProfilingInfo()) &&
      jit_.get() == nullptr) {
    // Note that when running ART standalone (not zygote, nor zygote fork),
    // the jit may have already been created.
    CreateJit();
  }

  StartSignalCatcher();

  // Start the JDWP thread. If the command-line debugger flags specified "suspend=y",
  // this will pause the runtime, so we probably want this to come last.
  Dbg::StartJdwp();
}

void Runtime::StartSignalCatcher() {
  if (!is_zygote_) {
    signal_catcher_ = new SignalCatcher(stack_trace_file_);
  }
}

bool Runtime::IsShuttingDown(Thread* self) {
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  return IsShuttingDownLocked();
}

bool Runtime::IsDebuggable() const {
  const OatFile* oat_file = GetOatFileManager().GetPrimaryOatFile();
  return oat_file != nullptr && oat_file->IsDebuggable();
}

void Runtime::StartDaemonThreads() {
  ScopedTrace trace(__FUNCTION__);
  VLOG(startup) << "Runtime::StartDaemonThreads entering";

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), kNative);

  JNIEnv* env = self->GetJniEnv();
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    LOG(FATAL) << "Error starting java.lang.Daemons";
  }

  VLOG(startup) << "Runtime::StartDaemonThreads exiting";
}

// Attempts to open dex files from image(s). Given the image location, try to find the oat file
// and open it to get the stored dex file. If the image is the first for a multi-image boot
// classpath, go on and also open the other images.
static bool OpenDexFilesFromImage(const std::string& image_location,
                                  std::vector<std::unique_ptr<const DexFile>>* dex_files,
                                  size_t* failures) {
  DCHECK(dex_files != nullptr) << "OpenDexFilesFromImage: out-param is nullptr";

  // Use a work-list approach, so that we can easily reuse the opening code.
  std::vector<std::string> image_locations;
  image_locations.push_back(image_location);

  for (size_t index = 0; index < image_locations.size(); ++index) {
    std::string system_filename;
    bool has_system = false;
    std::string cache_filename_unused;
    bool dalvik_cache_exists_unused;
    bool has_cache_unused;
    bool is_global_cache_unused;
    bool found_image = gc::space::ImageSpace::FindImageFilename(image_locations[index].c_str(),
                                                                kRuntimeISA,
                                                                &system_filename,
                                                                &has_system,
                                                                &cache_filename_unused,
                                                                &dalvik_cache_exists_unused,
                                                                &has_cache_unused,
                                                                &is_global_cache_unused);

    if (!found_image || !has_system) {
      return false;
    }

    // We are falling back to non-executable use of the oat file because patching failed, presumably
    // due to lack of space.
    std::string oat_filename =
        ImageHeader::GetOatLocationFromImageLocation(system_filename.c_str());
    std::string oat_location =
        ImageHeader::GetOatLocationFromImageLocation(image_locations[index].c_str());
    // Note: in the multi-image case, the image location may end in ".jar," and not ".art." Handle
    //       that here.
    if (EndsWith(oat_location, ".jar")) {
      oat_location.replace(oat_location.length() - 3, 3, "oat");
    }

    std::unique_ptr<File> file(OS::OpenFileForReading(oat_filename.c_str()));
    if (file.get() == nullptr) {
      return false;
    }
    std::string error_msg;
    std::unique_ptr<ElfFile> elf_file(ElfFile::Open(file.get(),
                                                    false,
                                                    false,
                                                    /*low_4gb*/false,
                                                    &error_msg));
    if (elf_file.get() == nullptr) {
      return false;
    }
    std::unique_ptr<const OatFile> oat_file(
        OatFile::OpenWithElfFile(elf_file.release(), oat_location, nullptr, &error_msg));
    if (oat_file == nullptr) {
      LOG(WARNING) << "Unable to use '" << oat_filename << "' because " << error_msg;
      return false;
    }

    for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
      if (oat_dex_file == nullptr) {
        *failures += 1;
        continue;
      }
      std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
      if (dex_file.get() == nullptr) {
        *failures += 1;
      } else {
        dex_files->push_back(std::move(dex_file));
      }
    }

    if (index == 0) {
      // First file. See if this is a multi-image environment, and if so, enqueue the other images.
      const OatHeader& boot_oat_header = oat_file->GetOatHeader();
      const char* boot_cp = boot_oat_header.GetStoreValueByKey(OatHeader::kBootClassPathKey);
      if (boot_cp != nullptr) {
        gc::space::ImageSpace::ExtractMultiImageLocations(image_locations[0],
                                                          boot_cp,
                                                          &image_locations);
      }
    }

    Runtime::Current()->GetOatFileManager().RegisterOatFile(std::move(oat_file));
  }
  return true;
}


static size_t OpenDexFiles(const std::vector<std::string>& dex_filenames,
                           const std::vector<std::string>& dex_locations,
                           const std::string& image_location,
                           std::vector<std::unique_ptr<const DexFile>>* dex_files) {
  DCHECK(dex_files != nullptr) << "OpenDexFiles: out-param is nullptr";
  size_t failure_count = 0;
  if (!image_location.empty() && OpenDexFilesFromImage(image_location, dex_files, &failure_count)) {
    return failure_count;
  }
  failure_count = 0;
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i].c_str();
    const char* dex_location = dex_locations[i].c_str();
    std::string error_msg;
    if (!OS::FileExists(dex_filename)) {
      LOG(WARNING) << "Skipping non-existent dex file '" << dex_filename << "'";
      continue;
    }
    if (!DexFile::Open(dex_filename, dex_location, &error_msg, dex_files)) {
      LOG(WARNING) << "Failed to open .dex from file '" << dex_filename << "': " << error_msg;
      ++failure_count;
    }
  }
  return failure_count;
}

void Runtime::SetSentinel(mirror::Object* sentinel) {
  CHECK(sentinel_.Read() == nullptr);
  CHECK(sentinel != nullptr);
  CHECK(!heap_->IsMovableObject(sentinel));
  sentinel_ = GcRoot<mirror::Object>(sentinel);
}

bool Runtime::Init(RuntimeArgumentMap&& runtime_options_in) {
  // (b/30160149): protect subprocesses from modifications to LD_LIBRARY_PATH, etc.
  // Take a snapshot of the environment at the time the runtime was created, for use by Exec, etc.
  env_snapshot_.TakeSnapshot();

  RuntimeArgumentMap runtime_options(std::move(runtime_options_in));
  ScopedTrace trace(__FUNCTION__);
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  MemMap::Init();

  using Opt = RuntimeArgumentMap;
  VLOG(startup) << "Runtime::Init -verbose:startup enabled";

  QuasiAtomic::Startup();

  oat_file_manager_ = new OatFileManager;

  Thread::SetSensitiveThreadHook(runtime_options.GetOrDefault(Opt::HookIsSensitiveThread));
  Monitor::Init(runtime_options.GetOrDefault(Opt::LockProfThreshold));

  boot_class_path_string_ = runtime_options.ReleaseOrDefault(Opt::BootClassPath);
  class_path_string_ = runtime_options.ReleaseOrDefault(Opt::ClassPath);
  properties_ = runtime_options.ReleaseOrDefault(Opt::PropertiesList);

  compiler_callbacks_ = runtime_options.GetOrDefault(Opt::CompilerCallbacksPtr);
  patchoat_executable_ = runtime_options.ReleaseOrDefault(Opt::PatchOat);
  must_relocate_ = runtime_options.GetOrDefault(Opt::Relocate);
  is_zygote_ = runtime_options.Exists(Opt::Zygote);
  is_explicit_gc_disabled_ = runtime_options.Exists(Opt::DisableExplicitGC);
  dex2oat_enabled_ = runtime_options.GetOrDefault(Opt::Dex2Oat);
  image_dex2oat_enabled_ = runtime_options.GetOrDefault(Opt::ImageDex2Oat);
  dump_native_stack_on_sig_quit_ = runtime_options.GetOrDefault(Opt::DumpNativeStackOnSigQuit);

  vfprintf_ = runtime_options.GetOrDefault(Opt::HookVfprintf);
  exit_ = runtime_options.GetOrDefault(Opt::HookExit);
  abort_ = runtime_options.GetOrDefault(Opt::HookAbort);

  default_stack_size_ = runtime_options.GetOrDefault(Opt::StackSize);
  stack_trace_file_ = runtime_options.ReleaseOrDefault(Opt::StackTraceFile);

  compiler_executable_ = runtime_options.ReleaseOrDefault(Opt::Compiler);
  compiler_options_ = runtime_options.ReleaseOrDefault(Opt::CompilerOptions);
  image_compiler_options_ = runtime_options.ReleaseOrDefault(Opt::ImageCompilerOptions);
  image_location_ = runtime_options.GetOrDefault(Opt::Image);

  max_spins_before_thin_lock_inflation_ =
      runtime_options.GetOrDefault(Opt::MaxSpinsBeforeThinLockInflation);

  monitor_list_ = new MonitorList;
  monitor_pool_ = MonitorPool::Create();
  thread_list_ = new ThreadList;
  intern_table_ = new InternTable;

  verify_ = runtime_options.GetOrDefault(Opt::Verify);
  allow_dex_file_fallback_ = !runtime_options.Exists(Opt::NoDexFileFallback);

  no_sig_chain_ = runtime_options.Exists(Opt::NoSigChain);
  force_native_bridge_ = runtime_options.Exists(Opt::ForceNativeBridge);

  Split(runtime_options.GetOrDefault(Opt::CpuAbiList), ',', &cpu_abilist_);

  fingerprint_ = runtime_options.ReleaseOrDefault(Opt::Fingerprint);

  if (runtime_options.GetOrDefault(Opt::Interpret)) {
    GetInstrumentation()->ForceInterpretOnly();
  }

  zygote_max_failed_boots_ = runtime_options.GetOrDefault(Opt::ZygoteMaxFailedBoots);
  experimental_flags_ = runtime_options.GetOrDefault(Opt::Experimental);
  is_low_memory_mode_ = runtime_options.Exists(Opt::LowMemoryMode);

  {
    CompilerFilter::Filter filter;
    std::string filter_str = runtime_options.GetOrDefault(Opt::OatFileManagerCompilerFilter);
    if (!CompilerFilter::ParseCompilerFilter(filter_str.c_str(), &filter)) {
      LOG(ERROR) << "Cannot parse compiler filter " << filter_str;
      return false;
    }
    OatFileManager::SetCompilerFilter(filter);
  }

  XGcOption xgc_option = runtime_options.GetOrDefault(Opt::GcOption);
  heap_ = new gc::Heap(runtime_options.GetOrDefault(Opt::MemoryInitialSize),
                       runtime_options.GetOrDefault(Opt::HeapGrowthLimit),
                       runtime_options.GetOrDefault(Opt::HeapMinFree),
                       runtime_options.GetOrDefault(Opt::HeapMaxFree),
                       runtime_options.GetOrDefault(Opt::HeapTargetUtilization),
                       runtime_options.GetOrDefault(Opt::ForegroundHeapGrowthMultiplier),
                       runtime_options.GetOrDefault(Opt::MemoryMaximumSize),
                       runtime_options.GetOrDefault(Opt::NonMovingSpaceCapacity),
                       runtime_options.GetOrDefault(Opt::Image),
                       runtime_options.GetOrDefault(Opt::ImageInstructionSet),
                       xgc_option.collector_type_,
                       runtime_options.GetOrDefault(Opt::BackgroundGc),
                       runtime_options.GetOrDefault(Opt::LargeObjectSpace),
                       runtime_options.GetOrDefault(Opt::LargeObjectThreshold),
                       runtime_options.GetOrDefault(Opt::ParallelGCThreads),
                       runtime_options.GetOrDefault(Opt::ConcGCThreads),
                       runtime_options.Exists(Opt::LowMemoryMode),
                       runtime_options.GetOrDefault(Opt::LongPauseLogThreshold),
                       runtime_options.GetOrDefault(Opt::LongGCLogThreshold),
                       runtime_options.Exists(Opt::IgnoreMaxFootprint),
                       runtime_options.GetOrDefault(Opt::UseTLAB),
                       xgc_option.verify_pre_gc_heap_,
                       xgc_option.verify_pre_sweeping_heap_,
                       xgc_option.verify_post_gc_heap_,
                       xgc_option.verify_pre_gc_rosalloc_,
                       xgc_option.verify_pre_sweeping_rosalloc_,
                       xgc_option.verify_post_gc_rosalloc_,
                       xgc_option.gcstress_,
                       runtime_options.GetOrDefault(Opt::EnableHSpaceCompactForOOM),
                       runtime_options.GetOrDefault(Opt::HSpaceCompactForOOMMinIntervalsMs));

  if (!heap_->HasBootImageSpace() && !allow_dex_file_fallback_) {
    LOG(ERROR) << "Dex file fallback disabled, cannot continue without image.";
    return false;
  }

  dump_gc_performance_on_shutdown_ = runtime_options.Exists(Opt::DumpGCPerformanceOnShutdown);

  if (runtime_options.Exists(Opt::JdwpOptions)) {
    Dbg::ConfigureJdwp(runtime_options.GetOrDefault(Opt::JdwpOptions));
  }

  jit_options_.reset(jit::JitOptions::CreateFromRuntimeArguments(runtime_options));
  if (IsAotCompiler()) {
    // If we are already the compiler at this point, we must be dex2oat. Don't create the jit in
    // this case.
    // If runtime_options doesn't have UseJIT set to true then CreateFromRuntimeArguments returns
    // null and we don't create the jit.
    jit_options_->SetUseJitCompilation(false);
    jit_options_->SetSaveProfilingInfo(false);
  }

  // Allocate a global table of boxed lambda objects <-> closures.
  lambda_box_table_ = MakeUnique<lambda::BoxTable>();

  // Use MemMap arena pool for jit, malloc otherwise. Malloc arenas are faster to allocate but
  // can't be trimmed as easily.
  const bool use_malloc = IsAotCompiler();
  arena_pool_.reset(new ArenaPool(use_malloc, /* low_4gb */ false));
  jit_arena_pool_.reset(
      new ArenaPool(/* use_malloc */ false, /* low_4gb */ false, "CompilerMetadata"));

  if (IsAotCompiler() && Is64BitInstructionSet(kRuntimeISA)) {
    // 4gb, no malloc. Explanation in header.
    low_4gb_arena_pool_.reset(new ArenaPool(/* use_malloc */ false, /* low_4gb */ true));
  }
  linear_alloc_.reset(CreateLinearAlloc());

  BlockSignals();
  InitPlatformSignalHandlers();

  // Change the implicit checks flags based on runtime architecture.
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
    case kX86:
    case kArm64:
    case kX86_64:
    case kMips:
    case kMips64:
      implicit_null_checks_ = true;
      // Installing stack protection does not play well with valgrind.
      implicit_so_checks_ = !(RUNNING_ON_MEMORY_TOOL && kMemoryToolIsValgrind);
      break;
    default:
      // Keep the defaults.
      break;
  }

  if (!no_sig_chain_) {
    // Dex2Oat's Runtime does not need the signal chain or the fault handler.

    // Initialize the signal chain so that any calls to sigaction get
    // correctly routed to the next in the chain regardless of whether we
    // have claimed the signal or not.
    InitializeSignalChain();

    if (implicit_null_checks_ || implicit_so_checks_ || implicit_suspend_checks_) {
      fault_manager.Init();

      // These need to be in a specific order.  The null point check handler must be
      // after the suspend check and stack overflow check handlers.
      //
      // Note: the instances attach themselves to the fault manager and are handled by it. The manager
      //       will delete the instance on Shutdown().
      if (implicit_suspend_checks_) {
        new SuspensionHandler(&fault_manager);
      }

      if (implicit_so_checks_) {
        new StackOverflowHandler(&fault_manager);
      }

      if (implicit_null_checks_) {
        new NullPointerHandler(&fault_manager);
      }

      if (kEnableJavaStackTraceHandler) {
        new JavaStackTraceHandler(&fault_manager);
      }
    }
  }

  java_vm_ = new JavaVMExt(this, runtime_options);

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread without creating
  // objects. We can't supply a thread group yet; it will be fixed later. Since we are the main
  // thread, we do not get a java peer.
  Thread* self = Thread::Attach("main", false, nullptr, false);
  CHECK_EQ(self->GetThreadId(), ThreadList::kMainThreadId);
  CHECK(self != nullptr);

  self->SetCanCallIntoJava(!IsAotCompiler());

  // Set us to runnable so tools using a runtime can allocate and GC by default
  self->TransitionFromSuspendedToRunnable();

  // Now we're attached, we can take the heap locks and validate the heap.
  GetHeap()->EnableObjectValidation();

  CHECK_GE(GetHeap()->GetContinuousSpaces().size(), 1U);
  class_linker_ = new ClassLinker(intern_table_);
  if (GetHeap()->HasBootImageSpace()) {
    std::string error_msg;
    bool result = class_linker_->InitFromBootImage(&error_msg);
    if (!result) {
      LOG(ERROR) << "Could not initialize from image: " << error_msg;
      return false;
    }
    if (kIsDebugBuild) {
      for (auto image_space : GetHeap()->GetBootImageSpaces()) {
        image_space->VerifyImageAllocations();
      }
    }
    if (boot_class_path_string_.empty()) {
      // The bootclasspath is not explicitly specified: construct it from the loaded dex files.
      const std::vector<const DexFile*>& boot_class_path = GetClassLinker()->GetBootClassPath();
      std::vector<std::string> dex_locations;
      dex_locations.reserve(boot_class_path.size());
      for (const DexFile* dex_file : boot_class_path) {
        dex_locations.push_back(dex_file->GetLocation());
      }
      boot_class_path_string_ = Join(dex_locations, ':');
    }
    {
      ScopedTrace trace2("AddImageStringsToTable");
      GetInternTable()->AddImagesStringsToTable(heap_->GetBootImageSpaces());
    }
    {
      ScopedTrace trace2("MoveImageClassesToClassTable");
      GetClassLinker()->AddBootImageClassesToClassTable();
    }
  } else {
    std::vector<std::string> dex_filenames;
    Split(boot_class_path_string_, ':', &dex_filenames);

    std::vector<std::string> dex_locations;
    if (!runtime_options.Exists(Opt::BootClassPathLocations)) {
      dex_locations = dex_filenames;
    } else {
      dex_locations = runtime_options.GetOrDefault(Opt::BootClassPathLocations);
      CHECK_EQ(dex_filenames.size(), dex_locations.size());
    }

    std::vector<std::unique_ptr<const DexFile>> boot_class_path;
    if (runtime_options.Exists(Opt::BootClassPathDexList)) {
      boot_class_path.swap(*runtime_options.GetOrDefault(Opt::BootClassPathDexList));
    } else {
      OpenDexFiles(dex_filenames,
                   dex_locations,
                   runtime_options.GetOrDefault(Opt::Image),
                   &boot_class_path);
    }
    instruction_set_ = runtime_options.GetOrDefault(Opt::ImageInstructionSet);
    std::string error_msg;
    if (!class_linker_->InitWithoutImage(std::move(boot_class_path), &error_msg)) {
      LOG(ERROR) << "Could not initialize without image: " << error_msg;
      return false;
    }

    // TODO: Should we move the following to InitWithoutImage?
    SetInstructionSet(instruction_set_);
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!HasCalleeSaveMethod(type)) {
        SetCalleeSaveMethod(CreateCalleeSaveMethod(), type);
      }
    }
  }

  CHECK(class_linker_ != nullptr);

  verifier::MethodVerifier::Init();

  if (runtime_options.Exists(Opt::MethodTrace)) {
    trace_config_.reset(new TraceConfig());
    trace_config_->trace_file = runtime_options.ReleaseOrDefault(Opt::MethodTraceFile);
    trace_config_->trace_file_size = runtime_options.ReleaseOrDefault(Opt::MethodTraceFileSize);
    trace_config_->trace_mode = Trace::TraceMode::kMethodTracing;
    trace_config_->trace_output_mode = runtime_options.Exists(Opt::MethodTraceStreaming) ?
        Trace::TraceOutputMode::kStreaming :
        Trace::TraceOutputMode::kFile;
  }

  {
    auto&& profiler_options = runtime_options.ReleaseOrDefault(Opt::ProfilerOpts);
    profile_output_filename_ = profiler_options.output_file_name_;

    // TODO: Don't do this, just change ProfilerOptions to include the output file name?
    ProfilerOptions other_options(
        profiler_options.enabled_,
        profiler_options.period_s_,
        profiler_options.duration_s_,
        profiler_options.interval_us_,
        profiler_options.backoff_coefficient_,
        profiler_options.start_immediately_,
        profiler_options.top_k_threshold_,
        profiler_options.top_k_change_threshold_,
        profiler_options.profile_type_,
        profiler_options.max_stack_depth_);

    profiler_options_ = other_options;
  }

  // TODO: move this to just be an Trace::Start argument
  Trace::SetDefaultClockSource(runtime_options.GetOrDefault(Opt::ProfileClock));

  // Pre-allocate an OutOfMemoryError for the double-OOME case.
  self->ThrowNewException("Ljava/lang/OutOfMemoryError;",
                          "OutOfMemoryError thrown while trying to throw OutOfMemoryError; "
                          "no stack trace available");
  pre_allocated_OutOfMemoryError_ = GcRoot<mirror::Throwable>(self->GetException());
  self->ClearException();

  // Pre-allocate a NoClassDefFoundError for the common case of failing to find a system class
  // ahead of checking the application's class loader.
  self->ThrowNewException("Ljava/lang/NoClassDefFoundError;",
                          "Class not found using the boot class loader; no stack trace available");
  pre_allocated_NoClassDefFoundError_ = GcRoot<mirror::Throwable>(self->GetException());
  self->ClearException();

  // Look for a native bridge.
  //
  // The intended flow here is, in the case of a running system:
  //
  // Runtime::Init() (zygote):
  //   LoadNativeBridge -> dlopen from cmd line parameter.
  //  |
  //  V
  // Runtime::Start() (zygote):
  //   No-op wrt native bridge.
  //  |
  //  | start app
  //  V
  // DidForkFromZygote(action)
  //   action = kUnload -> dlclose native bridge.
  //   action = kInitialize -> initialize library
  //
  //
  // The intended flow here is, in the case of a simple dalvikvm call:
  //
  // Runtime::Init():
  //   LoadNativeBridge -> dlopen from cmd line parameter.
  //  |
  //  V
  // Runtime::Start():
  //   DidForkFromZygote(kInitialize) -> try to initialize any native bridge given.
  //   No-op wrt native bridge.
  {
    std::string native_bridge_file_name = runtime_options.ReleaseOrDefault(Opt::NativeBridge);
    is_native_bridge_loaded_ = LoadNativeBridge(native_bridge_file_name);
  }

  VLOG(startup) << "Runtime::Init exiting";

  return true;
}

void Runtime::InitNativeMethods() {
  VLOG(startup) << "Runtime::InitNativeMethods entering";
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Must be in the kNative state for calling native methods (JNI_OnLoad code).
  CHECK_EQ(self->GetState(), kNative);

  // First set up JniConstants, which is used by both the runtime's built-in native
  // methods and libcore.
  JniConstants::init(env);

  // Then set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Initialize classes used in JNI. The initialization requires runtime native
  // methods to be loaded first.
  WellKnownClasses::Init(env);

  // Then set up libjavacore / libopenjdk, which are just a regular JNI libraries with
  // a regular JNI_OnLoad. Most JNI libraries can just use System.loadLibrary, but
  // libcore can't because it's the library that implements System.loadLibrary!
  {
    std::string error_msg;
    if (!java_vm_->LoadNativeLibrary(env, "libjavacore.so", nullptr, nullptr, &error_msg)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"libjavacore.so\": " << error_msg;
    }
  }
  {
    constexpr const char* kOpenJdkLibrary = kIsDebugBuild
                                                ? "libopenjdkd.so"
                                                : "libopenjdk.so";
    std::string error_msg;
    if (!java_vm_->LoadNativeLibrary(env, kOpenJdkLibrary, nullptr, nullptr, &error_msg)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"" << kOpenJdkLibrary << "\": " << error_msg;
    }
  }

  // Initialize well known classes that may invoke runtime native methods.
  WellKnownClasses::LateInit(env);

  VLOG(startup) << "Runtime::InitNativeMethods exiting";
}

void Runtime::ReclaimArenaPoolMemory() {
  arena_pool_->LockReclaimMemory();
}

void Runtime::InitThreadGroups(Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedJniEnvLocalRefState env_state(env);
  main_thread_group_ =
      env->NewGlobalRef(env->GetStaticObjectField(
          WellKnownClasses::java_lang_ThreadGroup,
          WellKnownClasses::java_lang_ThreadGroup_mainThreadGroup));
  CHECK(main_thread_group_ != nullptr || IsAotCompiler());
  system_thread_group_ =
      env->NewGlobalRef(env->GetStaticObjectField(
          WellKnownClasses::java_lang_ThreadGroup,
          WellKnownClasses::java_lang_ThreadGroup_systemThreadGroup));
  CHECK(system_thread_group_ != nullptr || IsAotCompiler());
}

jobject Runtime::GetMainThreadGroup() const {
  CHECK(main_thread_group_ != nullptr || IsAotCompiler());
  return main_thread_group_;
}

jobject Runtime::GetSystemThreadGroup() const {
  CHECK(system_thread_group_ != nullptr || IsAotCompiler());
  return system_thread_group_;
}

jobject Runtime::GetSystemClassLoader() const {
  CHECK(system_class_loader_ != nullptr || IsAotCompiler());
  return system_class_loader_;
}

void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
  register_dalvik_system_DexFile(env);
  register_dalvik_system_VMDebug(env);
  register_dalvik_system_VMRuntime(env);
  register_dalvik_system_VMStack(env);
  register_dalvik_system_ZygoteHooks(env);
  register_java_lang_Class(env);
  register_java_lang_DexCache(env);
  register_java_lang_Object(env);
  register_java_lang_ref_FinalizerReference(env);
  register_java_lang_reflect_AbstractMethod(env);
  register_java_lang_reflect_Array(env);
  register_java_lang_reflect_Constructor(env);
  register_java_lang_reflect_Field(env);
  register_java_lang_reflect_Method(env);
  register_java_lang_reflect_Proxy(env);
  register_java_lang_ref_Reference(env);
  register_java_lang_String(env);
  register_java_lang_StringFactory(env);
  register_java_lang_System(env);
  register_java_lang_Thread(env);
  register_java_lang_Throwable(env);
  register_java_lang_VMClassLoader(env);
  register_java_util_concurrent_atomic_AtomicLong(env);
  register_libcore_util_CharsetUtils(env);
  register_org_apache_harmony_dalvik_ddmc_DdmServer(env);
  register_org_apache_harmony_dalvik_ddmc_DdmVmInternal(env);
  register_sun_misc_Unsafe(env);
  //patch by Youlor
  //++++++++++++++++++++++++++++
  Unpacker::register_cn_youlor_Unpacker(env);
  //++++++++++++++++++++++++++++
}

void Runtime::DumpForSigQuit(std::ostream& os) {
  GetClassLinker()->DumpForSigQuit(os);
  GetInternTable()->DumpForSigQuit(os);
  GetJavaVM()->DumpForSigQuit(os);
  GetHeap()->DumpForSigQuit(os);
  oat_file_manager_->DumpForSigQuit(os);
  if (GetJit() != nullptr) {
    GetJit()->DumpForSigQuit(os);
  } else {
    os << "Running non JIT\n";
  }
  TrackedAllocators::Dump(os);
  os << "\n";

  thread_list_->DumpForSigQuit(os);
  BaseMutex::DumpAll(os);
}

void Runtime::DumpLockHolders(std::ostream& os) {
  uint64_t mutator_lock_owner = Locks::mutator_lock_->GetExclusiveOwnerTid();
  pid_t thread_list_lock_owner = GetThreadList()->GetLockOwner();
  pid_t classes_lock_owner = GetClassLinker()->GetClassesLockOwner();
  pid_t dex_lock_owner = GetClassLinker()->GetDexLockOwner();
  if ((thread_list_lock_owner | classes_lock_owner | dex_lock_owner) != 0) {
    os << "Mutator lock exclusive owner tid: " << mutator_lock_owner << "\n"
       << "ThreadList lock owner tid: " << thread_list_lock_owner << "\n"
       << "ClassLinker classes lock owner tid: " << classes_lock_owner << "\n"
       << "ClassLinker dex lock owner tid: " << dex_lock_owner << "\n";
  }
}

void Runtime::SetStatsEnabled(bool new_state) {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::instrument_entrypoints_lock_);
  if (new_state == true) {
    GetStats()->Clear(~0);
    // TODO: wouldn't it make more sense to clear _all_ threads' stats?
    self->GetStats()->Clear(~0);
    if (stats_enabled_ != new_state) {
      GetInstrumentation()->InstrumentQuickAllocEntryPointsLocked();
    }
  } else if (stats_enabled_ != new_state) {
    GetInstrumentation()->UninstrumentQuickAllocEntryPointsLocked();
  }
  stats_enabled_ = new_state;
}

void Runtime::ResetStats(int kinds) {
  GetStats()->Clear(kinds & 0xffff);
  // TODO: wouldn't it make more sense to clear _all_ threads' stats?
  Thread::Current()->GetStats()->Clear(kinds >> 16);
}

int32_t Runtime::GetStat(int kind) {
  RuntimeStats* stats;
  if (kind < (1<<16)) {
    stats = GetStats();
  } else {
    stats = Thread::Current()->GetStats();
    kind >>= 16;
  }
  switch (kind) {
  case KIND_ALLOCATED_OBJECTS:
    return stats->allocated_objects;
  case KIND_ALLOCATED_BYTES:
    return stats->allocated_bytes;
  case KIND_FREED_OBJECTS:
    return stats->freed_objects;
  case KIND_FREED_BYTES:
    return stats->freed_bytes;
  case KIND_GC_INVOCATIONS:
    return stats->gc_for_alloc_count;
  case KIND_CLASS_INIT_COUNT:
    return stats->class_init_count;
  case KIND_CLASS_INIT_TIME:
    // Convert ns to us, reduce to 32 bits.
    return static_cast<int>(stats->class_init_time_ns / 1000);
  case KIND_EXT_ALLOCATED_OBJECTS:
  case KIND_EXT_ALLOCATED_BYTES:
  case KIND_EXT_FREED_OBJECTS:
  case KIND_EXT_FREED_BYTES:
    return 0;  // backward compatibility
  default:
    LOG(FATAL) << "Unknown statistic " << kind;
    return -1;  // unreachable
  }
}

void Runtime::BlockSignals() {
  SignalSet signals;
  signals.Add(SIGPIPE);
  // SIGQUIT is used to dump the runtime's state (including stack traces).
  signals.Add(SIGQUIT);
  // SIGUSR1 is used to initiate a GC.
  signals.Add(SIGUSR1);
  signals.Block();
}

bool Runtime::AttachCurrentThread(const char* thread_name, bool as_daemon, jobject thread_group,
                                  bool create_peer) {
  ScopedTrace trace(__FUNCTION__);
  return Thread::Attach(thread_name, as_daemon, thread_group, create_peer) != nullptr;
}

void Runtime::DetachCurrentThread() {
  ScopedTrace trace(__FUNCTION__);
  Thread* self = Thread::Current();
  if (self == nullptr) {
    LOG(FATAL) << "attempting to detach thread that is not attached";
  }
  if (self->HasManagedStack()) {
    LOG(FATAL) << *Thread::Current() << " attempting to detach while still running code";
  }
  thread_list_->Unregister(self);
}

mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryError() {
  mirror::Throwable* oome = pre_allocated_OutOfMemoryError_.Read();
  if (oome == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated OOME";
  }
  return oome;
}

mirror::Throwable* Runtime::GetPreAllocatedNoClassDefFoundError() {
  mirror::Throwable* ncdfe = pre_allocated_NoClassDefFoundError_.Read();
  if (ncdfe == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated NoClassDefFoundError";
  }
  return ncdfe;
}

void Runtime::VisitConstantRoots(RootVisitor* visitor) {
  // Visit the classes held as static in mirror classes, these can be visited concurrently and only
  // need to be visited once per GC since they never change.
  mirror::Class::VisitRoots(visitor);
  mirror::Constructor::VisitRoots(visitor);
  mirror::Reference::VisitRoots(visitor);
  mirror::Method::VisitRoots(visitor);
  mirror::StackTraceElement::VisitRoots(visitor);
  mirror::String::VisitRoots(visitor);
  mirror::Throwable::VisitRoots(visitor);
  mirror::Field::VisitRoots(visitor);
  // Visit all the primitive array types classes.
  mirror::PrimitiveArray<uint8_t>::VisitRoots(visitor);   // BooleanArray
  mirror::PrimitiveArray<int8_t>::VisitRoots(visitor);    // ByteArray
  mirror::PrimitiveArray<uint16_t>::VisitRoots(visitor);  // CharArray
  mirror::PrimitiveArray<double>::VisitRoots(visitor);    // DoubleArray
  mirror::PrimitiveArray<float>::VisitRoots(visitor);     // FloatArray
  mirror::PrimitiveArray<int32_t>::VisitRoots(visitor);   // IntArray
  mirror::PrimitiveArray<int64_t>::VisitRoots(visitor);   // LongArray
  mirror::PrimitiveArray<int16_t>::VisitRoots(visitor);   // ShortArray
  // Visiting the roots of these ArtMethods is not currently required since all the GcRoots are
  // null.
  BufferedRootVisitor<16> buffered_visitor(visitor, RootInfo(kRootVMInternal));
  const size_t pointer_size = GetClassLinker()->GetImagePointerSize();
  if (HasResolutionMethod()) {
    resolution_method_->VisitRoots(buffered_visitor, pointer_size);
  }
  if (HasImtConflictMethod()) {
    imt_conflict_method_->VisitRoots(buffered_visitor, pointer_size);
  }
  if (imt_unimplemented_method_ != nullptr) {
    imt_unimplemented_method_->VisitRoots(buffered_visitor, pointer_size);
  }
  for (size_t i = 0; i < kLastCalleeSaveType; ++i) {
    auto* m = reinterpret_cast<ArtMethod*>(callee_save_methods_[i]);
    if (m != nullptr) {
      m->VisitRoots(buffered_visitor, pointer_size);
    }
  }
}

void Runtime::VisitConcurrentRoots(RootVisitor* visitor, VisitRootFlags flags) {
  intern_table_->VisitRoots(visitor, flags);
  class_linker_->VisitRoots(visitor, flags);
  heap_->VisitAllocationRecords(visitor);
  if ((flags & kVisitRootFlagNewRoots) == 0) {
    // Guaranteed to have no new roots in the constant roots.
    VisitConstantRoots(visitor);
  }
  Dbg::VisitRoots(visitor);
}

void Runtime::VisitTransactionRoots(RootVisitor* visitor) {
  if (preinitialization_transaction_ != nullptr) {
    preinitialization_transaction_->VisitRoots(visitor);
  }
}

void Runtime::VisitNonThreadRoots(RootVisitor* visitor) {
  java_vm_->VisitRoots(visitor);
  sentinel_.VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  pre_allocated_OutOfMemoryError_.VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  pre_allocated_NoClassDefFoundError_.VisitRootIfNonNull(visitor, RootInfo(kRootVMInternal));
  verifier::MethodVerifier::VisitStaticRoots(visitor);
  VisitTransactionRoots(visitor);
}

void Runtime::VisitNonConcurrentRoots(RootVisitor* visitor) {
  thread_list_->VisitRoots(visitor);
  VisitNonThreadRoots(visitor);
}

void Runtime::VisitThreadRoots(RootVisitor* visitor) {
  thread_list_->VisitRoots(visitor);
}

size_t Runtime::FlipThreadRoots(Closure* thread_flip_visitor, Closure* flip_callback,
                                gc::collector::GarbageCollector* collector) {
  return thread_list_->FlipThreadRoots(thread_flip_visitor, flip_callback, collector);
}

void Runtime::VisitRoots(RootVisitor* visitor, VisitRootFlags flags) {
  VisitNonConcurrentRoots(visitor);
  VisitConcurrentRoots(visitor, flags);
}

void Runtime::VisitImageRoots(RootVisitor* visitor) {
  for (auto* space : GetHeap()->GetContinuousSpaces()) {
    if (space->IsImageSpace()) {
      auto* image_space = space->AsImageSpace();
      const auto& image_header = image_space->GetImageHeader();
      for (size_t i = 0; i < ImageHeader::kImageRootsMax; ++i) {
        auto* obj = image_header.GetImageRoot(static_cast<ImageHeader::ImageRoot>(i));
        if (obj != nullptr) {
          auto* after_obj = obj;
          visitor->VisitRoot(&after_obj, RootInfo(kRootStickyClass));
          CHECK_EQ(after_obj, obj);
        }
      }
    }
  }
}

ArtMethod* Runtime::CreateImtConflictMethod(LinearAlloc* linear_alloc) {
  ClassLinker* const class_linker = GetClassLinker();
  ArtMethod* method = class_linker->CreateRuntimeMethod(linear_alloc);
  // When compiling, the code pointer will get set later when the image is loaded.
  const size_t pointer_size = GetInstructionSetPointerSize(instruction_set_);
  if (IsAotCompiler()) {
    method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  } else {
    method->SetEntryPointFromQuickCompiledCode(GetQuickImtConflictStub());
  }
  // Create empty conflict table.
  method->SetImtConflictTable(class_linker->CreateImtConflictTable(/*count*/0u, linear_alloc),
                              pointer_size);
  return method;
}

void Runtime::SetImtConflictMethod(ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(method->IsRuntimeMethod());
  imt_conflict_method_ = method;
}

ArtMethod* Runtime::CreateResolutionMethod() {
  auto* method = GetClassLinker()->CreateRuntimeMethod(GetLinearAlloc());
  // When compiling, the code pointer will get set later when the image is loaded.
  if (IsAotCompiler()) {
    size_t pointer_size = GetInstructionSetPointerSize(instruction_set_);
    method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  } else {
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionStub());
  }
  return method;
}

ArtMethod* Runtime::CreateCalleeSaveMethod() {
  auto* method = GetClassLinker()->CreateRuntimeMethod(GetLinearAlloc());
  size_t pointer_size = GetInstructionSetPointerSize(instruction_set_);
  method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  DCHECK_NE(instruction_set_, kNone);
  DCHECK(method->IsRuntimeMethod());
  return method;
}

void Runtime::DisallowNewSystemWeaks() {
  CHECK(!kUseReadBarrier);
  monitor_list_->DisallowNewMonitors();
  intern_table_->ChangeWeakRootState(gc::kWeakRootStateNoReadsOrWrites);
  java_vm_->DisallowNewWeakGlobals();
  heap_->DisallowNewAllocationRecords();
  lambda_box_table_->DisallowNewWeakBoxedLambdas();
}

void Runtime::AllowNewSystemWeaks() {
  CHECK(!kUseReadBarrier);
  monitor_list_->AllowNewMonitors();
  intern_table_->ChangeWeakRootState(gc::kWeakRootStateNormal);  // TODO: Do this in the sweeping.
  java_vm_->AllowNewWeakGlobals();
  heap_->AllowNewAllocationRecords();
  lambda_box_table_->AllowNewWeakBoxedLambdas();
}

void Runtime::BroadcastForNewSystemWeaks() {
  // This is used for the read barrier case that uses the thread-local
  // Thread::GetWeakRefAccessEnabled() flag.
  CHECK(kUseReadBarrier);
  monitor_list_->BroadcastForNewMonitors();
  intern_table_->BroadcastForNewInterns();
  java_vm_->BroadcastForNewWeakGlobals();
  heap_->BroadcastForNewAllocationRecords();
  lambda_box_table_->BroadcastForNewWeakBoxedLambdas();
}

void Runtime::SetInstructionSet(InstructionSet instruction_set) {
  instruction_set_ = instruction_set;
  if ((instruction_set_ == kThumb2) || (instruction_set_ == kArm)) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = arm::ArmCalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kMips) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = mips::MipsCalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kMips64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = mips64::Mips64CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kX86) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = x86::X86CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kX86_64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = x86_64::X86_64CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kArm64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = arm64::Arm64CalleeSaveMethodFrameInfo(type);
    }
  } else {
    UNIMPLEMENTED(FATAL) << instruction_set_;
  }
}

void Runtime::SetCalleeSaveMethod(ArtMethod* method, CalleeSaveType type) {
  DCHECK_LT(static_cast<int>(type), static_cast<int>(kLastCalleeSaveType));
  CHECK(method != nullptr);
  callee_save_methods_[type] = reinterpret_cast<uintptr_t>(method);
}

void Runtime::RegisterAppInfo(const std::vector<std::string>& code_paths,
                              const std::string& profile_output_filename,
                              const std::string& foreign_dex_profile_path,
                              const std::string& app_dir) {
  if (jit_.get() == nullptr) {
    // We are not JITing. Nothing to do.
    return;
  }

  VLOG(profiler) << "Register app with " << profile_output_filename
      << " " << Join(code_paths, ':');

  if (profile_output_filename.empty()) {
    LOG(WARNING) << "JIT profile information will not be recorded: profile filename is empty.";
    return;
  }
  if (!FileExists(profile_output_filename)) {
    LOG(WARNING) << "JIT profile information will not be recorded: profile file does not exits.";
    return;
  }
  if (code_paths.empty()) {
    LOG(WARNING) << "JIT profile information will not be recorded: code paths is empty.";
    return;
  }

  profile_output_filename_ = profile_output_filename;
  jit_->StartProfileSaver(profile_output_filename,
                          code_paths,
                          foreign_dex_profile_path,
                          app_dir);
}

void Runtime::NotifyDexLoaded(const std::string& dex_location) {
  VLOG(profiler) << "Notify dex loaded: " << dex_location;
  // We know that if the ProfileSaver is started then we can record profile information.
  if (ProfileSaver::IsStarted()) {
    ProfileSaver::NotifyDexUse(dex_location);
  }
}

// Transaction support.
void Runtime::EnterTransactionMode(Transaction* transaction) {
  DCHECK(IsAotCompiler());
  DCHECK(transaction != nullptr);
  DCHECK(!IsActiveTransaction());
  preinitialization_transaction_ = transaction;
}

void Runtime::ExitTransactionMode() {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_ = nullptr;
}

bool Runtime::IsTransactionAborted() const {
  if (!IsActiveTransaction()) {
    return false;
  } else {
    DCHECK(IsAotCompiler());
    return preinitialization_transaction_->IsAborted();
  }
}

void Runtime::AbortTransactionAndThrowAbortError(Thread* self, const std::string& abort_message) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  // Throwing an exception may cause its class initialization. If we mark the transaction
  // aborted before that, we may warn with a false alarm. Throwing the exception before
  // marking the transaction aborted avoids that.
  preinitialization_transaction_->ThrowAbortError(self, &abort_message);
  preinitialization_transaction_->Abort(abort_message);
}

void Runtime::ThrowTransactionAbortError(Thread* self) {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  // Passing nullptr means we rethrow an exception with the earlier transaction abort message.
  preinitialization_transaction_->ThrowAbortError(self, nullptr);
}

void Runtime::RecordWriteFieldBoolean(mirror::Object* obj, MemberOffset field_offset,
                                      uint8_t value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldBoolean(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldByte(mirror::Object* obj, MemberOffset field_offset,
                                   int8_t value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldByte(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldChar(mirror::Object* obj, MemberOffset field_offset,
                                   uint16_t value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldChar(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldShort(mirror::Object* obj, MemberOffset field_offset,
                                    int16_t value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldShort(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteField32(mirror::Object* obj, MemberOffset field_offset,
                                 uint32_t value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteField32(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteField64(mirror::Object* obj, MemberOffset field_offset,
                                 uint64_t value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteField64(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldReference(mirror::Object* obj, MemberOffset field_offset,
                                        mirror::Object* value, bool is_volatile) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldReference(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteArray(mirror::Array* array, size_t index, uint64_t value) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteArray(array, index, value);
}

void Runtime::RecordStrongStringInsertion(mirror::String* s) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringInsertion(s);
}

void Runtime::RecordWeakStringInsertion(mirror::String* s) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringInsertion(s);
}

void Runtime::RecordStrongStringRemoval(mirror::String* s) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringRemoval(s);
}

void Runtime::RecordWeakStringRemoval(mirror::String* s) const {
  DCHECK(IsAotCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringRemoval(s);
}

void Runtime::SetFaultMessage(const std::string& message) {
  MutexLock mu(Thread::Current(), fault_message_lock_);
  fault_message_ = message;
}

void Runtime::AddCurrentRuntimeFeaturesAsDex2OatArguments(std::vector<std::string>* argv)
    const {
  if (GetInstrumentation()->InterpretOnly()) {
    argv->push_back("--compiler-filter=interpret-only");
  }

  // Make the dex2oat instruction set match that of the launching runtime. If we have multiple
  // architecture support, dex2oat may be compiled as a different instruction-set than that
  // currently being executed.
  std::string instruction_set("--instruction-set=");
  instruction_set += GetInstructionSetString(kRuntimeISA);
  argv->push_back(instruction_set);

  std::unique_ptr<const InstructionSetFeatures> features(InstructionSetFeatures::FromCppDefines());
  std::string feature_string("--instruction-set-features=");
  feature_string += features->GetFeatureString();
  argv->push_back(feature_string);
}

void Runtime::CreateJit() {
  CHECK(!IsAotCompiler());
  if (kIsDebugBuild && GetInstrumentation()->IsForcedInterpretOnly()) {
    DCHECK(!jit_options_->UseJitCompilation());
  }
  std::string error_msg;
  jit_.reset(jit::Jit::Create(jit_options_.get(), &error_msg));
  if (jit_.get() == nullptr) {
    LOG(WARNING) << "Failed to create JIT " << error_msg;
  }
}

bool Runtime::CanRelocate() const {
  return !IsAotCompiler() || compiler_callbacks_->IsRelocationPossible();
}

bool Runtime::IsCompilingBootImage() const {
  return IsCompiler() && compiler_callbacks_->IsBootImage();
}

void Runtime::SetResolutionMethod(ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(method->IsRuntimeMethod()) << method;
  resolution_method_ = method;
}

void Runtime::SetImtUnimplementedMethod(ArtMethod* method) {
  CHECK(method != nullptr);
  CHECK(method->IsRuntimeMethod());
  imt_unimplemented_method_ = method;
}

void Runtime::FixupConflictTables() {
  // We can only do this after the class linker is created.
  const size_t pointer_size = GetClassLinker()->GetImagePointerSize();
  if (imt_unimplemented_method_->GetImtConflictTable(pointer_size) == nullptr) {
    imt_unimplemented_method_->SetImtConflictTable(
        ClassLinker::CreateImtConflictTable(/*count*/0u, GetLinearAlloc(), pointer_size),
        pointer_size);
  }
  if (imt_conflict_method_->GetImtConflictTable(pointer_size) == nullptr) {
    imt_conflict_method_->SetImtConflictTable(
          ClassLinker::CreateImtConflictTable(/*count*/0u, GetLinearAlloc(), pointer_size),
          pointer_size);
  }
}

bool Runtime::IsVerificationEnabled() const {
  return verify_ == verifier::VerifyMode::kEnable ||
      verify_ == verifier::VerifyMode::kSoftFail;
}

bool Runtime::IsVerificationSoftFail() const {
  return verify_ == verifier::VerifyMode::kSoftFail;
}

LinearAlloc* Runtime::CreateLinearAlloc() {
  // For 64 bit compilers, it needs to be in low 4GB in the case where we are cross compiling for a
  // 32 bit target. In this case, we have 32 bit pointers in the dex cache arrays which can't hold
  // when we have 64 bit ArtMethod pointers.
  return (IsAotCompiler() && Is64BitInstructionSet(kRuntimeISA))
      ? new LinearAlloc(low_4gb_arena_pool_.get())
      : new LinearAlloc(arena_pool_.get());
}

double Runtime::GetHashTableMinLoadFactor() const {
  return is_low_memory_mode_ ? kLowMemoryMinLoadFactor : kNormalMinLoadFactor;
}

double Runtime::GetHashTableMaxLoadFactor() const {
  return is_low_memory_mode_ ? kLowMemoryMaxLoadFactor : kNormalMaxLoadFactor;
}

void Runtime::UpdateProcessState(ProcessState process_state) {
  ProcessState old_process_state = process_state_;
  process_state_ = process_state;
  GetHeap()->UpdateProcessState(old_process_state, process_state);
}

void Runtime::RegisterSensitiveThread() const {
  Thread::SetJitSensitiveThread();
}

// Returns true if JIT compilations are enabled. GetJit() will be not null in this case.
bool Runtime::UseJitCompilation() const {
  return (jit_ != nullptr) && jit_->UseJitCompilation();
}

// Returns true if profile saving is enabled. GetJit() will be not null in this case.
bool Runtime::SaveProfileInfo() const {
  return (jit_ != nullptr) && jit_->SaveProfilingInfo();
}

void Runtime::EnvSnapshot::TakeSnapshot() {
  char** env = GetEnviron();
  for (size_t i = 0; env[i] != nullptr; ++i) {
    name_value_pairs_.emplace_back(new std::string(env[i]));
  }
  // The strings in name_value_pairs_ retain ownership of the c_str, but we assign pointers
  // for quick use by GetSnapshot.  This avoids allocation and copying cost at Exec.
  c_env_vector_.reset(new char*[name_value_pairs_.size() + 1]);
  for (size_t i = 0; env[i] != nullptr; ++i) {
    c_env_vector_[i] = const_cast<char*>(name_value_pairs_[i]->c_str());
  }
  c_env_vector_[name_value_pairs_.size()] = nullptr;
}

char** Runtime::EnvSnapshot::GetSnapshot() const {
  return c_env_vector_.get();
}

}  // namespace art
