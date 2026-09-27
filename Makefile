# =============================================================================
# Makefile: Real-Time Telemetry System
# Target Platform: Raspberry Pi Zero W (Raspberry Pi OS Lite 32-bit, armhf)
# Architecture: ARMv6zk (BCM2835, ARM1176JZF-S, VFPv2 Hard Float)
# =============================================================================

# Cross-compiler prefix (can be overridden on command line: make CROSS_COMPILE=arm-linux-gnueabihf-)
CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            := $(CROSS_COMPILE)gcc

# Target binary name
TARGET        := rtes_telemetry

# ARMv6zk architecture flags specific to Raspberry Pi Zero W
# CRITICAL: -marm ensures 32-bit ARM instruction set (Thumb-1 does not support VFP hard-float ABI)
ARCH_FLAGS    := -march=armv6zk -mtune=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm

# Sysroot path (defaults to $(HOME)/pi_sysroot if it exists, can be overridden)
SYSROOT ?= $(HOME)/pi_sysroot

# Architecture and Sysroot Flags
ifdef SYSROOT
    GCC_VER       := $(shell ls $(SYSROOT)/usr/lib/gcc/arm-linux-gnueabihf 2>/dev/null | sort -V | tail -n 1)
    SYSROOT_FLAGS := --sysroot=$(SYSROOT) \
                     -B$(SYSROOT)/usr/lib/gcc/arm-linux-gnueabihf/$(GCC_VER)/ \
                     -B$(SYSROOT)/usr/lib/arm-linux-gnueabihf/ \
                     -L$(SYSROOT)/usr/lib/gcc/arm-linux-gnueabihf/$(GCC_VER)/ \
                     -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf/ \
                     -L$(SYSROOT)/lib/arm-linux-gnueabihf/ \
                     -no-pie
endif

# Compiler and Linker Flags
CFLAGS        := $(ARCH_FLAGS) $(SYSROOT_FLAGS) -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS       := $(ARCH_FLAGS) $(SYSROOT_FLAGS) -pthread
LDLIBS        := -lwebsockets -lssl -lcrypto -lm

# Source and Object files
SRCS          := main.c circular_buffer.c cpu_stats.c cJSON.c
OBJS          := $(SRCS:.c=.o)
HEADERS       := circular_buffer.h cpu_stats.h cJSON.h

.PHONY: all clean native install-deps-pi install-deps-host help

# Default target: build for Raspberry Pi Zero W
all: $(TARGET)
	@echo "================================================================="
	@echo " Build Complete: $(TARGET) (ARMv6zk 32-bit)"
	@echo " Transfer this binary to your Raspberry Pi Zero W to run."
	@echo "================================================================="

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Native build (if compiling directly on the Raspberry Pi itself)
native:
	$(MAKE) CROSS_COMPILE= ARCH_FLAGS="" SYSROOT=""

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

# Helper target to show setup instructions
help:
	@echo "Targets:"
	@echo "  make                                - Cross-compile for Raspberry Pi Zero W"
	@echo "  make SYSROOT=/path/to/sysroot       - Cross-compile using a specific sysroot"
	@echo "  make native                         - Native compile (run directly on the Pi)"
	@echo "  make clean                          - Remove object files and binary"
	@echo "  make install-deps-pi                - Show commands to install dependencies on Pi"
	@echo "  make install-deps-host              - Show commands to install cross-compiler on Host"

install-deps-pi:
	@echo "Run the following on your Raspberry Pi Zero W:"
	@echo "  sudo apt update"
	@echo "  sudo apt install -y libwebsockets-dev libssl-dev"

install-deps-host:
	@echo "Run the following on your Linux / WSL host machine:"
	@echo "  sudo apt update"
	@echo "  sudo apt install -y gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf"
