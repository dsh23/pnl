# Makefile for pcie_latency DPDK harness
#
# Requires:
#   - DPDK installed (pkg-config dpdk available)
#   - RTE_SDK / RTE_TARGET or pkg-config path set
#
# Usage:
#   make
#   make clean

APP     := pcie_latency
SRCS    := pcie_latency.c

# Use pkg-config if available (DPDK >= 20.11 meson build)
PKG_OK  := $(shell pkg-config --exists libdpdk 2>/dev/null && echo yes)

ifeq ($(PKG_OK),yes)
    CFLAGS  := -O2 -g -Wall -Wextra \
               $(shell pkg-config --cflags libdpdk) \
               -DALLOW_EXPERIMENTAL_API
    LDFLAGS := $(shell pkg-config --libs libdpdk) -lm
else
    # Fallback: traditional RTE_SDK build
    ifndef RTE_SDK
        $(error "RTE_SDK not set and pkg-config dpdk not found")
    endif
    RTE_TARGET ?= x86_64-native-linuxapp-gcc
    CFLAGS  := -O2 -g -Wall -Wextra \
               -I$(RTE_SDK)/$(RTE_TARGET)/include \
               -DALLOW_EXPERIMENTAL_API
    LDFLAGS := -L$(RTE_SDK)/$(RTE_TARGET)/lib \
               -Wl,-rpath,$(RTE_SDK)/$(RTE_TARGET)/lib \
               -ldpdk -lm -lnuma -ldl -lpthread
endif

CC      := gcc

.PHONY: all clean

all: $(APP)

$(APP): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(APP) /tmp/tx_samples.csv /tmp/rx_samples.csv \
	             /tmp/tx_histo.csv   /tmp/rx_histo.csv
