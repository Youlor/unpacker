/*
 * Copyright 2014 The Android Open Source Project
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

#include "jit.h"

#include <dlfcn.h>

#include "art_method-inl.h"
#include "debugger.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"
#include "jit_code_cache.h"
#include "oat_file_manager.h"
#include "oat_quick_method_header.h"
#include "offline_profiling_info.h"
#include "profile_saver.h"
#include "runtime.h"
#include "runtime_options.h"
#include "stack_map.h"
#include "thread_list.h"
#include "utils.h"

namespace art {
namespace jit {

static constexpr bool kEnableOnStackReplacement = true;
// At what priority to schedule jit threads. 9 is the lowest foreground priority on device.
static constexpr int kJitPoolThreadPthreadPriority = 9;

// JIT compiler
void* Jit::jit_library_handle_= nullptr;
void* Jit::jit_compiler_handle_ = nullptr;
void* (*Jit::jit_load_)(bool*) = nullptr;
void (*Jit::jit_unload_)(void*) = nullptr;
bool (*Jit::jit_compile_method_)(void*, ArtMethod*, Thread*, bool) = nullptr;
void (*Jit::jit_types_loaded_)(void*, mirror::Class**, size_t count) = nullptr;
bool Jit::generate_debug_info_ = false;

JitOptions* JitOptions::CreateFromRuntimeArguments(const RuntimeArgumentMap& options) {
  auto* jit_options = new JitOptions;
  jit_options->use_jit_compilation_ = options.GetOrDefault(RuntimeArgumentMap::UseJitCompilation);

  jit_options->code_cache_initial_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheInitialCapacity);
  jit_options->code_cache_max_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheMaxCapacity);
  jit_options->dump_info_on_shutdown_ =
      options.Exists(RuntimeArgumentMap::DumpJITInfoOnShutdown);
  jit_options->save_profiling_info_ =
      options.GetOrDefault(RuntimeArgumentMap::JITSaveProfilingInfo);

  jit_options->compile_threshold_ = options.GetOrDefault(RuntimeArgumentMap::JITCompileThreshold);
  if (jit_options->compile_threshold_ > std::numeric_limits<uint16_t>::max()) {
    LOG(FATAL) << "Method compilation threshold is above its internal limit.";
  }

  if (options.Exists(RuntimeArgumentMap::JITWarmupThreshold)) {
    jit_options->warmup_threshold_ = *options.Get(RuntimeArgumentMap::JITWarmupThreshold);
    if (jit_options->warmup_threshold_ > std::numeric_limits<uint16_t>::max()) {
      LOG(FATAL) << "Method warmup threshold is above its internal limit.";
    }
  } else {
    jit_options->warmup_threshold_ = jit_options->compile_threshold_ / 2;
  }

  if (options.Exists(RuntimeArgumentMap::JITOsrThreshold)) {
    jit_options->osr_threshold_ = *options.Get(RuntimeArgumentMap::JITOsrThreshold);
    if (jit_options->osr_threshold_ > std::numeric_limits<uint16_t>::max()) {
      LOG(FATAL) << "Method on stack replacement threshold is above its internal limit.";
    }
  } else {
    jit_options->osr_threshold_ = jit_options->compile_threshold_ * 2;
    if (jit_options->osr_threshold_ > std::numeric_limits<uint16_t>::max()) {
      jit_options->osr_threshold_ = std::numeric_limits<uint16_t>::max();
    }
  }

  if (options.Exists(RuntimeArgumentMap::JITPriorityThreadWeight)) {
    jit_options->priority_thread_weight_ =
        *options.Get(RuntimeArgumentMap::JITPriorityThreadWeight);
    if (jit_options->priority_thread_weight_ > jit_options->warmup_threshold_) {
      LOG(FATAL) << "Priority thread weight is above the warmup threshold.";
    } else if (jit_options->priority_thread_weight_ == 0) {
      LOG(FATAL) << "Priority thread weight cannot be 0.";
    }
  } else {
    jit_options->priority_thread_weight_ = std::max(
        jit_options->warmup_threshold_ / Jit::kDefaultPriorityThreadWeightRatio,
        static_cast<size_t>(1));
  }

  if (options.Exists(RuntimeArgumentMap::JITInvokeTransitionWeight)) {
    jit_options->invoke_transition_weight_ =
        *options.Get(RuntimeArgumentMap::JITInvokeTransitionWeight);
    if (jit_options->invoke_transition_weight_ > jit_options->warmup_threshold_) {
      LOG(FATAL) << "Invoke transition weight is above the warmup threshold.";
    } else if (jit_options->invoke_transition_weight_  == 0) {
      LOG(FATAL) << "Invoke transition weight cannot be 0.";
    }
  } else {
    jit_options->invoke_transition_weight_ = std::max(
        jit_options->warmup_threshold_ / Jit::kDefaultInvokeTransitionWeightRatio,
        static_cast<size_t>(1));;
  }

  return jit_options;
}

bool Jit::ShouldUsePriorityThreadWeight() {
  return Runtime::Current()->InJankPerceptibleProcessState()
      && Thread::Current()->IsJitSensitiveThread();
}

void Jit::DumpInfo(std::ostream& os) {
  code_cache_->Dump(os);
  cumulative_timings_.Dump(os);
  MutexLock mu(Thread::Current(), lock_);
  memory_use_.PrintMemoryUse(os);
}

void Jit::DumpForSigQuit(std::ostream& os) {
  DumpInfo(os);
  ProfileSaver::DumpInstanceInfo(os);
}

void Jit::AddTimingLogger(const TimingLogger& logger) {
  cumulative_timings_.AddLogger(logger);
}

Jit::Jit() : dump_info_on_shutdown_(false),
             cumulative_timings_("JIT timings"),
             memory_use_("Memory used for compilation", 16),
             lock_("JIT memory use lock"),
             use_jit_compilation_(true),
             save_profiling_info_(false),
             hot_method_threshold_(0),
             warm_method_threshold_(0),
             osr_method_threshold_(0),
             priority_thread_weight_(0),
             invoke_transition_weight_(0) {}

Jit* Jit::Create(JitOptions* options, std::string* error_msg) {
  DCHECK(options->UseJitCompilation() || options->GetSaveProfilingInfo());
  std::unique_ptr<Jit> jit(new Jit);
  jit->dump_info_on_shutdown_ = options->DumpJitInfoOnShutdown();
  if (jit_compiler_handle_ == nullptr && !LoadCompiler(error_msg)) {
    return nullptr;
  }
  jit->code_cache_.reset(JitCodeCache::Create(
      options->GetCodeCacheInitialCapacity(),
      options->GetCodeCacheMaxCapacity(),
      jit->generate_debug_info_,
      error_msg));
  if (jit->GetCodeCache() == nullptr) {
    return nullptr;
  }
  jit->use_jit_compilation_ = options->UseJitCompilation();
  jit->save_profiling_info_ = options->GetSaveProfilingInfo();
  VLOG(jit) << "JIT created with initial_capacity="
      << PrettySize(options->GetCodeCacheInitialCapacity())
      << ", max_capacity=" << PrettySize(options->GetCodeCacheMaxCapacity())
      << ", compile_threshold=" << options->GetCompileThreshold()
      << ", save_profiling_info=" << options->GetSaveProfilingInfo();


  jit->hot_method_threshold_ = options->GetCompileThreshold();
  jit->warm_method_threshold_ = options->GetWarmupThreshold();
  jit->osr_method_threshold_ = options->GetOsrThreshold();
  jit->priority_thread_weight_ = options->GetPriorityThreadWeight();
  jit->invoke_transition_weight_ = options->GetInvokeTransitionWeight();

  jit->CreateThreadPool();

  // Notify native debugger about the classes already loaded before the creation of the jit.
  jit->DumpTypeInfoForLoadedTypes(Runtime::Current()->GetClassLinker());
  return jit.release();
}

bool Jit::LoadCompilerLibrary(std::string* error_msg) {
  jit_library_handle_ = dlopen(
      kIsDebugBuild ? "libartd-compiler.so" : "libart-compiler.so", RTLD_NOW);
  if (jit_library_handle_ == nullptr) {
    std::ostringstream oss;
    oss << "JIT could not load libart-compiler.so: " << dlerror();
    *error_msg = oss.str();
    return false;
  }
  jit_load_ = reinterpret_cast<void* (*)(bool*)>(dlsym(jit_library_handle_, "jit_load"));
  if (jit_load_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_load entry point";
    return false;
  }
  jit_unload_ = reinterpret_cast<void (*)(void*)>(
      dlsym(jit_library_handle_, "jit_unload"));
  if (jit_unload_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_unload entry point";
    return false;
  }
  jit_compile_method_ = reinterpret_cast<bool (*)(void*, ArtMethod*, Thread*, bool)>(
      dlsym(jit_library_handle_, "jit_compile_method"));
  if (jit_compile_method_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_compile_method entry point";
    return false;
  }
  jit_types_loaded_ = reinterpret_cast<void (*)(void*, mirror::Class**, size_t)>(
      dlsym(jit_library_handle_, "jit_types_loaded"));
  if (jit_types_loaded_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_types_loaded entry point";
    return false;
  }
  return true;
}

bool Jit::LoadCompiler(std::string* error_msg) {
  if (jit_library_handle_ == nullptr && !LoadCompilerLibrary(error_msg)) {
    return false;
  }
  bool will_generate_debug_symbols = false;
  VLOG(jit) << "Calling JitLoad interpreter_only="
      << Runtime::Current()->GetInstrumentation()->InterpretOnly();
  jit_compiler_handle_ = (jit_load_)(&will_generate_debug_symbols);
  if (jit_compiler_handle_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't load compiler";
    return false;
  }
  generate_debug_info_ = will_generate_debug_symbols;
  return true;
}

bool Jit::CompileMethod(ArtMethod* method, Thread* self, bool osr) {
  DCHECK(Runtime::Current()->UseJitCompilation());
  DCHECK(!method->IsRuntimeMethod());

  // Don't compile the method if it has breakpoints.
  if (Dbg::IsDebuggerActive() && Dbg::MethodHasAnyBreakpoints(method)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to breakpoint";
    return false;
  }

  // Don't compile the method if we are supposed to be deoptimized.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (instrumentation->AreAllMethodsDeoptimized() || instrumentation->IsDeoptimized(method)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to deoptimization";
    return false;
  }

  // If we get a request to compile a proxy method, we pass the actual Java method
  // of that proxy method, as the compiler does not expect a proxy method.
  ArtMethod* method_to_compile = method->GetInterfaceMethodIfProxy(sizeof(void*));
  if (!code_cache_->NotifyCompilationOf(method_to_compile, self, osr)) {
    return false;
  }

  VLOG(jit) << "Compiling method "
            << PrettyMethod(method_to_compile)
            << " osr=" << std::boolalpha << osr;
  bool success = jit_compile_method_(jit_compiler_handle_, method_to_compile, self, osr);
  code_cache_->DoneCompiling(method_to_compile, self, osr);
  if (!success) {
    VLOG(jit) << "Failed to compile method "
              << PrettyMethod(method_to_compile)
              << " osr=" << std::boolalpha << osr;
  }
  return success;
}

void Jit::CreateThreadPool() {
  // There is a DCHECK in the 'AddSamples' method to ensure the tread pool
  // is not null when we instrument.

  // We need peers as we may report the JIT thread, e.g., in the debugger.
  constexpr bool kJitPoolNeedsPeers = true;
  thread_pool_.reset(new ThreadPool("Jit thread pool", 1, kJitPoolNeedsPeers));

  thread_pool_->SetPthreadPriority(kJitPoolThreadPthreadPriority);
  thread_pool_->StartWorkers(Thread::Current());
}

void Jit::DeleteThreadPool() {
  Thread* self = Thread::Current();
  DCHECK(Runtime::Current()->IsShuttingDown(self));
  if (thread_pool_ != nullptr) {
    ThreadPool* cache = nullptr;
    {
      ScopedSuspendAll ssa(__FUNCTION__);
      // Clear thread_pool_ field while the threads are suspended.
      // A mutator in the 'AddSamples' method will check against it.
      cache = thread_pool_.release();
    }
    cache->StopWorkers(self);
    cache->RemoveAllTasks(self);
    // We could just suspend all threads, but we know those threads
    // will finish in a short period, so it's not worth adding a suspend logic
    // here. Besides, this is only done for shutdown.
    cache->Wait(self, false, false);
    delete cache;
  }
}

void Jit::StartProfileSaver(const std::string& filename,
                            const std::vector<std::string>& code_paths,
                            const std::string& foreign_dex_profile_path,
                            const std::string& app_dir) {
  if (save_profiling_info_) {
    ProfileSaver::Start(filename, code_cache_.get(), code_paths, foreign_dex_profile_path, app_dir);
  }
}

void Jit::StopProfileSaver() {
  if (save_profiling_info_ && ProfileSaver::IsStarted()) {
    ProfileSaver::Stop(dump_info_on_shutdown_);
  }
}

bool Jit::JitAtFirstUse() {
  return HotMethodThreshold() == 0;
}

bool Jit::CanInvokeCompiledCode(ArtMethod* method) {
  return code_cache_->ContainsPc(method->GetEntryPointFromQuickCompiledCode());
}

Jit::~Jit() {
  DCHECK(!save_profiling_info_ || !ProfileSaver::IsStarted());
  if (dump_info_on_shutdown_) {
    DumpInfo(LOG(INFO));
  }
  DeleteThreadPool();
  if (jit_compiler_handle_ != nullptr) {
    jit_unload_(jit_compiler_handle_);
    jit_compiler_handle_ = nullptr;
  }
  if (jit_library_handle_ != nullptr) {
    dlclose(jit_library_handle_);
    jit_library_handle_ = nullptr;
  }
}

void Jit::NewTypeLoadedIfUsingJit(mirror::Class* type) {
  if (!Runtime::Current()->UseJitCompilation()) {
    // No need to notify if we only use the JIT to save profiles.
    return;
  }
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit->generate_debug_info_) {
    DCHECK(jit->jit_types_loaded_ != nullptr);
    jit->jit_types_loaded_(jit->jit_compiler_handle_, &type, 1);
  }
}

void Jit::DumpTypeInfoForLoadedTypes(ClassLinker* linker) {
  struct CollectClasses : public ClassVisitor {
    bool operator()(mirror::Class* klass) override {
      classes_.push_back(klass);
      return true;
    }
    std::vector<mirror::Class*> classes_;
  };

  if (generate_debug_info_) {
    ScopedObjectAccess so(Thread::Current());

    CollectClasses visitor;
    linker->VisitClasses(&visitor);
    jit_types_loaded_(jit_compiler_handle_, visitor.classes_.data(), visitor.classes_.size());
  }
}

extern "C" void art_quick_osr_stub(void** stack,
                                   uint32_t stack_size_in_bytes,
                                   const uint8_t* native_pc,
                                   JValue* result,
                                   const char* shorty,
                                   Thread* self);

bool Jit::MaybeDoOnStackReplacement(Thread* thread,
                                    ArtMethod* method,
                                    uint32_t dex_pc,
                                    int32_t dex_pc_offset,
                                    JValue* result) {
  if (!kEnableOnStackReplacement) {
    return false;
  }

  Jit* jit = Runtime::Current()->GetJit();
  if (jit == nullptr) {
    return false;
  }

  if (UNLIKELY(__builtin_frame_address(0) < thread->GetStackEnd())) {
    // Don't attempt to do an OSR if we are close to the stack limit. Since
    // the interpreter frames are still on stack, OSR has the potential
    // to stack overflow even for a simple loop.
    // b/27094810.
    return false;
  }

  // Get the actual Java method if this method is from a proxy class. The compiler
  // and the JIT code cache do not expect methods from proxy classes.
  method = method->GetInterfaceMethodIfProxy(sizeof(void*));

  // Cheap check if the method has been compiled already. That's an indicator that we should
  // osr into it.
  if (!jit->GetCodeCache()->ContainsPc(method->GetEntryPointFromQuickCompiledCode())) {
    return false;
  }

  // Fetch some data before looking up for an OSR method. We don't want thread
  // suspension once we hold an OSR method, as the JIT code cache could delete the OSR
  // method while we are being suspended.
  const size_t number_of_vregs = method->GetCodeItem()->registers_size_;
  const char* shorty = method->GetShorty();
  std::string method_name(VLOG_IS_ON(jit) ? PrettyMethod(method) : "");
  void** memory = nullptr;
  size_t frame_size = 0;
  ShadowFrame* shadow_frame = nullptr;
  const uint8_t* native_pc = nullptr;

  {
    ScopedAssertNoThreadSuspension sts(thread, "Holding OSR method");
    const OatQuickMethodHeader* osr_method = jit->GetCodeCache()->LookupOsrMethodHeader(method);
    if (osr_method == nullptr) {
      // No osr method yet, just return to the interpreter.
      return false;
    }

    CodeInfo code_info = osr_method->GetOptimizedCodeInfo();
    CodeInfoEncoding encoding = code_info.ExtractEncoding();

    // Find stack map starting at the target dex_pc.
    StackMap stack_map = code_info.GetOsrStackMapForDexPc(dex_pc + dex_pc_offset, encoding);
    if (!stack_map.IsValid()) {
      // There is no OSR stack map for this dex pc offset. Just return to the interpreter in the
      // hope that the next branch has one.
      return false;
    }

    // Before allowing the jump, make sure the debugger is not active to avoid jumping from
    // interpreter to OSR while e.g. single stepping. Note that we could selectively disable
    // OSR when single stepping, but that's currently hard to know at this point.
    if (Dbg::IsDebuggerActive()) {
      return false;
    }

    // We found a stack map, now fill the frame with dex register values from the interpreter's
    // shadow frame.
    DexRegisterMap vreg_map =
        code_info.GetDexRegisterMapOf(stack_map, encoding, number_of_vregs);

    frame_size = osr_method->GetFrameSizeInBytes();

    // Allocate memory to put shadow frame values. The osr stub will copy that memory to
    // stack.
    // Note that we could pass the shadow frame to the stub, and let it copy the values there,
    // but that is engineering complexity not worth the effort for something like OSR.
    memory = reinterpret_cast<void**>(malloc(frame_size));
    CHECK(memory != nullptr);
    memset(memory, 0, frame_size);

    // Art ABI: ArtMethod is at the bottom of the stack.
    memory[0] = method;

    shadow_frame = thread->PopShadowFrame();
    if (!vreg_map.IsValid()) {
      // If we don't have a dex register map, then there are no live dex registers at
      // this dex pc.
    } else {
      for (uint16_t vreg = 0; vreg < number_of_vregs; ++vreg) {
        DexRegisterLocation::Kind location =
            vreg_map.GetLocationKind(vreg, number_of_vregs, code_info, encoding);
        if (location == DexRegisterLocation::Kind::kNone) {
          // Dex register is dead or uninitialized.
          continue;
        }

        if (location == DexRegisterLocation::Kind::kConstant) {
          // We skip constants because the compiled code knows how to handle them.
          continue;
        }

        DCHECK_EQ(location, DexRegisterLocation::Kind::kInStack);

        int32_t vreg_value = shadow_frame->GetVReg(vreg);
        int32_t slot_offset = vreg_map.GetStackOffsetInBytes(vreg,
                                                             number_of_vregs,
                                                             code_info,
                                                             encoding);
        DCHECK_LT(slot_offset, static_cast<int32_t>(frame_size));
        DCHECK_GT(slot_offset, 0);
        (reinterpret_cast<int32_t*>(memory))[slot_offset / sizeof(int32_t)] = vreg_value;
      }
    }

    native_pc = stack_map.GetNativePcOffset(encoding.stack_map_encoding) +
        osr_method->GetEntryPoint();
    VLOG(jit) << "Jumping to "
              << method_name
              << "@"
              << std::hex << reinterpret_cast<uintptr_t>(native_pc);
  }

  {
    ManagedStack fragment;
    thread->PushManagedStackFragment(&fragment);
    (*art_quick_osr_stub)(memory,
                          frame_size,
                          native_pc,
                          result,
                          shorty,
                          thread);

    if (UNLIKELY(thread->GetException() == Thread::GetDeoptimizationException())) {
      thread->DeoptimizeWithDeoptimizationException(result);
    }
    thread->PopManagedStackFragment(fragment);
  }
  free(memory);
  thread->PushShadowFrame(shadow_frame);
  VLOG(jit) << "Done running OSR code for " << method_name;
  return true;
}

void Jit::AddMemoryUsage(ArtMethod* method, size_t bytes) {
  if (bytes > 4 * MB) {
    LOG(INFO) << "Compiler allocated "
              << PrettySize(bytes)
              << " to compile "
              << PrettyMethod(method);
  }
  MutexLock mu(Thread::Current(), lock_);
  memory_use_.AddValue(bytes);
}

class JitCompileTask FINAL : public Task {
 public:
  enum TaskKind {
    kAllocateProfile,
    kCompile,
    kCompileOsr
  };

  JitCompileTask(ArtMethod* method, TaskKind kind) : method_(method), kind_(kind) {
    ScopedObjectAccess soa(Thread::Current());
    // Add a global ref to the class to prevent class unloading until compilation is done.
    klass_ = soa.Vm()->AddGlobalRef(soa.Self(), method_->GetDeclaringClass());
    CHECK(klass_ != nullptr);
  }

  ~JitCompileTask() {
    ScopedObjectAccess soa(Thread::Current());
    soa.Vm()->DeleteGlobalRef(soa.Self(), klass_);
  }

  void Run(Thread* self) OVERRIDE {
    ScopedObjectAccess soa(self);
    if (kind_ == kCompile) {
      Runtime::Current()->GetJit()->CompileMethod(method_, self, /* osr */ false);
    } else if (kind_ == kCompileOsr) {
      Runtime::Current()->GetJit()->CompileMethod(method_, self, /* osr */ true);
    } else {
      DCHECK(kind_ == kAllocateProfile);
      if (ProfilingInfo::Create(self, method_, /* retry_allocation */ true)) {
        VLOG(jit) << "Start profiling " << PrettyMethod(method_);
      }
    }
    ProfileSaver::NotifyJitActivity();
  }

  void Finalize() OVERRIDE {
    delete this;
  }

 private:
  ArtMethod* const method_;
  const TaskKind kind_;
  jobject klass_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(JitCompileTask);
};

void Jit::AddSamples(Thread* self, ArtMethod* method, uint16_t count, bool with_backedges) {
  if (thread_pool_ == nullptr) {
    // Should only see this when shutting down.
    DCHECK(Runtime::Current()->IsShuttingDown(self));
    return;
  }

  if (method->IsClassInitializer() || method->IsNative() || !method->IsCompilable()) {
    // We do not want to compile such methods.
    return;
  }
  DCHECK(thread_pool_ != nullptr);
  DCHECK_GT(warm_method_threshold_, 0);
  DCHECK_GT(hot_method_threshold_, warm_method_threshold_);
  DCHECK_GT(osr_method_threshold_, hot_method_threshold_);
  DCHECK_GE(priority_thread_weight_, 1);
  DCHECK_LE(priority_thread_weight_, hot_method_threshold_);

  int32_t starting_count = method->GetCounter();
  if (Jit::ShouldUsePriorityThreadWeight()) {
    count *= priority_thread_weight_;
  }
  int32_t new_count = starting_count + count;   // int32 here to avoid wrap-around;
  if (starting_count < warm_method_threshold_) {
    if ((new_count >= warm_method_threshold_) &&
        (method->GetProfilingInfo(sizeof(void*)) == nullptr)) {
      bool success = ProfilingInfo::Create(self, method, /* retry_allocation */ false);
      if (success) {
        VLOG(jit) << "Start profiling " << PrettyMethod(method);
      }

      if (thread_pool_ == nullptr) {
        // Calling ProfilingInfo::Create might put us in a suspended state, which could
        // lead to the thread pool being deleted when we are shutting down.
        DCHECK(Runtime::Current()->IsShuttingDown(self));
        return;
      }

      if (!success) {
        // We failed allocating. Instead of doing the collection on the Java thread, we push
        // an allocation to a compiler thread, that will do the collection.
        thread_pool_->AddTask(self, new JitCompileTask(method, JitCompileTask::kAllocateProfile));
      }
    }
    // Avoid jumping more than one state at a time.
    new_count = std::min(new_count, hot_method_threshold_ - 1);
  } else if (use_jit_compilation_) {
    if (starting_count < hot_method_threshold_) {
      if ((new_count >= hot_method_threshold_) &&
          !code_cache_->ContainsPc(method->GetEntryPointFromQuickCompiledCode())) {
        DCHECK(thread_pool_ != nullptr);
        thread_pool_->AddTask(self, new JitCompileTask(method, JitCompileTask::kCompile));
      }
      // Avoid jumping more than one state at a time.
      new_count = std::min(new_count, osr_method_threshold_ - 1);
    } else if (starting_count < osr_method_threshold_) {
      if (!with_backedges) {
        // If the samples don't contain any back edge, we don't increment the hotness.
        return;
      }
      if ((new_count >= osr_method_threshold_) &&  !code_cache_->IsOsrCompiled(method)) {
        DCHECK(thread_pool_ != nullptr);
        thread_pool_->AddTask(self, new JitCompileTask(method, JitCompileTask::kCompileOsr));
      }
    }
  }
  // Update hotness counter
  method->SetCounter(new_count);
}

void Jit::MethodEntered(Thread* thread, ArtMethod* method) {
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(runtime->UseJitCompilation() && runtime->GetJit()->JitAtFirstUse())) {
    // The compiler requires a ProfilingInfo object.
    ProfilingInfo::Create(thread, method, /* retry_allocation */ true);
    JitCompileTask compile_task(method, JitCompileTask::kCompile);
    compile_task.Run(thread);
    return;
  }

  ProfilingInfo* profiling_info = method->GetProfilingInfo(sizeof(void*));
  // Update the entrypoint if the ProfilingInfo has one. The interpreter will call it
  // instead of interpreting the method.
  if ((profiling_info != nullptr) && (profiling_info->GetSavedEntryPoint() != nullptr)) {
    Runtime::Current()->GetInstrumentation()->UpdateMethodsCode(
        method, profiling_info->GetSavedEntryPoint());
  } else {
    AddSamples(thread, method, 1, /* with_backedges */false);
  }
}

void Jit::InvokeVirtualOrInterface(Thread* thread,
                                   mirror::Object* this_object,
                                   ArtMethod* caller,
                                   uint32_t dex_pc,
                                   ArtMethod* callee ATTRIBUTE_UNUSED) {
  ScopedAssertNoThreadSuspension ants(thread, __FUNCTION__);
  DCHECK(this_object != nullptr);
  ProfilingInfo* info = caller->GetProfilingInfo(sizeof(void*));
  if (info != nullptr) {
    info->AddInvokeInfo(dex_pc, this_object->GetClass());
  }
}

void Jit::WaitForCompilationToFinish(Thread* self) {
  if (thread_pool_ != nullptr) {
    thread_pool_->Wait(self, false, false);
  }
}

}  // namespace jit
}  // namespace art
