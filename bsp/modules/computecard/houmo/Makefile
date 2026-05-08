
KERNEL_SRC_DIR := $(LICHEE_KERN_DIR)
MODULE_INS_DIR := $(LICHEE_MOD_DIR)
KERNEL_OUT_DIR := $(O)

# Re-setup it
KERNEL_SRC_DIR := $(word 1, $(KERNEL_SRC_DIR) $(KERNEL_SRC))
KERNEL_OUT_DIR := $(word 1, $(KERNEL_OUT_DIR) $(KERNEL_SRC_DIR))


ifeq ($(KERNEL_SRC_DIR),)
  $(error KERNEL_SRC_DIR not setup)
endif

KERNEL_VERSION := $(shell awk -F\" '/UTS_RELEASE/{print $$2}' $(KERNEL_OUT_DIR)/include/generated/utsrelease.h 2>/dev/null)

ifneq ($(INSTALL_MOD_PATH),)
  MODULE_INS_DIR := $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)
  MODULE_INS_DIR := $(patsubst %/, %, $(MODULE_INS_DIR))
endif

HOUMO_MOD_OUT_DIR  := $(patsubst $(KERNEL_SRC_DIR)/%,$(KERNEL_OUT_DIR)/%,$(shell pwd)/$(SOC))
# HOUMO_BUILD_CONFIG := $(shell grep "CONFIG_AW_HOUMO=m" $(KERNEL_OUT_DIR)/.config 2>/dev/null)

PWD		:= $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

KERNEL_DIR	?= $(KERNEL_SRC_DIR)
TARGET_ARCH	?= $(LICHEE_ARCH)

Q		?= @
MAKEFLAGS	+= --no-print-directory

DEVICE_NOR ?= UNKNOWN

# must be same as PACKAGE_VERSION in dkms.conf
XH2A_HOST_DRIVER_RELEASE	:= v0.7.0
XH2A_HOST_DRIVER_BUILDTIME	:= $(shell date  +"%Y-%m-%d %H:%M:%S %Z")


ENABLE_CLANG ?= $(shell grep -w 'CONFIG_CC_IS_CLANG=y' $(KERNEL_OUT_DIR)/.config 2> /dev/null)
ifneq ($(ENABLE_CLANG),)
ccflags-y	+= -Werror
else
ccflags-y	+= -Wall -Wunused-but-set-variable -Wmaybe-uninitialized
endif

ccflags-y	+= -DXH2A_HOST_DRIVER_RELEASE='"$(XH2A_HOST_DRIVER_RELEASE)"'
ccflags-y	+= -DXH2A_HOST_DRIVER_BUILDTIME='"$(XH2A_HOST_DRIVER_BUILDTIME)"'

ifeq ($(TARGET_ARCH),arm64)
ccflags-y	+= -DHOST_KERNEL_VERSION_ABOVE_5_5
endif

# enable runtime PM
ccflags-y	+= -DHM_RUNTIME_PM_ENABLE

ccflags-y	+= -I$(PWD)/include
ccflags-y	+= -I$(PWD)/public

HM_ADDRESS_LAYOUT_FILE := $(PWD)/include/houmo_address_layout.h

xh2a_drv-objs := xh2a_host_drv.o
xh2a_drv-objs += pcie/xh2a_pcie.o pcie/xh2a_pcie_hdma.o pcie/xh2a_pcie_api.o pcie/xh2a_pcie_misc.o pcie/xh2a_pcie_efuse.o
xh2a_drv-objs += sys/xh2a_sys.o sys/xh2a_ctc.o
ifneq ($(wildcard $(HM_ADDRESS_LAYOUT_FILE)),)
ccflags-y += -DHM_ADDRESS_LAYOUT
endif
xh2a_drv-objs += fast_memory/xh2a_fast_memory.o
xh2a_drv-objs += memory_allocator/xh2a_memory_allocator.o memory_allocator/allocator_algorithm.o
xh2a_drv-objs += memory_allocator/xh2a_memory_allocator_spm_api.o
xh2a_drv-objs += memory_transfer/xh2a_memory_transfer.o memory_transfer/xh2a_memory_transfer_sysdma.o
xh2a_drv-objs += transport/xh2a_transport.o transport/transport_protocol.o
xh2a_drv-objs += qspi/xh2a_qspi.o
xh2a_drv-objs += qspi/axi_crc/xh2a_axi_crc.o
ifeq ($(DEVICE_NOR), MICRON)
xh2a_drv-objs += qspi/nor_flash/device_nor_mt25.o
ccflags-y += -DDEVICE_NOR_MICRON
else ifeq ($(DEVICE_NOR), GIGADEVICE)
xh2a_drv-objs += qspi/nor_flash/device_nor_gd25.o
ccflags-y += -DDEVICE_NOR_GIGADEVICE
else ifeq ($(DEVICE_NOR), UNKNOWN)
ccflags-y += -DDEVICE_NOR_UNKNOWN
endif
xh2a_drv-objs += i2c/xh2a_i2c.o

-include $(PWD)/ipu/ipu.mk

-include $(PWD)/rpmsg_lite/rpmsg_lite.mk

obj-m		:= xh2a_drv.o

PHONY += __default
__default: modules

PHONY += modules clean modules_install
modules clean:
	$(Q)$(MAKE) -j$(shell nproc) $@ \
		ARCH=$(TARGET_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		-C $(KERNEL_DIR) M=$(PWD) O=$(KERNEL_OUT_DIR)

modules_install:
	@if [ -z "$(MODULE_INS_DIR)" ]; then \
		echo "MODULE_INS_DIR not configured, nothing to do"; \
	else \
		mkdir -p $(MODULE_INS_DIR); \
		cp -rdf xh2a_drv.ko $(MODULE_INS_DIR); \
	fi

.PHONY: $(PHONY)
