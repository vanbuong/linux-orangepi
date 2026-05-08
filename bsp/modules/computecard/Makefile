#
# Copyright (C) 2015-2019 Allwinner Technology Limited. All rights reserved.
#
# This program is free software and is provided to you under the terms of the GNU
# General Public License version 2 as published by the Free Software Foundation, and
# any use by you of this program is subject to the terms of such GNU licence.
#
# Author: Albert Yu <yuxyun@allwinnertech.com>
#

# Do not use allwinner private variables. But for compatible solution of old build
# scritp, load it as default values. Please remove it in the furture.
# All need setup:
#
# KERNEL_SRC_DIR  (KERNEL_SRC)
# KERNEL_OUT_DIR  (O)
# MODULE_INS_DIR  (INSTALL_MOD_PATH)

# Init all variables
KERNEL_SRC_DIR := $(LICHEE_KERN_DIR)
CONFIG_OS_TYPE := $(LICHEE_PLATFORM)
MODULE_INS_DIR := $(LICHEE_MOD_DIR)
KERNEL_OUT_DIR := $(O)

# Re-setup it
KERNEL_SRC_DIR := $(word 1, $(KERNEL_SRC_DIR) $(KERNEL_SRC))
KERNEL_OUT_DIR := $(word 1, $(KERNEL_OUT_DIR) $(KERNEL_SRC_DIR))
CONFIG_OS_TYPE := $(word 1, $(CONFIG_OS_TYPE) android)

ifeq ($(KERNEL_SRC_DIR),)
  $(error KERNEL_SRC_DIR not setup)
endif

KERNEL_VERSION := $(shell awk -F\" '/UTS_RELEASE/{print $$2}' $(KERNEL_OUT_DIR)/include/generated/utsrelease.h 2>/dev/null)

ifneq ($(INSTALL_MOD_PATH),)
  MODULE_INS_DIR := $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)
  MODULE_INS_DIR := $(patsubst %/, %, $(MODULE_INS_DIR))
endif

# OUT/TOP maybe cause gpu build out not right, unexport it.
unexport OUT
unexport TOP

export KDIR = $(KERNEL_SRC_DIR)
export KERNELDIR = $(LICHEE_OUT_DIR)/kernel/build

ifneq ($(KERNEL_OUT_DIR),)
  -include $(KERNEL_OUT_DIR)/.config
else
  -include .buildconfig
  -include $(LICHEE_PLAT_OUT)/.config
endif

COMPUTECARD_TYPE       := $(word 1, $(COMPUTECARD_TYPE) $(CONFIG_AW_COMPUTECARD_TYPE))

ifneq ($(findstring $(COMPUTECARD_TYPE), "houmo-xh2a"),)
  COMPUTECARD_BUILD_DIR = houmo
endif

##############################################################
.PHONY: modules clean modules_install

ifneq ($(COMPUTECARD_BUILD_DIR),)
modules clean modules_install:
	$(MAKE) -C $(COMPUTECARD_BUILD_DIR) $@
else
modules clean modules_install:
	@echo "Nothing to do for unsupport configuration"
endif
