ccflags-y	+= -I${PWD}/rpmsg_lite
ccflags-y	+= -I${PWD}/rpmsg_lite/rpmsg-lite/lib/include
ccflags-y	+= -I${PWD}/rpmsg_lite/rpmsg-lite/lib/include/environment/xh2a_host
ccflags-y	+= -I${PWD}/rpmsg_lite/rpmsg-lite/lib/include/platform/xh2a_host
xh2a_drv-objs += rpmsg_lite/rpmsg-lite/lib/common/llist.o \
		 rpmsg_lite/rpmsg-lite/lib/virtio/virtqueue.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/rpmsg_ns.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/rpmsg_queue.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/rpmsg_lite.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/porting/environment/rpmsg_env_xh2a_host.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/porting/platform/xh2a_host/rpmsg_platform.o \
		 rpmsg_lite/xh2a_rpmsg_core.o \
		 rpmsg_lite/xh2a_rpmsg_lite.o \
		 rpmsg_lite/xh2a_rpmsg_lsm.o \
		 rpmsg_lite/xh2a_rpmsg_lite_ext.o
