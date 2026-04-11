# ─────────────────────────────────────────────────────────────────────
#  Unified GBA Test Harness
#
#  One Makefile to build for any host / cross-compile target.
#
#  Parameters (all optional, shown with defaults):
#
#    ARCH=x86          Target architecture: x86 | arm | riscv
#    TRACE=0           Instruction tracing: 0 | 1
#    DYNAREC=1         JIT recompiler: 0 (interpreter only) | 1
#
#  Examples:
#    make                              # x86 JIT
#    make ARCH=riscv                   # RISC-V JIT (qemu-riscv32)
#    make ARCH=arm                     # ARM32 JIT  (qemu-arm)
#    make DYNAREC=0                    # x86 interpreter
#    make clean                        # remove build artifacts
# ─────────────────────────────────────────────────────────────────────

ARCH    ?= x86
TRACE   ?= 0
DYNAREC ?= 1

ROOT    = ../..
LRC     = $(ROOT)/libretro/libretro-common

# ── Architecture presets ───────────────────────────────────────────

ifeq ($(ARCH),x86)
  CC       = gcc
  CXX      = g++
  JIT_S    = $(ROOT)/x86/x86_stub.S
  ARCH_INC = -I$(ROOT)/x86
  ARCH_DEF = -DMMAP_JIT_CACHE
  MARCH    =
  STATIC   =
  LDLIBS   = -lm
else ifeq ($(ARCH),arm)
  CROSS   ?= /opt/arm-linux-musleabihf-cross/bin/arm-linux-musleabihf-
  CC       = $(CROSS)gcc
  CXX      = $(CROSS)g++
  JIT_S    = $(ROOT)/arm/arm_stub.S
  ARCH_INC = -I$(ROOT)/arm
  ARCH_DEF = -DARM_ARCH
  MARCH    = -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -marm
  STATIC   = -static
  LDLIBS   = -lm -lgcc -lc -lgcc
else ifeq ($(ARCH),riscv)
  CROSS   ?= /opt/riscv32-linux-musl-cross/bin/riscv32-linux-musl-
  CC       = $(CROSS)gcc
  CXX      = $(CROSS)g++
  JIT_S    = $(ROOT)/riscv/riscv_stub.S
  ARCH_INC = -I$(ROOT)/riscv
  ARCH_DEF = -DRISCV_ARCH -DHAVE_RVC
  ifeq ($(ZBB),1)
    MARCH    = -march=rv32gc_zbb -mabi=ilp32d
    ARCH_DEF += -DHAVE_ZBB
  else
    MARCH    = -march=rv32gc -mabi=ilp32d
  endif
  STATIC   = -static
  LDLIBS   = -lm -lgcc -lc -lgcc
else
  $(error Unknown ARCH=$(ARCH). Use x86, arm, or riscv)
endif

# ── Feature flags ──────────────────────────────────────────────────

FEAT_DEF =

ifeq ($(DYNAREC),1)
  JIT_C    = $(ROOT)/cpu_threaded.c
  FEAT_DEF += -DHAVE_DYNAREC
else
  JIT_C    =
  JIT_S    =
endif

ifeq ($(TRACE),1)
  FEAT_DEF += -DTRACE_INSTRUCTIONS
endif

ifeq ($(SMALL_CACHE),1)
  FEAT_DEF += -DSMALL_TRANSLATION_CACHE
endif

ifeq ($(HOT_ZONE),1)
  FEAT_DEF += -DROM_HOT_ZONE
endif

# ── Common flags ───────────────────────────────────────────────────

INCS     = -I$(ROOT) $(ARCH_INC) -I$(ROOT)/libretro -I$(LRC)/include -I.
DEFS     = -DQEMU_HARNESS $(ARCH_DEF) $(FEAT_DEF)

CFLAGS   = $(MARCH) $(STATIC) -O2 -fno-pic -fno-pie -g -Wall -Wno-unused -Wno-sign-compare \
           $(DEFS) $(INCS)
CXXFLAGS = $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
ASFLAGS  = $(MARCH) -fno-pic -fno-pie $(DEFS) $(INCS)
LDFLAGS  = $(MARCH) $(STATIC) -no-pie -Wl,-z,muldefs

# ── Source files ───────────────────────────────────────────────────

CORE_C   = $(ROOT)/gba_memory.c \
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

LRC_C    = $(LRC)/streams/file_stream.c \
           $(LRC)/vfs/vfs_implementation.c \
           $(LRC)/compat/compat_strl.c \
           $(LRC)/file/file_path.c \
           $(LRC)/string/stdstring.c \
           $(LRC)/encodings/encoding_utf.c \
           $(LRC)/time/rtime.c \
           $(LRC)/compat/fopen_utf8.c

HARNESS_C = harness_main.c harness_stubs.c

# ── Build directory (per-config, allows parallel builds) ───────────

_TRACE_TAG = $(if $(filter 1,$(TRACE)),_trace)
_JIT_TAG   = $(if $(filter 0,$(DYNAREC)),_interp)
BUILD      = build/$(ARCH)$(_TRACE_TAG)$(_JIT_TAG)
TARGET     = test_gba_$(ARCH)$(_TRACE_TAG)$(_JIT_TAG)

# ── Object lists ───────────────────────────────────────────────────

ALL_C   = $(CORE_C) $(LRC_C) $(HARNESS_C) $(JIT_C)
ALL_CXX = $(CORE_CXX)
ALL_S   = $(CORE_S) $(JIT_S)

OBJS = $(addprefix $(BUILD)/, $(notdir $(ALL_C:.c=.o))) \
       $(addprefix $(BUILD)/, $(notdir $(ALL_CXX:.cc=.o))) \
       $(addprefix $(BUILD)/, $(notdir $(ALL_S:.S=.o)))

# ── VPATH (source lookup) ─────────────────────────────────────────

VPATH = $(ROOT) \
        $(ROOT)/x86 $(ROOT)/arm $(ROOT)/riscv \
        $(ROOT)/libretro \
        $(LRC)/streams $(LRC)/vfs $(LRC)/compat \
        $(LRC)/file $(LRC)/string $(LRC)/encodings $(LRC)/time \
        .

# ── Rules ──────────────────────────────────────────────────────────

.PHONY: all clean info

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILD)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cc | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf build test_gba_*

info:
	@echo "ARCH     = $(ARCH)"
	@echo "TRACE    = $(TRACE)"
	@echo "DYNAREC  = $(DYNAREC)"
	@echo "TARGET   = $(TARGET)"
	@echo "BUILD    = $(BUILD)"
	@echo "CC       = $(CC)"
	@echo "DEFS     = $(DEFS)"
