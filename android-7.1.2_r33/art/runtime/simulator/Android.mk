#
# Copyright (C) 2015 The Android Open Source Project
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

LIBART_SIMULATOR_SRC_FILES := \
  code_simulator.cc \
  code_simulator_arm64.cc

# $(1): target or host
# $(2): ndebug or debug
define build-libart-simulator
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

  art_target_or_host := $(1)
  art_ndebug_or_debug := $(2)

  include $(CLEAR_VARS)
  ifeq ($$(art_target_or_host),host)
     LOCAL_IS_HOST_MODULE := true
  endif
  LOCAL_CPP_EXTENSION := $(ART_CPP_EXTENSION)
  ifeq ($$(art_ndebug_or_debug),ndebug)
    LOCAL_MODULE := libart-simulator
  else # debug
    LOCAL_MODULE := libartd-simulator
  endif

  LOCAL_MODULE_TAGS := optional
  LOCAL_MODULE_CLASS := SHARED_LIBRARIES

  LOCAL_SRC_FILES := $$(LIBART_SIMULATOR_SRC_FILES)

  ifeq ($$(art_target_or_host),target)
    $(call set-target-local-clang-vars)
    $(call set-target-local-cflags-vars,$(2))
  else # host
    LOCAL_CLANG := $(ART_HOST_CLANG)
    LOCAL_LDLIBS := $(ART_HOST_LDLIBS)
    LOCAL_CFLAGS += $(ART_HOST_CFLAGS)
    LOCAL_ASFLAGS += $(ART_HOST_ASFLAGS)
    ifeq ($$(art_ndebug_or_debug),debug)
      LOCAL_CFLAGS += $(ART_HOST_DEBUG_CFLAGS)
    else
      LOCAL_CFLAGS += $(ART_HOST_NON_DEBUG_CFLAGS)
    endif
  endif

  LOCAL_SHARED_LIBRARIES += liblog
  ifeq ($$(art_ndebug_or_debug),debug)
    LOCAL_SHARED_LIBRARIES += libartd
  else
    LOCAL_SHARED_LIBRARIES += libart
  endif

  LOCAL_C_INCLUDES += $(ART_C_INCLUDES) art/runtime
  LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)
  LOCAL_MULTILIB := both

  LOCAL_ADDITIONAL_DEPENDENCIES := art/build/Android.common_build.mk
  LOCAL_ADDITIONAL_DEPENDENCIES += $(LOCAL_PATH)/Android.mk
  LOCAL_NATIVE_COVERAGE := $(ART_COVERAGE)
  # For simulator_arm64.
  ifeq ($$(art_ndebug_or_debug),debug)
     LOCAL_SHARED_LIBRARIES += libvixl
  else
     LOCAL_SHARED_LIBRARIES += libvixl
  endif
  ifeq ($$(art_target_or_host),target)
    include $(BUILD_SHARED_LIBRARY)
  else # host
    include $(BUILD_HOST_SHARED_LIBRARY)
  endif
endef

ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-libart-simulator,host,ndebug))
endif
ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-libart-simulator,host,debug))
endif
