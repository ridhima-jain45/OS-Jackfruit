# Makefile – Multi-Container Runtime
#
# Targets:
#   all        – build user-space binaries and kernel module
#   ci         – user-space only (no kernel headers required; for GitHub Actions)
#   module     – kernel module only
#   clean      – remove all build artifacts

# ─── kernel module (obj-m consumed by kbuild) ───
obj-m += monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Workload binaries are statically linked so they can be copied into an
# Alpine mini-rootfs and executed without musl/glibc present.
# Override with WORKLOAD_LDFLAGS= (empty) if your toolchain doesn't support -static.
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

# ─── default target ─────────────────────────────
all: $(USER_TARGETS) module

# ─── CI-safe target (no kernel headers / sudo needed) ───────
ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

# ─── kernel module ──────────────────────────────
module: monitor.ko

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ─── user-space binaries ────────────────────────
engine: engine.c monitor_ioctl.h
	gcc -O2 -Wall -Wextra -o engine engine.c -lpthread

memory_hog: memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse io_pulse.c

# ─── clean ──────────────────────────────────────
clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS)
	rm -f *.o *.mod *.mod.c *.symvers *.order .*.cmd
	rm -rf .tmp_versions
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

.PHONY: all ci module clean
