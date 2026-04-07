# ARM JIT test harness — via qemu-arm
#
# Reuses harness_main.c / harness_stubs.c from qemu_harness
# with the ARM (32-bit) JIT backend.
#
# Usage:
#   make
#   qemu-arm ./test_gba_arm ../../bios/gba_bios.bin ../../bios/game.gba 2500

ROOT    = ../..
LRC     = $(ROOT)/libretro/libretro-common
QHARNESS = ../qemu_harness

CROSS   = /opt/arm-linux-musleabihf-cross/bin/arm-linux-musleabihf-
CC      = $(CROSS)gcc
CXX     = $(CROSS)g++
AS      = $(CROSS)gcc

MARCH   = -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -marm

# --- Source files ---
CORE_C  = $(ROOT)/gba_memory.c \
          $(ROOT)/main.c \
          $(ROOT)/cheats.c \
          $(ROOT)/input.c \
          $(ROOT)/serial.c \
          $(ROOT)/rfu.c \
          $(ROOT)/gbp.c \
          $(ROOT)/serial_proto.c \
          $(ROOT)/memmap.c \
          $(ROOT)/savestate.c \
          $(ROOT)/sound.c \
          $(ROOT)/gba_cc_lut.c

CORE_CXX = $(ROOT)/cpu.cc \
           $(ROOT)/video.cc

CORE_S  = $(ROOT)/bios_data.S

JIT_C   = $(ROOT)/cpu_threaded.c
JIT_S   = $(ROOT)/arm/arm_stub.S

LRC_C   = $(LRC)/streams/file_stream.c \
          $(LRC)/vfs/vfs_implementation.c \
          $(LRC)/compat/compat_strl.c \
          $(LRC)/file/file_path.c \
          $(LRC)/string/stdstring.c \
          $(LRC)/encodings/encoding_utf.c \
          $(LRC)/time/rtime.c \
          $(LRC)/compat/fopen_utf8.c

HARNESS_C = $(QHARNESS)/harness_main.c $(QHARNESS)/harness_stubs.c

# --- Include paths ---
INCS    = -I$(ROOT) \
          -I$(ROOT)/arm \
          -I$(ROOT)/libretro \
          -I$(LRC)/include \
          -I$(QHARNESS)

# --- Flags ---
DEFS    = -DQEMU_HARNESS -DHAVE_DYNAREC -DARM_ARCH

CFLAGS   = $(MARCH) -static -O2 -g -Wall -Wno-unused -Wno-sign-compare $(DEFS) $(INCS)
CXXFLAGS = $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
ASFLAGS  = $(MARCH) $(DEFS) $(INCS)
LDFLAGS  = $(MARCH) -static -Wl,-z,muldefs
LDLIBS   = -lm -lgcc -lc -lgcc

# --- Objects (all built in build/ subdirectory) ---
ALL_SRC_C   = $(CORE_C) $(LRC_C) $(HARNESS_C) $(JIT_C)
ALL_SRC_CXX = $(CORE_CXX)
ALL_SRC_S   = $(CORE_S) $(JIT_S)

OBJS = $(addprefix build/, $(notdir $(ALL_SRC_C:.c=.o))) \
       $(addprefix build/, $(notdir $(ALL_SRC_CXX:.cc=.o))) \
       $(addprefix build/, $(notdir $(ALL_SRC_S:.S=.o)))

# --- VPATH for source lookup ---
VPATH   = $(ROOT) \
          $(ROOT)/arm \
          $(ROOT)/libretro \
          $(LRC)/streams \
          $(LRC)/vfs \
          $(LRC)/compat \
          $(LRC)/file \
          $(LRC)/string \
          $(LRC)/encodings \
          $(LRC)/time \
          $(QHARNESS) \
          .

TARGET  = test_gba_arm

.PHONY: all clean

all: build $(TARGET)

build:
	@mkdir -p build

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: %.cc | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/%.o: %.S | build
	$(AS) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf build $(TARGET)
