# just for ipu debug trace
ccflags-y	+= -DIPU_DEBUG_TRACE
ccflags-y	+= -I${PWD}/ipu/include
ccflags-y	+= -I${PWD}/ipu/policy

# default schedule policy PRIORITY POLICY
IPU_SCHE_POLICY	:= IPU_PRIORITY_POLICY
ifeq ($(IPU_SCHE_POLICY), IPU_PRIORITY_POLICY)
xh2a_drv-objs += ipu/policy/xh2a_priority_policy.o
ccflags-y += -DIPU_PRIORITY_POLICY
else
$(error Invalid IPU POLICY! Use IPU_SCHE_POLICY=IPU_PRIORITY_POLICY by default!)
endif
xh2a_drv-objs += ipu/xh2a_ipu_hw.o \
		 ipu/xh2a_ipu_device.o \
		 ipu/xh2a_ipu_file_handle.o \
		 ipu/xh2a_ipu_group.o \
		 ipu/xh2a_ipu_kernel.o \
		 ipu/xh2a_ipu_interrupt.o \
		 ipu/xh2a_ipu_ioctl.o \
		 ipu/xh2a_ipu_mempool.o \
		 ipu/xh2a_ipu_load.o \
		 ipu/xh2a_ipu_policy.o


