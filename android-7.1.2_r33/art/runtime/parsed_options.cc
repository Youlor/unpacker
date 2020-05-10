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

#include "parsed_options.h"

#include <sstream>

#include "base/stringpiece.h"
#include "debugger.h"
#include "gc/heap.h"
#include "monitor.h"
#include "runtime.h"
#include "trace.h"
#include "utils.h"

#include "cmdline_parser.h"
#include "runtime_options.h"

namespace art {

using MemoryKiB = Memory<1024>;

ParsedOptions::ParsedOptions()
  : hook_is_sensitive_thread_(nullptr),
    hook_vfprintf_(vfprintf),
    hook_exit_(exit),
    hook_abort_(nullptr) {                          // We don't call abort(3) by default; see
                                                    // Runtime::Abort
}

bool ParsedOptions::Parse(const RuntimeOptions& options,
                          bool ignore_unrecognized,
                          RuntimeArgumentMap* runtime_options) {
  CHECK(runtime_options != nullptr);

  ParsedOptions parser;
  return parser.DoParse(options, ignore_unrecognized, runtime_options);
}

using RuntimeParser = CmdlineParser<RuntimeArgumentMap, RuntimeArgumentMap::Key>;

// Yes, the stack frame is huge. But we get called super early on (and just once)
// to pass the command line arguments, so we'll probably be ok.
// Ideas to avoid suppressing this diagnostic are welcome!
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-larger-than="

std::unique_ptr<RuntimeParser> ParsedOptions::MakeParser(bool ignore_unrecognized) {
  using M = RuntimeArgumentMap;

  std::unique_ptr<RuntimeParser::Builder> parser_builder =
      std::unique_ptr<RuntimeParser::Builder>(new RuntimeParser::Builder());

  parser_builder->
       Define("-Xzygote")
          .IntoKey(M::Zygote)
      .Define("-help")
          .IntoKey(M::Help)
      .Define("-showversion")
          .IntoKey(M::ShowVersion)
      .Define("-Xbootclasspath:_")
          .WithType<std::string>()
          .IntoKey(M::BootClassPath)
      .Define("-Xbootclasspath-locations:_")
          .WithType<ParseStringList<':'>>()  // std::vector<std::string>, split by :
          .IntoKey(M::BootClassPathLocations)
      .Define({"-classpath _", "-cp _"})
          .WithType<std::string>()
          .IntoKey(M::ClassPath)
      .Define("-Ximage:_")
          .WithType<std::string>()
          .IntoKey(M::Image)
      .Define("-Xcheck:jni")
          .IntoKey(M::CheckJni)
      .Define("-Xjniopts:forcecopy")
          .IntoKey(M::JniOptsForceCopy)
      .Define({"-Xrunjdwp:_", "-agentlib:jdwp=_"})
          .WithType<JDWP::JdwpOptions>()
          .IntoKey(M::JdwpOptions)
      .Define("-Xms_")
          .WithType<MemoryKiB>()
          .IntoKey(M::MemoryInitialSize)
      .Define("-Xmx_")
          .WithType<MemoryKiB>()
          .IntoKey(M::MemoryMaximumSize)
      .Define("-XX:HeapGrowthLimit=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::HeapGrowthLimit)
      .Define("-XX:HeapMinFree=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::HeapMinFree)
      .Define("-XX:HeapMaxFree=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::HeapMaxFree)
      .Define("-XX:NonMovingSpaceCapacity=_")
          .WithType<MemoryKiB>()
          .IntoKey(M::NonMovingSpaceCapacity)
      .Define("-XX:HeapTargetUtilization=_")
          .WithType<double>().WithRange(0.1, 0.9)
          .IntoKey(M::HeapTargetUtilization)
      .Define("-XX:ForegroundHeapGrowthMultiplier=_")
          .WithType<double>().WithRange(0.1, 1.0)
          .IntoKey(M::ForegroundHeapGrowthMultiplier)
      .Define("-XX:ParallelGCThreads=_")
          .WithType<unsigned int>()
          .IntoKey(M::ParallelGCThreads)
      .Define("-XX:ConcGCThreads=_")
          .WithType<unsigned int>()
          .IntoKey(M::ConcGCThreads)
      .Define("-Xss_")
          .WithType<Memory<1>>()
          .IntoKey(M::StackSize)
      .Define("-XX:MaxSpinsBeforeThinLockInflation=_")
          .WithType<unsigned int>()
          .IntoKey(M::MaxSpinsBeforeThinLockInflation)
      .Define("-XX:LongPauseLogThreshold=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::LongPauseLogThreshold)
      .Define("-XX:LongGCLogThreshold=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::LongGCLogThreshold)
      .Define("-XX:DumpGCPerformanceOnShutdown")
          .IntoKey(M::DumpGCPerformanceOnShutdown)
      .Define("-XX:DumpJITInfoOnShutdown")
          .IntoKey(M::DumpJITInfoOnShutdown)
      .Define("-XX:IgnoreMaxFootprint")
          .IntoKey(M::IgnoreMaxFootprint)
      .Define("-XX:LowMemoryMode")
          .IntoKey(M::LowMemoryMode)
      .Define("-XX:UseTLAB")
          .WithValue(true)
          .IntoKey(M::UseTLAB)
      .Define({"-XX:EnableHSpaceCompactForOOM", "-XX:DisableHSpaceCompactForOOM"})
          .WithValues({true, false})
          .IntoKey(M::EnableHSpaceCompactForOOM)
      .Define("-XX:DumpNativeStackOnSigQuit:_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::DumpNativeStackOnSigQuit)
      .Define("-Xusejit:_")
          .WithType<bool>()
          .WithValueMap({{"false", false}, {"true", true}})
          .IntoKey(M::UseJitCompilation)
      .Define("-Xjitinitialsize:_")
          .WithType<MemoryKiB>()
          .IntoKey(M::JITCodeCacheInitialCapacity)
      .Define("-Xjitmaxsize:_")
          .WithType<MemoryKiB>()
          .IntoKey(M::JITCodeCacheMaxCapacity)
      .Define("-Xjitthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITCompileThreshold)
      .Define("-Xjitwarmupthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITWarmupThreshold)
      .Define("-Xjitosrthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITOsrThreshold)
      .Define("-Xjitprithreadweight:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITPriorityThreadWeight)
      .Define("-Xjittransitionweight:_")
          .WithType<unsigned int>()
          .IntoKey(M::JITInvokeTransitionWeight)
      .Define("-Xjitsaveprofilinginfo")
          .WithValue(true)
          .IntoKey(M::JITSaveProfilingInfo)
      .Define("-XX:HspaceCompactForOOMMinIntervalMs=_")  // in ms
          .WithType<MillisecondsToNanoseconds>()  // store as ns
          .IntoKey(M::HSpaceCompactForOOMMinIntervalsMs)
      .Define("-D_")
          .WithType<std::vector<std::string>>().AppendValues()
          .IntoKey(M::PropertiesList)
      .Define("-Xjnitrace:_")
          .WithType<std::string>()
          .IntoKey(M::JniTrace)
      .Define("-Xpatchoat:_")
          .WithType<std::string>()
          .IntoKey(M::PatchOat)
      .Define({"-Xrelocate", "-Xnorelocate"})
          .WithValues({true, false})
          .IntoKey(M::Relocate)
      .Define({"-Xdex2oat", "-Xnodex2oat"})
          .WithValues({true, false})
          .IntoKey(M::Dex2Oat)
      .Define({"-Ximage-dex2oat", "-Xnoimage-dex2oat"})
          .WithValues({true, false})
          .IntoKey(M::ImageDex2Oat)
      .Define("-Xint")
          .WithValue(true)
          .IntoKey(M::Interpret)
      .Define("-Xgc:_")
          .WithType<XGcOption>()
          .IntoKey(M::GcOption)
      .Define("-XX:LargeObjectSpace=_")
          .WithType<gc::space::LargeObjectSpaceType>()
          .WithValueMap({{"disabled", gc::space::LargeObjectSpaceType::kDisabled},
                         {"freelist", gc::space::LargeObjectSpaceType::kFreeList},
                         {"map",      gc::space::LargeObjectSpaceType::kMap}})
          .IntoKey(M::LargeObjectSpace)
      .Define("-XX:LargeObjectThreshold=_")
          .WithType<Memory<1>>()
          .IntoKey(M::LargeObjectThreshold)
      .Define("-XX:BackgroundGC=_")
          .WithType<BackgroundGcOption>()
          .IntoKey(M::BackgroundGc)
      .Define("-XX:+DisableExplicitGC")
          .IntoKey(M::DisableExplicitGC)
      .Define("-verbose:_")
          .WithType<LogVerbosity>()
          .IntoKey(M::Verbose)
      .Define("-Xlockprofthreshold:_")
          .WithType<unsigned int>()
          .IntoKey(M::LockProfThreshold)
      .Define("-Xstacktracefile:_")
          .WithType<std::string>()
          .IntoKey(M::StackTraceFile)
      .Define("-Xmethod-trace")
          .IntoKey(M::MethodTrace)
      .Define("-Xmethod-trace-file:_")
          .WithType<std::string>()
          .IntoKey(M::MethodTraceFile)
      .Define("-Xmethod-trace-file-size:_")
          .WithType<unsigned int>()
          .IntoKey(M::MethodTraceFileSize)
      .Define("-Xmethod-trace-stream")
          .IntoKey(M::MethodTraceStreaming)
      .Define("-Xprofile:_")
          .WithType<TraceClockSource>()
          .WithValueMap({{"threadcpuclock", TraceClockSource::kThreadCpu},
                         {"wallclock",      TraceClockSource::kWall},
                         {"dualclock",      TraceClockSource::kDual}})
          .IntoKey(M::ProfileClock)
      .Define("-Xenable-profiler")
          .WithType<TestProfilerOptions>()
          .AppendValues()
          .IntoKey(M::ProfilerOpts)  // NOTE: Appends into same key as -Xprofile-*
      .Define("-Xprofile-_")  // -Xprofile-<key>:<value>
          .WithType<TestProfilerOptions>()
          .AppendValues()
          .IntoKey(M::ProfilerOpts)  // NOTE: Appends into same key as -Xenable-profiler
      .Define("-Xcompiler:_")
          .WithType<std::string>()
          .IntoKey(M::Compiler)
      .Define("-Xcompiler-option _")
          .WithType<std::vector<std::string>>()
          .AppendValues()
          .IntoKey(M::CompilerOptions)
      .Define("-Ximage-compiler-option _")
          .WithType<std::vector<std::string>>()
          .AppendValues()
          .IntoKey(M::ImageCompilerOptions)
      .Define("-Xverify:_")
          .WithType<verifier::VerifyMode>()
          .WithValueMap({{"none",     verifier::VerifyMode::kNone},
                         {"remote",   verifier::VerifyMode::kEnable},
                         {"all",      verifier::VerifyMode::kEnable},
                         {"softfail", verifier::VerifyMode::kSoftFail}})
          .IntoKey(M::Verify)
      .Define("-XX:NativeBridge=_")
          .WithType<std::string>()
          .IntoKey(M::NativeBridge)
      .Define("-Xzygote-max-boot-retry=_")
          .WithType<unsigned int>()
          .IntoKey(M::ZygoteMaxFailedBoots)
      .Define("-Xno-dex-file-fallback")
          .IntoKey(M::NoDexFileFallback)
      .Define("-Xno-sig-chain")
          .IntoKey(M::NoSigChain)
      .Define("--cpu-abilist=_")
          .WithType<std::string>()
          .IntoKey(M::CpuAbiList)
      .Define("-Xfingerprint:_")
          .WithType<std::string>()
          .IntoKey(M::Fingerprint)
      .Define("-Xexperimental:_")
          .WithType<ExperimentalFlags>()
          .AppendValues()
          .IntoKey(M::Experimental)
      .Define("-Xforce-nb-testing")
          .IntoKey(M::ForceNativeBridge)
      .Define("-XOatFileManagerCompilerFilter:_")
          .WithType<std::string>()
          .IntoKey(M::OatFileManagerCompilerFilter)
      .Ignore({
          "-ea", "-da", "-enableassertions", "-disableassertions", "--runtime-arg", "-esa",
          "-dsa", "-enablesystemassertions", "-disablesystemassertions", "-Xrs", "-Xint:_",
          "-Xdexopt:_", "-Xnoquithandler", "-Xjnigreflimit:_", "-Xgenregmap", "-Xnogenregmap",
          "-Xverifyopt:_", "-Xcheckdexsum", "-Xincludeselectedop", "-Xjitop:_",
          "-Xincludeselectedmethod", "-Xjitthreshold:_",
          "-Xjitblocking", "-Xjitmethod:_", "-Xjitclass:_", "-Xjitoffset:_",
          "-Xjitconfig:_", "-Xjitcheckcg", "-Xjitverbose", "-Xjitprofile",
          "-Xjitdisableopt", "-Xjitsuspendpoll", "-XX:mainThreadStackSize=_"})
      .IgnoreUnrecognized(ignore_unrecognized);

  // TODO: Move Usage information into this DSL.

  return std::unique_ptr<RuntimeParser>(new RuntimeParser(parser_builder->Build()));
}

#pragma GCC diagnostic pop

// Remove all the special options that have something in the void* part of the option.
// If runtime_options is not null, put the options in there.
// As a side-effect, populate the hooks from options.
bool ParsedOptions::ProcessSpecialOptions(const RuntimeOptions& options,
                                          RuntimeArgumentMap* runtime_options,
                                          std::vector<std::string>* out_options) {
  using M = RuntimeArgumentMap;

  // TODO: Move the below loop into JNI
  // Handle special options that set up hooks
  for (size_t i = 0; i < options.size(); ++i) {
    const std::string option(options[i].first);
      // TODO: support -Djava.class.path
    if (option == "bootclasspath") {
      auto boot_class_path = static_cast<std::vector<std::unique_ptr<const DexFile>>*>(
          const_cast<void*>(options[i].second));

      if (runtime_options != nullptr) {
        runtime_options->Set(M::BootClassPathDexList, boot_class_path);
      }
    } else if (option == "compilercallbacks") {
      CompilerCallbacks* compiler_callbacks =
          reinterpret_cast<CompilerCallbacks*>(const_cast<void*>(options[i].second));
      if (runtime_options != nullptr) {
        runtime_options->Set(M::CompilerCallbacksPtr, compiler_callbacks);
      }
    } else if (option == "imageinstructionset") {
      const char* isa_str = reinterpret_cast<const char*>(options[i].second);
      auto&& image_isa = GetInstructionSetFromString(isa_str);
      if (image_isa == kNone) {
        Usage("%s is not a valid instruction set.", isa_str);
        return false;
      }
      if (runtime_options != nullptr) {
        runtime_options->Set(M::ImageInstructionSet, image_isa);
      }
    } else if (option == "sensitiveThread") {
      const void* hook = options[i].second;
      bool (*hook_is_sensitive_thread)() = reinterpret_cast<bool (*)()>(const_cast<void*>(hook));

      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookIsSensitiveThread, hook_is_sensitive_thread);
      }
    } else if (option == "vfprintf") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("vfprintf argument was nullptr");
        return false;
      }
      int (*hook_vfprintf)(FILE *, const char*, va_list) =
          reinterpret_cast<int (*)(FILE *, const char*, va_list)>(const_cast<void*>(hook));

      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookVfprintf, hook_vfprintf);
      }
      hook_vfprintf_ = hook_vfprintf;
    } else if (option == "exit") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("exit argument was nullptr");
        return false;
      }
      void(*hook_exit)(jint) = reinterpret_cast<void(*)(jint)>(const_cast<void*>(hook));
      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookExit, hook_exit);
      }
      hook_exit_ = hook_exit;
    } else if (option == "abort") {
      const void* hook = options[i].second;
      if (hook == nullptr) {
        Usage("abort was nullptr\n");
        return false;
      }
      void(*hook_abort)() = reinterpret_cast<void(*)()>(const_cast<void*>(hook));
      if (runtime_options != nullptr) {
        runtime_options->Set(M::HookAbort, hook_abort);
      }
      hook_abort_ = hook_abort;
    } else {
      // It is a regular option, that doesn't have a known 'second' value.
      // Push it on to the regular options which will be parsed by our parser.
      if (out_options != nullptr) {
        out_options->push_back(option);
      }
    }
  }

  return true;
}

// Intended for local changes only.
static void MaybeOverrideVerbosity() {
  //  gLogVerbosity.class_linker = true;  // TODO: don't check this in!
  //  gLogVerbosity.collector = true;  // TODO: don't check this in!
  //  gLogVerbosity.compiler = true;  // TODO: don't check this in!
  //  gLogVerbosity.deopt = true;  // TODO: don't check this in!
  //  gLogVerbosity.gc = true;  // TODO: don't check this in!
  //  gLogVerbosity.heap = true;  // TODO: don't check this in!
  //  gLogVerbosity.jdwp = true;  // TODO: don't check this in!
  //  gLogVerbosity.jit = true;  // TODO: don't check this in!
  //  gLogVerbosity.jni = true;  // TODO: don't check this in!
  //  gLogVerbosity.monitor = true;  // TODO: don't check this in!
  //  gLogVerbosity.oat = true;  // TODO: don't check this in!
  //  gLogVerbosity.profiler = true;  // TODO: don't check this in!
  //  gLogVerbosity.signals = true;  // TODO: don't check this in!
  //  gLogVerbosity.simulator = true; // TODO: don't check this in!
  //  gLogVerbosity.startup = true;  // TODO: don't check this in!
  //  gLogVerbosity.third_party_jni = true;  // TODO: don't check this in!
  //  gLogVerbosity.threads = true;  // TODO: don't check this in!
  //  gLogVerbosity.verifier = true;  // TODO: don't check this in!
}

bool ParsedOptions::DoParse(const RuntimeOptions& options,
                            bool ignore_unrecognized,
                            RuntimeArgumentMap* runtime_options) {
  for (size_t i = 0; i < options.size(); ++i) {
    if (true && options[0].first == "-Xzygote") {
      LOG(INFO) << "option[" << i << "]=" << options[i].first;
    }
  }

  auto parser = MakeParser(ignore_unrecognized);

  // Convert to a simple string list (without the magic pointer options)
  std::vector<std::string> argv_list;
  if (!ProcessSpecialOptions(options, nullptr, &argv_list)) {
    return false;
  }

  CmdlineResult parse_result = parser->Parse(argv_list);

  // Handle parse errors by displaying the usage and potentially exiting.
  if (parse_result.IsError()) {
    if (parse_result.GetStatus() == CmdlineResult::kUsage) {
      UsageMessage(stdout, "%s\n", parse_result.GetMessage().c_str());
      Exit(0);
    } else if (parse_result.GetStatus() == CmdlineResult::kUnknown && !ignore_unrecognized) {
      Usage("%s\n", parse_result.GetMessage().c_str());
      return false;
    } else {
      Usage("%s\n", parse_result.GetMessage().c_str());
      Exit(0);
    }

    UNREACHABLE();
  }

  using M = RuntimeArgumentMap;
  RuntimeArgumentMap args = parser->ReleaseArgumentsMap();

  // -help, -showversion, etc.
  if (args.Exists(M::Help)) {
    Usage(nullptr);
    return false;
  } else if (args.Exists(M::ShowVersion)) {
    UsageMessage(stdout, "ART version %s\n", Runtime::GetVersion());
    Exit(0);
  } else if (args.Exists(M::BootClassPath)) {
    LOG(INFO) << "setting boot class path to " << *args.Get(M::BootClassPath);
  }

  if (args.GetOrDefault(M::UseJitCompilation) && args.GetOrDefault(M::Interpret)) {
    Usage("-Xusejit:true and -Xint cannot be specified together");
    Exit(0);
  }

  // Set a default boot class path if we didn't get an explicit one via command line.
  if (getenv("BOOTCLASSPATH") != nullptr) {
    args.SetIfMissing(M::BootClassPath, std::string(getenv("BOOTCLASSPATH")));
  }

  // Set a default class path if we didn't get an explicit one via command line.
  if (getenv("CLASSPATH") != nullptr) {
    args.SetIfMissing(M::ClassPath, std::string(getenv("CLASSPATH")));
  }

  // Default to number of processors minus one since the main GC thread also does work.
  args.SetIfMissing(M::ParallelGCThreads, gc::Heap::kDefaultEnableParallelGC ?
      static_cast<unsigned int>(sysconf(_SC_NPROCESSORS_CONF) - 1u) : 0u);

  // -Xverbose:
  {
    LogVerbosity *log_verbosity = args.Get(M::Verbose);
    if (log_verbosity != nullptr) {
      gLogVerbosity = *log_verbosity;
    }
  }

  MaybeOverrideVerbosity();

  // -Xprofile:
  Trace::SetDefaultClockSource(args.GetOrDefault(M::ProfileClock));

  if (!ProcessSpecialOptions(options, &args, nullptr)) {
      return false;
  }

  {
    // If not set, background collector type defaults to homogeneous compaction.
    // If foreground is GSS, use GSS as background collector.
    // If not low memory mode, semispace otherwise.

    gc::CollectorType background_collector_type_;
    gc::CollectorType collector_type_ = (XGcOption{}).collector_type_;  // NOLINT [whitespace/braces] [5]
    bool low_memory_mode_ = args.Exists(M::LowMemoryMode);

    background_collector_type_ = args.GetOrDefault(M::BackgroundGc);
    {
      XGcOption* xgc = args.Get(M::GcOption);
      if (xgc != nullptr && xgc->collector_type_ != gc::kCollectorTypeNone) {
        collector_type_ = xgc->collector_type_;
      }
    }

    if (background_collector_type_ == gc::kCollectorTypeNone) {
      if (collector_type_ != gc::kCollectorTypeGSS) {
        background_collector_type_ = low_memory_mode_ ?
            gc::kCollectorTypeSS : gc::kCollectorTypeHomogeneousSpaceCompact;
      } else {
        background_collector_type_ = collector_type_;
      }
    }

    args.Set(M::BackgroundGc, BackgroundGcOption { background_collector_type_ });
  }

  // If a reference to the dalvik core.jar snuck in, replace it with
  // the art specific version. This can happen with on device
  // boot.art/boot.oat generation by GenerateImage which relies on the
  // value of BOOTCLASSPATH.
#if defined(ART_TARGET)
  std::string core_jar("/core.jar");
  std::string core_libart_jar("/core-libart.jar");
#else
  // The host uses hostdex files.
  std::string core_jar("/core-hostdex.jar");
  std::string core_libart_jar("/core-libart-hostdex.jar");
#endif
  auto boot_class_path_string = args.GetOrDefault(M::BootClassPath);

  size_t core_jar_pos = boot_class_path_string.find(core_jar);
  if (core_jar_pos != std::string::npos) {
    boot_class_path_string.replace(core_jar_pos, core_jar.size(), core_libart_jar);
    args.Set(M::BootClassPath, boot_class_path_string);
  }

  {
    auto&& boot_class_path = args.GetOrDefault(M::BootClassPath);
    auto&& boot_class_path_locations = args.GetOrDefault(M::BootClassPathLocations);
    if (args.Exists(M::BootClassPathLocations)) {
      size_t boot_class_path_count = ParseStringList<':'>::Split(boot_class_path).Size();

      if (boot_class_path_count != boot_class_path_locations.Size()) {
        Usage("The number of boot class path files does not match"
            " the number of boot class path locations given\n"
            "  boot class path files     (%zu): %s\n"
            "  boot class path locations (%zu): %s\n",
            boot_class_path.size(), boot_class_path_string.c_str(),
            boot_class_path_locations.Size(), boot_class_path_locations.Join().c_str());
        return false;
      }
    }
  }

  if (!args.Exists(M::CompilerCallbacksPtr) && !args.Exists(M::Image)) {
    std::string image = GetAndroidRoot();
    image += "/framework/boot.art";
    args.Set(M::Image, image);
  }

  // 0 means no growth limit, and growth limit should be always <= heap size
  if (args.GetOrDefault(M::HeapGrowthLimit) <= 0u ||
      args.GetOrDefault(M::HeapGrowthLimit) > args.GetOrDefault(M::MemoryMaximumSize)) {
    args.Set(M::HeapGrowthLimit, args.GetOrDefault(M::MemoryMaximumSize));
  }

  if (args.GetOrDefault(M::Experimental) & ExperimentalFlags::kLambdas) {
    LOG(WARNING) << "Experimental lambdas have been enabled. All lambda opcodes have "
                 << "an unstable specification and are nearly guaranteed to change over time. "
                 << "Do not attempt to write shipping code against these opcodes.";
  }

  *runtime_options = std::move(args);
  return true;
}

void ParsedOptions::Exit(int status) {
  hook_exit_(status);
}

void ParsedOptions::Abort() {
  hook_abort_();
}

void ParsedOptions::UsageMessageV(FILE* stream, const char* fmt, va_list ap) {
  hook_vfprintf_(stream, fmt, ap);
}

void ParsedOptions::UsageMessage(FILE* stream, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageMessageV(stream, fmt, ap);
  va_end(ap);
}

void ParsedOptions::Usage(const char* fmt, ...) {
  bool error = (fmt != nullptr);
  FILE* stream = error ? stderr : stdout;

  if (fmt != nullptr) {
    va_list ap;
    va_start(ap, fmt);
    UsageMessageV(stream, fmt, ap);
    va_end(ap);
  }

  const char* program = "dalvikvm";
  UsageMessage(stream, "%s: [options] class [argument ...]\n", program);
  UsageMessage(stream, "\n");
  UsageMessage(stream, "The following standard options are supported:\n");
  UsageMessage(stream, "  -classpath classpath (-cp classpath)\n");
  UsageMessage(stream, "  -Dproperty=value\n");
  UsageMessage(stream, "  -verbose:tag ('gc', 'jit', 'jni', or 'class')\n");
  UsageMessage(stream, "  -showversion\n");
  UsageMessage(stream, "  -help\n");
  UsageMessage(stream, "  -agentlib:jdwp=options\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following extended options are supported:\n");
  UsageMessage(stream, "  -Xrunjdwp:<options>\n");
  UsageMessage(stream, "  -Xbootclasspath:bootclasspath\n");
  UsageMessage(stream, "  -Xcheck:tag  (e.g. 'jni')\n");
  UsageMessage(stream, "  -XmsN (min heap, must be multiple of 1K, >= 1MB)\n");
  UsageMessage(stream, "  -XmxN (max heap, must be multiple of 1K, >= 2MB)\n");
  UsageMessage(stream, "  -XssN (stack size)\n");
  UsageMessage(stream, "  -Xint\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following Dalvik options are supported:\n");
  UsageMessage(stream, "  -Xzygote\n");
  UsageMessage(stream, "  -Xjnitrace:substring (eg NativeClass or nativeMethod)\n");
  UsageMessage(stream, "  -Xstacktracefile:<filename>\n");
  UsageMessage(stream, "  -Xgc:[no]preverify\n");
  UsageMessage(stream, "  -Xgc:[no]postverify\n");
  UsageMessage(stream, "  -XX:HeapGrowthLimit=N\n");
  UsageMessage(stream, "  -XX:HeapMinFree=N\n");
  UsageMessage(stream, "  -XX:HeapMaxFree=N\n");
  UsageMessage(stream, "  -XX:NonMovingSpaceCapacity=N\n");
  UsageMessage(stream, "  -XX:HeapTargetUtilization=doublevalue\n");
  UsageMessage(stream, "  -XX:ForegroundHeapGrowthMultiplier=doublevalue\n");
  UsageMessage(stream, "  -XX:LowMemoryMode\n");
  UsageMessage(stream, "  -Xprofile:{threadcpuclock,wallclock,dualclock}\n");
  UsageMessage(stream, "  -Xjitthreshold:integervalue\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following unique to ART options are supported:\n");
  UsageMessage(stream, "  -Xgc:[no]preverify_rosalloc\n");
  UsageMessage(stream, "  -Xgc:[no]postsweepingverify_rosalloc\n");
  UsageMessage(stream, "  -Xgc:[no]postverify_rosalloc\n");
  UsageMessage(stream, "  -Xgc:[no]presweepingverify\n");
  UsageMessage(stream, "  -Ximage:filename\n");
  UsageMessage(stream, "  -Xbootclasspath-locations:bootclasspath\n"
                       "     (override the dex locations of the -Xbootclasspath files)\n");
  UsageMessage(stream, "  -XX:+DisableExplicitGC\n");
  UsageMessage(stream, "  -XX:ParallelGCThreads=integervalue\n");
  UsageMessage(stream, "  -XX:ConcGCThreads=integervalue\n");
  UsageMessage(stream, "  -XX:MaxSpinsBeforeThinLockInflation=integervalue\n");
  UsageMessage(stream, "  -XX:LongPauseLogThreshold=integervalue\n");
  UsageMessage(stream, "  -XX:LongGCLogThreshold=integervalue\n");
  UsageMessage(stream, "  -XX:DumpGCPerformanceOnShutdown\n");
  UsageMessage(stream, "  -XX:DumpJITInfoOnShutdown\n");
  UsageMessage(stream, "  -XX:IgnoreMaxFootprint\n");
  UsageMessage(stream, "  -XX:UseTLAB\n");
  UsageMessage(stream, "  -XX:BackgroundGC=none\n");
  UsageMessage(stream, "  -XX:LargeObjectSpace={disabled,map,freelist}\n");
  UsageMessage(stream, "  -XX:LargeObjectThreshold=N\n");
  UsageMessage(stream, "  -XX:DumpNativeStackOnSigQuit=booleanvalue\n");
  UsageMessage(stream, "  -Xmethod-trace\n");
  UsageMessage(stream, "  -Xmethod-trace-file:filename");
  UsageMessage(stream, "  -Xmethod-trace-file-size:integervalue\n");
  UsageMessage(stream, "  -Xenable-profiler\n");
  UsageMessage(stream, "  -Xprofile-filename:filename\n");
  UsageMessage(stream, "  -Xprofile-period:integervalue\n");
  UsageMessage(stream, "  -Xprofile-duration:integervalue\n");
  UsageMessage(stream, "  -Xprofile-interval:integervalue\n");
  UsageMessage(stream, "  -Xprofile-backoff:doublevalue\n");
  UsageMessage(stream, "  -Xprofile-start-immediately\n");
  UsageMessage(stream, "  -Xprofile-top-k-threshold:doublevalue\n");
  UsageMessage(stream, "  -Xprofile-top-k-change-threshold:doublevalue\n");
  UsageMessage(stream, "  -Xprofile-type:{method,stack}\n");
  UsageMessage(stream, "  -Xprofile-max-stack-depth:integervalue\n");
  UsageMessage(stream, "  -Xcompiler:filename\n");
  UsageMessage(stream, "  -Xcompiler-option dex2oat-option\n");
  UsageMessage(stream, "  -Ximage-compiler-option dex2oat-option\n");
  UsageMessage(stream, "  -Xpatchoat:filename\n");
  UsageMessage(stream, "  -Xusejit:booleanvalue\n");
  UsageMessage(stream, "  -Xjitinitialsize:N\n");
  UsageMessage(stream, "  -Xjitmaxsize:N\n");
  UsageMessage(stream, "  -Xjitwarmupthreshold:integervalue\n");
  UsageMessage(stream, "  -Xjitosrthreshold:integervalue\n");
  UsageMessage(stream, "  -Xjitprithreadweight:integervalue\n");
  UsageMessage(stream, "  -X[no]relocate\n");
  UsageMessage(stream, "  -X[no]dex2oat (Whether to invoke dex2oat on the application)\n");
  UsageMessage(stream, "  -X[no]image-dex2oat (Whether to create and use a boot image)\n");
  UsageMessage(stream, "  -Xno-dex-file-fallback "
                       "(Don't fall back to dex files without oat files)\n");
  UsageMessage(stream, "  -Xexperimental:lambdas "
                       "(Enable new and experimental dalvik opcodes and semantics)\n");
  UsageMessage(stream, "\n");

  UsageMessage(stream, "The following previously supported Dalvik options are ignored:\n");
  UsageMessage(stream, "  -ea[:<package name>... |:<class name>]\n");
  UsageMessage(stream, "  -da[:<package name>... |:<class name>]\n");
  UsageMessage(stream, "   (-enableassertions, -disableassertions)\n");
  UsageMessage(stream, "  -esa\n");
  UsageMessage(stream, "  -dsa\n");
  UsageMessage(stream, "   (-enablesystemassertions, -disablesystemassertions)\n");
  UsageMessage(stream, "  -Xverify:{none,remote,all,softfail}\n");
  UsageMessage(stream, "  -Xrs\n");
  UsageMessage(stream, "  -Xint:portable, -Xint:fast, -Xint:jit\n");
  UsageMessage(stream, "  -Xdexopt:{none,verified,all,full}\n");
  UsageMessage(stream, "  -Xnoquithandler\n");
  UsageMessage(stream, "  -Xjniopts:{warnonly,forcecopy}\n");
  UsageMessage(stream, "  -Xjnigreflimit:integervalue\n");
  UsageMessage(stream, "  -Xgc:[no]precise\n");
  UsageMessage(stream, "  -Xgc:[no]verifycardtable\n");
  UsageMessage(stream, "  -X[no]genregmap\n");
  UsageMessage(stream, "  -Xverifyopt:[no]checkmon\n");
  UsageMessage(stream, "  -Xcheckdexsum\n");
  UsageMessage(stream, "  -Xincludeselectedop\n");
  UsageMessage(stream, "  -Xjitop:hexopvalue[-endvalue][,hexopvalue[-endvalue]]*\n");
  UsageMessage(stream, "  -Xincludeselectedmethod\n");
  UsageMessage(stream, "  -Xjitblocking\n");
  UsageMessage(stream, "  -Xjitmethod:signature[,signature]* (eg Ljava/lang/String\\;replace)\n");
  UsageMessage(stream, "  -Xjitclass:classname[,classname]*\n");
  UsageMessage(stream, "  -Xjitcodecachesize:N\n");
  UsageMessage(stream, "  -Xjitoffset:offset[,offset]\n");
  UsageMessage(stream, "  -Xjitconfig:filename\n");
  UsageMessage(stream, "  -Xjitcheckcg\n");
  UsageMessage(stream, "  -Xjitverbose\n");
  UsageMessage(stream, "  -Xjitprofile\n");
  UsageMessage(stream, "  -Xjitdisableopt\n");
  UsageMessage(stream, "  -Xjitsuspendpoll\n");
  UsageMessage(stream, "  -XX:mainThreadStackSize=N\n");
  UsageMessage(stream, "\n");

  Exit((error) ? 1 : 0);
}

}  // namespace art
