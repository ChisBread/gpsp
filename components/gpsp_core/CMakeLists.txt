set(GPSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

set(GPSP_CORE_SRCS
    "${GPSP_ROOT}/main.c"
    "${GPSP_ROOT}/gba_memory.c"
    "${GPSP_ROOT}/savestate.c"
    "${GPSP_ROOT}/input.c"
    "${GPSP_ROOT}/sound.c"
    "${GPSP_ROOT}/cheats.c"
    "${GPSP_ROOT}/memmap.c"
    "${GPSP_ROOT}/serial.c"
    "${GPSP_ROOT}/gbp.c"
    "${GPSP_ROOT}/rfu.c"
    "${GPSP_ROOT}/serial_proto.c"
    "${GPSP_ROOT}/gba_cc_lut.c"
)

set(GPSP_CORE_CXX_SRCS
    "${GPSP_ROOT}/video.cc"
    "${GPSP_ROOT}/cpu.cc"
)

set(GPSP_CORE_ASM_SRCS
    "${GPSP_ROOT}/bios_data.S"
)

set(GPSP_CORE_INCLUDES
    "${GPSP_ROOT}"
    "${GPSP_ROOT}/libretro/libretro-common/include"
)

# Conditionally add dynarec sources
if(CONFIG_GPSP_DYNAREC)
    list(APPEND GPSP_CORE_SRCS "${GPSP_ROOT}/cpu_threaded.c")
    if(CONFIG_IDF_TARGET_ESP32P4)
        list(APPEND GPSP_CORE_ASM_SRCS "${GPSP_ROOT}/riscv/riscv_stub.S")
        list(APPEND GPSP_CORE_INCLUDES "${GPSP_ROOT}/riscv")
    endif()
endif()

if(CONFIG_IDF_TARGET_ESP32P4 AND CONFIG_GPSP_P4_SIMD_PIXEL_OPS)
    list(APPEND GPSP_CORE_ASM_SRCS "${GPSP_ROOT}/riscv/p4_pie_pixel_ops.S")
endif()

# libretro-common compatibility sources (file I/O, strings)
set(LIBRETRO_COMMON_SRCS
    "${GPSP_ROOT}/libretro/libretro-common/compat/compat_posix_string.c"
    "${GPSP_ROOT}/libretro/libretro-common/compat/compat_strl.c"
    "${GPSP_ROOT}/libretro/libretro-common/compat/fopen_utf8.c"
    "${GPSP_ROOT}/libretro/libretro-common/encodings/encoding_utf.c"
    "${GPSP_ROOT}/libretro/libretro-common/file/file_path.c"
    "${GPSP_ROOT}/libretro/libretro-common/file/file_path_io.c"
    "${GPSP_ROOT}/libretro/libretro-common/streams/file_stream.c"
    "${GPSP_ROOT}/libretro/libretro-common/string/stdstring.c"
    "${GPSP_ROOT}/libretro/libretro-common/time/rtime.c"
    "${GPSP_ROOT}/libretro/libretro-common/vfs/vfs_implementation.c"
)

idf_component_register(
    SRCS
        ${GPSP_CORE_SRCS}
        ${GPSP_CORE_CXX_SRCS}
        ${GPSP_CORE_ASM_SRCS}
        ${LIBRETRO_COMMON_SRCS}
    INCLUDE_DIRS
        ${GPSP_CORE_INCLUDES}
    REQUIRES
        esp_timer
        esp_mm
)

# Core compile definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    INLINE=inline
    _GNU_SOURCE
)

if(CONFIG_GPSP_CPU_PROFILE_STATS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CPU_PROFILE_STATS)
endif()

if(CONFIG_GPSP_DYNAREC)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        HAVE_DYNAREC
        RISCV_ARCH
    )
endif()

if(CONFIG_GPSP_SMALL_TRANSLATION_CACHE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SMALL_TRANSLATION_CACHE)
endif()

if(CONFIG_GPSP_ROM_BUFFER_SIZE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        ROM_BUFFER_SIZE=${CONFIG_GPSP_ROM_BUFFER_SIZE}
    )
endif()

# Aggressive optimization for hot emulation code
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)

# Enable Zbb (bit-manipulation) extension
if(CONFIG_GPSP_HAVE_ZBB)
    target_compile_options(${COMPONENT_LIB} PRIVATE -march=rv32imafc_zicsr_zifencei_zbb_xespv)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HAVE_ZBB)
endif()

# Suppress warnings in upstream code (treated as errors by IDF default)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-maybe-uninitialized
    -Wno-empty-body
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
)
