#
# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

include art/build/Android.common_build.mk

LIBART_COMMON_SRC_FILES := \
  art_field.cc \
  art_method.cc \
  atomic.cc.arm \
  barrier.cc \
  base/allocator.cc \
  base/arena_allocator.cc \
  base/arena_bit_vector.cc \
  base/bit_vector.cc \
  base/file_magic.cc \
  base/hex_dump.cc \
  base/logging.cc \
  base/mutex.cc \
  base/scoped_arena_allocator.cc \
  base/scoped_flock.cc \
  base/stringpiece.cc \
  base/stringprintf.cc \
  base/time_utils.cc \
  base/timing_logger.cc \
  base/unix_file/fd_file.cc \
  base/unix_file/random_access_file_utils.cc \
  check_jni.cc \
  class_linker.cc \
  class_table.cc \
  code_simulator_container.cc \
  common_throws.cc \
  compiler_filter.cc \
  debugger.cc \
  dex_file.cc \
  dex_file_verifier.cc \
  dex_instruction.cc \
  elf_file.cc \
  fault_handler.cc \
  gc/allocation_record.cc \
  gc/allocator/dlmalloc.cc \
  gc/allocator/rosalloc.cc \
  gc/accounting/bitmap.cc \
  gc/accounting/card_table.cc \
  gc/accounting/heap_bitmap.cc \
  gc/accounting/mod_union_table.cc \
  gc/accounting/remembered_set.cc \
  gc/accounting/space_bitmap.cc \
  gc/collector/concurrent_copying.cc \
  gc/collector/garbage_collector.cc \
  gc/collector/immune_region.cc \
  gc/collector/immune_spaces.cc \
  gc/collector/mark_compact.cc \
  gc/collector/mark_sweep.cc \
  gc/collector/partial_mark_sweep.cc \
  gc/collector/semi_space.cc \
  gc/collector/sticky_mark_sweep.cc \
  gc/gc_cause.cc \
  gc/heap.cc \
  gc/reference_processor.cc \
  gc/reference_queue.cc \
  gc/scoped_gc_critical_section.cc \
  gc/space/bump_pointer_space.cc \
  gc/space/dlmalloc_space.cc \
  gc/space/image_space.cc \
  gc/space/large_object_space.cc \
  gc/space/malloc_space.cc \
  gc/space/region_space.cc \
  gc/space/rosalloc_space.cc \
  gc/space/space.cc \
  gc/space/zygote_space.cc \
  gc/task_processor.cc \
  hprof/hprof.cc \
  image.cc \
  indirect_reference_table.cc \
  instrumentation.cc \
  intern_table.cc \
  interpreter/interpreter.cc \
  interpreter/interpreter_common.cc \
  interpreter/interpreter_goto_table_impl.cc \
  interpreter/interpreter_switch_impl.cc \
  interpreter/unstarted_runtime.cc \
  java_vm_ext.cc \
  jdwp/jdwp_event.cc \
  jdwp/jdwp_expand_buf.cc \
  jdwp/jdwp_handler.cc \
  jdwp/jdwp_main.cc \
  jdwp/jdwp_request.cc \
  jdwp/jdwp_socket.cc \
  jdwp/object_registry.cc \
  jni_env_ext.cc \
  jit/debugger_interface.cc \
  jit/jit.cc \
  jit/jit_code_cache.cc \
  jit/offline_profiling_info.cc \
  jit/profiling_info.cc \
  jit/profile_saver.cc  \
  lambda/art_lambda_method.cc \
  lambda/box_table.cc \
  lambda/closure.cc \
  lambda/closure_builder.cc \
  lambda/leaking_allocator.cc \
  jni_internal.cc \
  jobject_comparator.cc \
  linear_alloc.cc \
  mem_map.cc \
  memory_region.cc \
  mirror/abstract_method.cc \
  mirror/array.cc \
  mirror/class.cc \
  mirror/dex_cache.cc \
  mirror/field.cc \
  mirror/method.cc \
  mirror/object.cc \
  mirror/reference.cc \
  mirror/stack_trace_element.cc \
  mirror/string.cc \
  mirror/throwable.cc \
  monitor.cc \
  native_bridge_art_interface.cc \
  native/dalvik_system_DexFile.cc \
  native/dalvik_system_VMDebug.cc \
  native/dalvik_system_VMRuntime.cc \
  native/dalvik_system_VMStack.cc \
  native/dalvik_system_ZygoteHooks.cc \
  native/java_lang_Class.cc \
  native/java_lang_DexCache.cc \
  native/java_lang_Object.cc \
  native/java_lang_String.cc \
  native/java_lang_StringFactory.cc \
  native/java_lang_System.cc \
  native/java_lang_Thread.cc \
  native/java_lang_Throwable.cc \
  native/java_lang_VMClassLoader.cc \
  native/java_lang_ref_FinalizerReference.cc \
  native/java_lang_ref_Reference.cc \
  native/java_lang_reflect_AbstractMethod.cc \
  native/java_lang_reflect_Array.cc \
  native/java_lang_reflect_Constructor.cc \
  native/java_lang_reflect_Field.cc \
  native/java_lang_reflect_Method.cc \
  native/java_lang_reflect_Proxy.cc \
  native/java_util_concurrent_atomic_AtomicLong.cc \
  native/libcore_util_CharsetUtils.cc \
  native/org_apache_harmony_dalvik_ddmc_DdmServer.cc \
  native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc \
  native/sun_misc_Unsafe.cc \
  oat.cc \
  oat_file.cc \
  oat_file_assistant.cc \
  oat_file_manager.cc \
  oat_quick_method_header.cc \
  object_lock.cc \
  offsets.cc \
  os_linux.cc \
  parsed_options.cc \
  primitive.cc \
  profiler.cc \
  quick_exception_handler.cc \
  quick/inline_method_analyser.cc \
  reference_table.cc \
  reflection.cc \
  runtime.cc \
  runtime_options.cc \
  signal_catcher.cc \
  stack.cc \
  stack_map.cc \
  thread.cc \
  thread_list.cc \
  thread_pool.cc \
  trace.cc \
  transaction.cc \
  type_lookup_table.cc \
  utf.cc \
  utils.cc \
  verifier/instruction_flags.cc \
  verifier/method_verifier.cc \
  verifier/reg_type.cc \
  verifier/reg_type_cache.cc \
  verifier/register_line.cc \
  well_known_classes.cc \
  zip_archive.cc \
  unpacker/unpacker.cc \
  unpacker/cJSON.cc
#patch by Youlor
#++++++++++++++++++++++++++++
  # zip_archive.cc \
  # unpacker/unpacker.cc \
  # unpacker/cJSON.cc
#++++++++++++++++++++++++++++

LIBART_COMMON_SRC_FILES += \
  arch/context.cc \
  arch/instruction_set.cc \
  arch/instruction_set_features.cc \
  arch/memcmp16.cc \
  arch/arm/instruction_set_features_arm.cc \
  arch/arm/registers_arm.cc \
  arch/arm64/instruction_set_features_arm64.cc \
  arch/arm64/registers_arm64.cc \
  arch/mips/instruction_set_features_mips.cc \
  arch/mips/registers_mips.cc \
  arch/mips64/instruction_set_features_mips64.cc \
  arch/mips64/registers_mips64.cc \
  arch/x86/instruction_set_features_x86.cc \
  arch/x86/registers_x86.cc \
  arch/x86_64/registers_x86_64.cc \
  entrypoints/entrypoint_utils.cc \
  entrypoints/jni/jni_entrypoints.cc \
  entrypoints/math_entrypoints.cc \
  entrypoints/quick/quick_alloc_entrypoints.cc \
  entrypoints/quick/quick_cast_entrypoints.cc \
  entrypoints/quick/quick_deoptimization_entrypoints.cc \
  entrypoints/quick/quick_dexcache_entrypoints.cc \
  entrypoints/quick/quick_field_entrypoints.cc \
  entrypoints/quick/quick_fillarray_entrypoints.cc \
  entrypoints/quick/quick_instrumentation_entrypoints.cc \
  entrypoints/quick/quick_jni_entrypoints.cc \
  entrypoints/quick/quick_lock_entrypoints.cc \
  entrypoints/quick/quick_math_entrypoints.cc \
  entrypoints/quick/quick_thread_entrypoints.cc \
  entrypoints/quick/quick_throw_entrypoints.cc \
  entrypoints/quick/quick_trampoline_entrypoints.cc

LIBART_TARGET_LDFLAGS :=
LIBART_HOST_LDFLAGS :=

# Keep the __jit_debug_register_code symbol as a unique symbol during ICF for architectures where
# we use gold as the linker (arm, x86, x86_64). The symbol is used by the debuggers to detect when
# new jit code is generated. We don't want it to be called when a different function with the same
# (empty) body is called.
JIT_DEBUG_REGISTER_CODE_LDFLAGS := -Wl,--keep-unique,__jit_debug_register_code
LIBART_TARGET_LDFLAGS_arm    := $(JIT_DEBUG_REGISTER_CODE_LDFLAGS)
LIBART_TARGET_LDFLAGS_arm64  := $(JIT_DEBUG_REGISTER_CODE_LDFLAGS)
LIBART_TARGET_LDFLAGS_x86    := $(JIT_DEBUG_REGISTER_CODE_LDFLAGS)
LIBART_TARGET_LDFLAGS_x86_64 := $(JIT_DEBUG_REGISTER_CODE_LDFLAGS)
JIT_DEBUG_REGISTER_CODE_LDFLAGS :=

LIBART_TARGET_SRC_FILES := \
  $(LIBART_COMMON_SRC_FILES) \
  jdwp/jdwp_adb.cc \
  monitor_android.cc \
  runtime_android.cc \
  thread_android.cc

LIBART_TARGET_SRC_FILES_arm := \
  interpreter/mterp/mterp.cc \
  interpreter/mterp/out/mterp_arm.S \
  arch/arm/context_arm.cc.arm \
  arch/arm/entrypoints_init_arm.cc \
  arch/arm/instruction_set_features_assembly_tests.S \
  arch/arm/jni_entrypoints_arm.S \
  arch/arm/memcmp16_arm.S \
  arch/arm/quick_entrypoints_arm.S \
  arch/arm/quick_entrypoints_cc_arm.cc \
  arch/arm/thread_arm.cc \
  arch/arm/fault_handler_arm.cc

LIBART_TARGET_SRC_FILES_arm64 := \
  interpreter/mterp/mterp.cc \
  interpreter/mterp/out/mterp_arm64.S \
  arch/arm64/context_arm64.cc \
  arch/arm64/entrypoints_init_arm64.cc \
  arch/arm64/jni_entrypoints_arm64.S \
  arch/arm64/memcmp16_arm64.S \
  arch/arm64/quick_entrypoints_arm64.S \
  arch/arm64/thread_arm64.cc \
  monitor_pool.cc \
  arch/arm64/fault_handler_arm64.cc

LIBART_SRC_FILES_x86 := \
  interpreter/mterp/mterp.cc \
  interpreter/mterp/out/mterp_x86.S \
  arch/x86/context_x86.cc \
  arch/x86/entrypoints_init_x86.cc \
  arch/x86/jni_entrypoints_x86.S \
  arch/x86/memcmp16_x86.S \
  arch/x86/quick_entrypoints_x86.S \
  arch/x86/thread_x86.cc \
  arch/x86/fault_handler_x86.cc

LIBART_TARGET_SRC_FILES_x86 := \
  $(LIBART_SRC_FILES_x86)

# Note that the fault_handler_x86.cc is not a mistake.  This file is
# shared between the x86 and x86_64 architectures.
LIBART_SRC_FILES_x86_64 := \
  interpreter/mterp/mterp.cc \
  interpreter/mterp/out/mterp_x86_64.S \
  arch/x86_64/context_x86_64.cc \
  arch/x86_64/entrypoints_init_x86_64.cc \
  arch/x86_64/jni_entrypoints_x86_64.S \
  arch/x86_64/memcmp16_x86_64.S \
  arch/x86_64/quick_entrypoints_x86_64.S \
  arch/x86_64/thread_x86_64.cc \
  monitor_pool.cc \
  arch/x86/fault_handler_x86.cc

LIBART_TARGET_SRC_FILES_x86_64 := \
  $(LIBART_SRC_FILES_x86_64) \

LIBART_TARGET_SRC_FILES_mips := \
  interpreter/mterp/mterp.cc \
  interpreter/mterp/out/mterp_mips.S \
  arch/mips/context_mips.cc \
  arch/mips/entrypoints_init_mips.cc \
  arch/mips/jni_entrypoints_mips.S \
  arch/mips/memcmp16_mips.S \
  arch/mips/quick_entrypoints_mips.S \
  arch/mips/thread_mips.cc \
  arch/mips/fault_handler_mips.cc

LIBART_TARGET_SRC_FILES_mips64 := \
  interpreter/mterp/mterp.cc \
  interpreter/mterp/out/mterp_mips64.S \
  arch/mips64/context_mips64.cc \
  arch/mips64/entrypoints_init_mips64.cc \
  arch/mips64/jni_entrypoints_mips64.S \
  arch/mips64/memcmp16_mips64.S \
  arch/mips64/quick_entrypoints_mips64.S \
  arch/mips64/thread_mips64.cc \
  monitor_pool.cc \
  arch/mips64/fault_handler_mips64.cc

LIBART_HOST_SRC_FILES := \
  $(LIBART_COMMON_SRC_FILES) \
  monitor_linux.cc \
  runtime_linux.cc \
  thread_linux.cc

LIBART_HOST_SRC_FILES_32 := \
  $(LIBART_SRC_FILES_x86)

LIBART_HOST_SRC_FILES_64 := \
  $(LIBART_SRC_FILES_x86_64)

LIBART_ENUM_OPERATOR_OUT_HEADER_FILES := \
  arch/instruction_set.h \
  base/allocator.h \
  base/mutex.h \
  debugger.h \
  base/unix_file/fd_file.h \
  dex_file.h \
  dex_instruction.h \
  dex_instruction_utils.h \
  gc_root.h \
  gc/allocator_type.h \
  gc/allocator/rosalloc.h \
  gc/collector_type.h \
  gc/collector/gc_type.h \
  gc/heap.h \
  gc/space/region_space.h \
  gc/space/space.h \
  gc/weak_root_state.h \
  image.h \
  instrumentation.h \
  indirect_reference_table.h \
  invoke_type.h \
  jdwp/jdwp.h \
  jdwp/jdwp_constants.h \
  lock_word.h \
  mirror/class.h \
  oat.h \
  object_callbacks.h \
  process_state.h \
  profiler_options.h \
  quick/inline_method_analyser.h \
  runtime.h \
  stack.h \
  thread.h \
  thread_state.h \
  verifier/method_verifier.h

LIBOPENJDKJVM_SRC_FILES := openjdkjvm/OpenjdkJvm.cc

LIBART_CFLAGS := -DBUILDING_LIBART=1

LIBART_TARGET_CFLAGS :=
LIBART_HOST_CFLAGS :=

# Default dex2oat instruction set features.
LIBART_HOST_DEFAULT_INSTRUCTION_SET_FEATURES := default
LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES := default
2ND_LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES := default
ifeq ($(DEX2OAT_TARGET_ARCH),arm)
  ifneq (,$(filter $(DEX2OAT_TARGET_CPU_VARIANT),cortex-a15 krait denver))
    LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES := atomic_ldrd_strd,div
  else
    ifneq (,$(filter $(DEX2OAT_TARGET_CPU_VARIANT),cortex-a7))
      LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES := div
    endif
  endif
endif
ifeq ($(2ND_DEX2OAT_TARGET_ARCH),arm)
  ifneq (,$(filter $(DEX2OAT_TARGET_CPU_VARIANT),cortex-a15 krait denver))
    2ND_LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES := atomic_ldrd_strd,div
  else
    ifneq (,$(filter $(DEX2OAT_TARGET_CPU_VARIANT),cortex-a7))
      2ND_LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES := div
    endif
  endif
endif

# $(1): target or host
# $(2): ndebug or debug
# $(3): static or shared (note that static only applies for host)
# $(4): module name : either libart or libopenjdkjvm
define build-runtime-library
  ifneq ($(1),target)
    ifneq ($(1),host)
      $$(error expected target or host for argument 1, received $(1))
    endif
  endif
  ifneq ($(2),ndebug)
    ifneq ($(2),debug)
      $$(error expected ndebug or debug for argument 2, received $(2))
    endif
  endif
  ifneq ($(4),libart)
    ifneq ($(4),libopenjdkjvm)
      $$(error expected libart of libopenjdkjvm for argument 4, received $(4))
    endif
  endif

  art_target_or_host := $(1)
  art_ndebug_or_debug := $(2)
  art_static_or_shared := $(3)

  include $$(CLEAR_VARS)
  LOCAL_CPP_EXTENSION := $$(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := $(4)
    ifeq ($$(art_target_or_host),target)
      LOCAL_FDO_SUPPORT := true
    endif
  else # debug
    LOCAL_MODULE := $(4)d
  endif

  LOCAL_MODULE_TAGS := optional

  ifeq ($$(art_static_or_shared),static)
    LOCAL_MODULE_CLASS := STATIC_LIBRARIES
  else
    LOCAL_MODULE_CLASS := SHARED_LIBRARIES
  endif

  ifeq ($(4),libart)
    ifeq ($$(art_target_or_host),target)
      LOCAL_SRC_FILES := $$(LIBART_TARGET_SRC_FILES)
      $$(foreach arch,$$(ART_TARGET_SUPPORTED_ARCH), \
        $$(eval LOCAL_SRC_FILES_$$(arch) := $$$$(LIBART_TARGET_SRC_FILES_$$(arch))))
    else # host
      LOCAL_SRC_FILES := $$(LIBART_HOST_SRC_FILES)
      LOCAL_SRC_FILES_32 := $$(LIBART_HOST_SRC_FILES_32)
      LOCAL_SRC_FILES_64 := $$(LIBART_HOST_SRC_FILES_64)
      LOCAL_IS_HOST_MODULE := true
    endif
  else # libopenjdkjvm
    LOCAL_SRC_FILES := $$(LIBOPENJDKJVM_SRC_FILES)
    ifeq ($$(art_target_or_host),host)
      LOCAL_IS_HOST_MODULE := true
    endif
  endif

ifeq ($(4),libart)
  GENERATED_SRC_DIR := $$(call local-generated-sources-dir)
  ENUM_OPERATOR_OUT_CC_FILES := $$(patsubst %.h,%_operator_out.cc,$$(LIBART_ENUM_OPERATOR_OUT_HEADER_FILES))
  ENUM_OPERATOR_OUT_GEN := $$(addprefix $$(GENERATED_SRC_DIR)/,$$(ENUM_OPERATOR_OUT_CC_FILES))

$$(ENUM_OPERATOR_OUT_GEN): art/tools/generate-operator-out.py
$$(ENUM_OPERATOR_OUT_GEN): PRIVATE_CUSTOM_TOOL = art/tools/generate-operator-out.py $(LOCAL_PATH) $$< > $$@
$$(ENUM_OPERATOR_OUT_GEN): $$(GENERATED_SRC_DIR)/%_operator_out.cc : $(LOCAL_PATH)/%.h
	$$(transform-generated-source)

  LOCAL_GENERATED_SOURCES += $$(ENUM_OPERATOR_OUT_GEN)
endif

  LOCAL_CFLAGS := $$(LIBART_CFLAGS)
  LOCAL_LDFLAGS := $$(LIBART_LDFLAGS)
  ifeq ($$(art_target_or_host),target)
    LOCAL_CFLAGS += $$(LIBART_TARGET_CFLAGS)
    LOCAL_LDFLAGS += $$(LIBART_TARGET_LDFLAGS)
    $$(foreach arch,$$(ART_TARGET_SUPPORTED_ARCH), \
      $$(eval LOCAL_LDFLAGS_$$(arch) := $$(LIBART_TARGET_LDFLAGS_$$(arch))))
  else #host
    LOCAL_CFLAGS += $$(LIBART_HOST_CFLAGS)
    LOCAL_LDFLAGS += $$(LIBART_HOST_LDFLAGS)
    ifeq ($$(art_static_or_shared),static)
      LOCAL_LDFLAGS += -static
    endif
  endif

  # Clang usage
  ifeq ($$(art_target_or_host),target)
    $$(eval $$(call set-target-local-clang-vars))
    $$(eval $$(call set-target-local-cflags-vars,$(2)))
    LOCAL_CLANG_ASFLAGS_arm += -no-integrated-as
    LOCAL_CFLAGS_$(DEX2OAT_TARGET_ARCH) += -DART_DEFAULT_INSTRUCTION_SET_FEATURES="$(LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES)"
    LOCAL_CFLAGS_$(2ND_DEX2OAT_TARGET_ARCH) += -DART_DEFAULT_INSTRUCTION_SET_FEATURES="$(2ND_LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES)"
  else # host
    LOCAL_CLANG := $$(ART_HOST_CLANG)
    LOCAL_LDLIBS := $$(ART_HOST_LDLIBS)
    LOCAL_LDLIBS += -ldl -lpthread
    ifeq ($$(HOST_OS),linux)
      LOCAL_LDLIBS += -lrt
    endif
    LOCAL_CFLAGS += $$(ART_HOST_CFLAGS)
    LOCAL_CFLAGS += -DART_DEFAULT_INSTRUCTION_SET_FEATURES="$(LIBART_HOST_DEFAULT_INSTRUCTION_SET_FEATURES)"
    LOCAL_ASFLAGS += $$(ART_HOST_ASFLAGS)

    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $$(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $$(ART_HOST_NON_DEBUG_CFLAGS)
    endif
    LOCAL_MULTILIB := both
  endif

  LOCAL_C_INCLUDES += $$(ART_C_INCLUDES)
  LOCAL_C_INCLUDES += art/cmdline
  LOCAL_C_INCLUDES += art/sigchainlib
  LOCAL_C_INCLUDES += art

  ifeq ($$(art_static_or_shared),static)
    LOCAL_STATIC_LIBRARIES := libnativehelper
    LOCAL_STATIC_LIBRARIES += libnativebridge
    LOCAL_STATIC_LIBRARIES += libnativeloader
    LOCAL_STATIC_LIBRARIES += libsigchain_dummy
    LOCAL_STATIC_LIBRARIES += libbacktrace
    LOCAL_STATIC_LIBRARIES += liblz4
  else
    LOCAL_SHARED_LIBRARIES := libnativehelper
    LOCAL_SHARED_LIBRARIES += libnativebridge
    LOCAL_SHARED_LIBRARIES += libnativeloader
    LOCAL_SHARED_LIBRARIES += libsigchain
    LOCAL_SHARED_LIBRARIES += libbacktrace
    LOCAL_SHARED_LIBRARIES += liblz4
  endif

  ifeq ($$(art_target_or_host),target)
    LOCAL_SHARED_LIBRARIES += libdl
    # ZipArchive support, the order matters here to get all symbols.
    LOCAL_STATIC_LIBRARIES := libziparchive libz libbase
    # For android::FileMap used by libziparchive.
    LOCAL_SHARED_LIBRARIES += libutils
    # For liblog, atrace, properties, ashmem, set_sched_policy and socket_peer_is_trusted.
    LOCAL_SHARED_LIBRARIES += libcutils
  else # host
    ifeq ($$(art_static_or_shared),static)
      LOCAL_STATIC_LIBRARIES += libziparchive-host libz
      # For ashmem_create_region.
      LOCAL_STATIC_LIBRARIES += libcutils
    else
      LOCAL_SHARED_LIBRARIES += libziparchive-host libz-host
      # For ashmem_create_region.
      LOCAL_SHARED_LIBRARIES += libcutils
    endif
  endif

  ifeq ($(4),libopenjdkjvm)
    ifeq ($$(art_ndebug_or_debug),ndebug)
      LOCAL_SHARED_LIBRARIES += libart
    else
      LOCAL_SHARED_LIBRARIES += libartd
    endif
    LOCAL_NOTICE_FILE := $(LOCAL_PATH)/openjdkjvm/NOTICE
  endif
  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $$(LOCAL_PATH)/Android.mk

  ifeq ($$(art_target_or_host),target)
    LOCAL_MODULE_TARGET_ARCH := $$(ART_TARGET_SUPPORTED_ARCH)
  endif

  LOCAL_NATIVE_COVERAGE := $(ART_COVERAGE)

  ifeq ($$(art_target_or_host),target)
    ifneq ($$(art_ndebug_or_debug),debug)
      # Leave the symbols in the shared library so that stack unwinders can
      # produce meaningful name resolution.
      LOCAL_STRIP_MODULE := keep_symbols
    endif
    include $$(BUILD_SHARED_LIBRARY)
  else # host
    ifeq ($$(art_static_or_shared),static)
      include $$(BUILD_HOST_STATIC_LIBRARY)
    else
      include $$(BUILD_HOST_SHARED_LIBRARY)
    endif
  endif

  # Clear locally defined variables.
  GENERATED_SRC_DIR :=
  ENUM_OPERATOR_OUT_CC_FILES :=
  ENUM_OPERATOR_OUT_GEN :=
  art_target_or_host :=
  art_ndebug_or_debug :=
  art_static_or_shared :=
endef

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since
# they are used to cross compile for the target.
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-runtime-library,host,ndebug,shared,libart))
  $(eval $(call build-runtime-library,host,ndebug,shared,libopenjdkjvm))
  ifeq ($(ART_BUILD_HOST_STATIC),true)
    $(eval $(call build-runtime-library,host,ndebug,static,libart))
    $(eval $(call build-runtime-library,host,ndebug,static,libopenjdkjvm))
  endif
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-runtime-library,host,debug,shared,libart))
  $(eval $(call build-runtime-library,host,debug,shared,libopenjdkjvm))
  ifeq ($(ART_BUILD_HOST_STATIC),true)
    $(eval $(call build-runtime-library,host,debug,static,libart))
    $(eval $(call build-runtime-library,host,debug,static,libopenjdkjvm))
  endif
endif

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
#  $(error $(call build-runtime-library,target,ndebug))
  $(eval $(call build-runtime-library,target,ndebug,shared,libart))
  $(eval $(call build-runtime-library,target,ndebug,shared,libopenjdkjvm))
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-runtime-library,target,debug,shared,libart))
  $(eval $(call build-runtime-library,target,debug,shared,libopenjdkjvm))
endif

# Clear locally defined variables.
LOCAL_PATH :=
LIBART_COMMON_SRC_FILES :=
LIBART_HOST_DEFAULT_INSTRUCTION_SET_FEATURES :=
LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES :=
2ND_LIBART_TARGET_DEFAULT_INSTRUCTION_SET_FEATURES :=
LIBART_HOST_LDFLAGS :=
LIBART_TARGET_LDFLAGS :=
LIBART_TARGET_LDFLAGS_arm :=
LIBART_TARGET_LDFLAGS_arm64 :=
LIBART_TARGET_LDFLAGS_x86 :=
LIBART_TARGET_LDFLAGS_x86_64 :=
LIBART_TARGET_LDFLAGS_mips :=
LIBART_TARGET_LDFLAGS_mips64 :=
LIBART_TARGET_SRC_FILES :=
LIBART_TARGET_SRC_FILES_arm :=
LIBART_TARGET_SRC_FILES_arm64 :=
LIBART_TARGET_SRC_FILES_x86 :=
LIBART_TARGET_SRC_FILES_x86_64 :=
LIBART_TARGET_SRC_FILES_mips :=
LIBART_TARGET_SRC_FILES_mips64 :=
LIBART_HOST_SRC_FILES :=
LIBART_HOST_SRC_FILES_32 :=
LIBART_HOST_SRC_FILES_64 :=
LIBART_ENUM_OPERATOR_OUT_HEADER_FILES :=
LIBART_CFLAGS :=
LIBART_TARGET_CFLAGS :=
LIBART_HOST_CFLAGS :=
build-runtime-library :=
