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

# ASan slows down dex2oat by ~3.5x, which translates into extremely slow first
# boot. Disabled to help speed up SANITIZE_TARGET mode.
# The supported way of using SANITIZE_TARGET is by first running a normal build,
# followed by a SANITIZE_TARGET=address build on top of it (in the same build
# tree). By disabling this module in SANITIZE_TARGET build, we keep the regular,
# uninstrumented version of it.
# Bug: 22233158
ifeq (,$(filter address, $(SANITIZE_TARGET)))

LOCAL_PATH := $(call my-dir)

include art/build/Android.executable.mk

DEX2OAT_SRC_FILES := \
	dex2oat.cc

# TODO: Remove this when the framework (installd) supports pushing the
# right instruction-set parameter for the primary architecture.
ifneq ($(filter ro.zygote=zygote64,$(PRODUCT_DEFAULT_PROPERTY_OVERRIDES)),)
  dex2oat_target_arch := 64
else
  dex2oat_target_arch := 32
endif

ifeq ($(HOST_PREFER_32_BIT),true)
  # We need to explicitly restrict the host arch to 32-bit only, as
  # giving 'both' would make build-art-executable generate a build
  # rule for a 64-bit dex2oat executable too.
  dex2oat_host_arch := 32
else
  dex2oat_host_arch := both
endif

ifeq ($(ART_BUILD_TARGET_NDEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),libcutils libart-compiler libsigchain,art/compiler,target,ndebug,$(dex2oat_target_arch)))
endif

ifeq ($(ART_BUILD_TARGET_DEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),libcutils libartd-compiler libsigchain,art/compiler,target,debug,$(dex2oat_target_arch)))
endif

# Note: the order is important because of static linking resolution.
DEX2OAT_STATIC_DEPENDENCIES := \
  libziparchive-host \
  libnativehelper \
  libnativebridge \
  libnativeloader \
  libsigchain_dummy \
  libvixl \
  liblog \
  libz \
  libbacktrace \
  libLLVMObject \
  libLLVMBitReader \
  libLLVMMC \
  libLLVMMCParser \
  libLLVMCore \
  libLLVMSupport \
  libcutils \
  libunwindbacktrace \
  libutils \
  libbase \
  liblz4 \
  liblzma

# We always build dex2oat and dependencies, even if the host build is otherwise disabled, since they are used to cross compile for the target.
ifeq ($(ART_BUILD_HOST_NDEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),libcutils libart-compiler libsigchain libziparchive-host liblz4,art/compiler,host,ndebug,$(dex2oat_host_arch)))
  ifeq ($(ART_BUILD_HOST_STATIC),true)
    $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),libart libart-compiler libart $(DEX2OAT_STATIC_DEPENDENCIES),art/compiler,host,ndebug,$(dex2oat_host_arch),static))
  endif
endif

ifeq ($(ART_BUILD_HOST_DEBUG),true)
  $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),libcutils libartd-compiler libsigchain libziparchive-host liblz4,art/compiler,host,debug,$(dex2oat_host_arch)))
  ifeq ($(ART_BUILD_HOST_STATIC),true)
    $(eval $(call build-art-executable,dex2oat,$(DEX2OAT_SRC_FILES),libartd libartd-compiler libartd $(DEX2OAT_STATIC_DEPENDENCIES),art/compiler,host,debug,$(dex2oat_host_arch),static))
  endif
endif

# Clear locals now they've served their purpose.
dex2oat_target_arch :=
dex2oat_host_arch :=

endif
