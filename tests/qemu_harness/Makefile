# Standalone GBA emulator test harness for QEMU RV32
# Usage:
#   make
#   make run BIOS=bios.bin ROM=test.gba

CROSS   ?= /opt/riscv32-linux-musl-cross/bin/riscv32-linux-musl-
CC       = $(CROSS)gcc
CXX      = $(CROSS)g++
AS       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
QEMU     = qemu-riscv32

MARCH   = -march=rv32gc -mabi=ilp32d

# Project root
ROOT    = ../..

# Mode: always build with JIT support (use --interp at runtime for interpreter)
JIT_C   = $(ROOT)/cpu_threaded.c
JIT_S   = $(ROOT)/riscv/riscv_stub.S
JIT_DEF = -DHAVE_DYNAREC -DRISCV_ARCH

CORE_C  = $(ROOT)/gba_memory.c \
          $(ROOT)/main.c \
          $(ROOT)/cheats.c \
          $(ROOT)/input.c \
          $(ROOT)/serial.c \
          $(ROOT)/rfu.c \
          $(ROOT)/gbp.c \
          $(ROOT)/serial_proto.c \
          $(ROOT)/memmap.c \
          $(ROOT)/savestate.c

CORE_CXX = $(ROOT)/cpu.cc \
           $(ROOT)/video.cc

CORE_C += $(ROOT)/sound.c

# libretro-common file I/O
LRC     = $(ROOT)/libretro/libretro-common
LRC_C   = $(LRC)/streams/file_stream.c \
          $(LRC)/vfs/vfs_implementation.c \
          $(LRC)/compat/compat_strl.c \
          $(LRC)/file/file_path.c \
          $(LRC)/string/stdstring.c \
          $(LRC)/encodings/encoding_utf.c \
          $(LRC)/time/rtime.c \
          $(LRC)/compat/fopen_utf8.c

# Our harness
HARNESS_C = harness_main.c harness_stubs.c

# --- Flags ---
INCS    = -I$(ROOT) \
          -I$(ROOT)/riscv \
          -I$(ROOT)/libretro \
          -I$(LRC)/include \
          -I.

DEFS    = -DQEMU_HARNESS \
          $(JIT_DEF)

CFLAGS  = $(MARCH) -static -O2 -g -Wall -Wno-unused $(DEFS) $(INCS)
CXXFLAGS = $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
ASFLAGS = $(MARCH) $(DEFS) $(INCS)
LDFLAGS = $(MARCH) -static -Wl,-z,muldefs
LDLIBS  = -lm -lgcc -lc -lgcc

# --- Objects ---
ALL_C   = $(CORE_C) $(LRC_C) $(HARNESS_C) $(JIT_C)
ALL_CXX = $(CORE_CXX)
ALL_S   = $(JIT_S)

OBJS    = $(ALL_C:.c=.o) $(ALL_CXX:.cc=.o) $(ALL_S:.S=.o)

TARGET  = test_gba

# --- Rules ---
.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) test_gba

run: $(TARGET)
	$(QEMU) ./$(TARGET) $(BIOS) $(ROM)
