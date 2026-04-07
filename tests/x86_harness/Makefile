# x86_64 JIT test harness — baseline for comparing against RISC-V JIT
#
# Reuses harness_main.c / harness_stubs.c from qemu_harness
# with the x86 JIT backend instead of RISC-V.
#
# Usage:
#   make
#   ./test_gba_x86 ../../bios/gba_bios.bin ../../bios/game.gba 2500

ROOT    = ../..
LRC     = $(ROOT)/libretro/libretro-common
QHARNESS = ../qemu_harness

CC      = gcc
CXX     = g++

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
JIT_S   = $(ROOT)/x86/x86_stub.S

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
          -I$(ROOT)/x86 \
          -I$(ROOT)/libretro \
          -I$(LRC)/include \
          -I$(QHARNESS)

# --- Flags ---
DEFS    = -DQEMU_HARNESS -DMMAP_JIT_CACHE -DHAVE_DYNAREC

CFLAGS   = -O2 -g -Wall -Wno-unused -Wno-sign-compare $(DEFS) $(INCS)
CXXFLAGS = $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
ASFLAGS  = $(DEFS) $(INCS)
LDFLAGS  = -Wl,-z,muldefs
LDLIBS   = -lm

# --- Objects (all built in build/ subdirectory) ---
ALL_SRC_C   = $(CORE_C) $(LRC_C) $(HARNESS_C) $(JIT_C)
ALL_SRC_CXX = $(CORE_CXX)
ALL_SRC_S   = $(CORE_S) $(JIT_S)

OBJS = $(addprefix build/, $(notdir $(ALL_SRC_C:.c=.o))) \
       $(addprefix build/, $(notdir $(ALL_SRC_CXX:.cc=.o))) \
       $(addprefix build/, $(notdir $(ALL_SRC_S:.S=.o)))

# --- VPATH for source lookup ---
VPATH   = $(ROOT) \
          $(ROOT)/x86 \
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

TARGET  = test_gba_x86

.PHONY: all clean

all: $(TARGET)

build:
	@mkdir -p build

$(TARGET): $(OBJS) | build
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)


build/%.o: %.c ; @mkdir -p $(dir $@) && $(CC) $(CFLAGS) -c -o $@ $<

build/%.o: %.cc ; @mkdir -p $(dir $@) && $(CXX) $(CXXFLAGS) -c -o $@ $<

build/%.o: %.S ; @mkdir -p $(dir $@) && $(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf build $(TARGET)
