# Frame Comparison Test — Native x86_64
#
# Builds two binaries from the same gpsp core:
#   test_single  — single-core renderer (reference output)
#   test_dual    — dual-core PPU simulation (snapshot + replay)
#
# Usage:
#   make
#   ./test_single bios.bin rom.gba 18000   # ~5 min @ 60fps
#   ./test_dual   bios.bin rom.gba 18000
#   python3 compare_frames.py frames_single.bin frames_dual.bin
#
# Or with JIT disabled (interpreter only):
#   make DYNAREC=0

ROOT    = ../..
LRC     = $(ROOT)/libretro/libretro-common

CC      = gcc
CXX     = g++

# JIT support (x86_64 dynarec, on by default)
DYNAREC ?= 1

ifeq ($(DYNAREC),1)
  JIT_C   = $(ROOT)/cpu_threaded.c
  JIT_S   = $(ROOT)/x86/x86_stub.S
  JIT_DEF = -DHAVE_DYNAREC
else
  JIT_C   =
  JIT_S   =
  JIT_DEF =
endif

# --- Include paths ---
INCS    = -I$(ROOT) \
          -I$(ROOT)/x86 \
          -I$(ROOT)/libretro \
          -I$(LRC)/include \
          -I.

# --- Base flags (no DUAL_CORE_PPU yet — added per-target) ---
BASE_DEFS = -DQEMU_HARNESS -DMMAP_JIT_CACHE $(JIT_DEF)

CFLAGS   = -O2 -g -Wall -Wno-unused -Wno-sign-compare $(INCS) $(BASE_DEFS)
CXXFLAGS = $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
ASFLAGS  = $(INCS) $(BASE_DEFS)
LDFLAGS  = -Wl,-z,muldefs
LDLIBS   = -lm

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

CORE_S   = $(ROOT)/bios_data.S

LRC_C   = $(LRC)/streams/file_stream.c \
           $(LRC)/vfs/vfs_implementation.c \
           $(LRC)/compat/compat_strl.c \
           $(LRC)/file/file_path.c \
           $(LRC)/string/stdstring.c \
           $(LRC)/encodings/encoding_utf.c \
           $(LRC)/time/rtime.c \
           $(LRC)/compat/fopen_utf8.c

HARNESS_C = compare_main.c compare_stubs.c

# --- Object lists (built in separate directories) ---
COMMON_BASES = $(notdir $(CORE_C:.c=.o)) \
               $(notdir $(LRC_C:.c=.o)) \
               $(notdir $(CORE_CXX:.cc=.o)) \
               $(notdir $(CORE_S:.S=.o)) \
               $(notdir $(HARNESS_C:.c=.o)) \
               $(notdir $(JIT_C:.c=.o)) \
               $(notdir $(JIT_S:.S=.o))

OBJS_S  = $(addprefix build_s/, $(COMMON_BASES))
OBJS_D  = $(addprefix build_d/, $(COMMON_BASES) ppu_sim.o)

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
          .

# --- Targets ---
.PHONY: all clean

all: test_single test_dual

test_single: $(OBJS_S)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_dual: $(OBJS_D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lpthread

# --- Build directories ---
build_s build_d:
	@mkdir -p $@

# --- Single-core rules (no DUAL_CORE_PPU) ---
build_s/%.o: %.c | build_s
	$(CC) $(CFLAGS) -c -o $@ $<

build_s/%.o: %.cc | build_s
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build_s/%.o: %.S | build_s
	$(CC) $(ASFLAGS) -c -o $@ $<

# --- Dual-core rules (with DUAL_CORE_PPU) ---
build_d/%.o: %.c | build_d
	$(CC) $(CFLAGS) -DDUAL_CORE_PPU -c -o $@ $<

build_d/%.o: %.cc | build_d
	$(CXX) $(CXXFLAGS) -DDUAL_CORE_PPU -c -o $@ $<

build_d/%.o: %.S | build_d
	$(CC) $(ASFLAGS) -DDUAL_CORE_PPU -c -o $@ $<

clean:
	rm -rf build_s build_d test_single test_dual *.bin
