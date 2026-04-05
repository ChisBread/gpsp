
// Instrumentation and debugging code
// Used to count instruction and register usage
// Also provides some tracing capabilities

#ifdef CPU_PROFILE_STATS
  #include "esp_log.h"

  typedef struct {
    u32 frames;
    u32 total_cycles;
    u32 update_cycles;
    u32 core_arm_cycles;
    u32 core_thumb_cycles;
    u32 dynarec_total_cycles;
    u32 dynarec_lookup_cycles;
    u32 dynarec_translate_cycles;
    u32 dynarec_icache_sync_cycles;
    u32 scanline_cycles;
    u32 scanline_order_cycles;
    u32 scanline_bg_cycles;
    u32 scanline_bg_text_fast_cycles;
    u32 scanline_bg_text_mosaic_cycles;
    u32 scanline_bg_affine_cycles;
    u32 scanline_bg_bitmap_cycles;
    u32 scanline_obj_cycles;
    u32 scanline_effect_cycles;
    u32 scanline_blank_cycles;
    u32 scanline_affine_cycles;
    u32 sound_cycles;
    u32 timer_cycles;
    u32 serial_cycles;
    u32 dma_cycles;
    u32 irq_cycles;
    u32 arm_count;
    u32 thumb_count;
    u32 core_arm_to_thumb_switches;
    u32 core_thumb_to_arm_switches;
    u32 core_pc_region_switches;
    u32 core_gamepak_page_loads;
    u32 dynarec_frames;
    u32 dynarec_lookup_hits;
    u32 dynarec_lookup_misses;
    u32 dynarec_translate_arm_blocks;
    u32 dynarec_translate_thumb_blocks;
    u32 dynarec_translate_arm_ram_blocks;
    u32 dynarec_translate_thumb_ram_blocks;
    u32 dynarec_flush_rom_count;
    u32 dynarec_flush_ram_count;
    u32 dynarec_icache_sync_count;
    u32 pc_region[16];
  } cpu_profile_stats_t;

  extern cpu_profile_stats_t cpu_prof;

  static inline u32 cpu_prof_cycles(void)
  {
    u32 cycles;
    __asm__ volatile("rdcycle %0" : "=r"(cycles));
    return cycles;
  }

  #define CPU_PROF_TOTAL_BEGIN() u32 _cpu_prof_total_begin = cpu_prof_cycles()
  #define CPU_PROF_TOTAL_END() cpu_prof.total_cycles += cpu_prof_cycles() - _cpu_prof_total_begin
  #define CPU_PROF_ARM() cpu_prof.arm_count++
  #define CPU_PROF_THUMB() cpu_prof.thumb_count++
  #define CPU_PROF_PC_REGION(pc) cpu_prof.pc_region[((pc) >> 24) & 0x0F]++
  #define CPU_PROF_FRAME() cpu_prof.frames++

  #define CPU_PROF_INC(field) cpu_prof.field++
  #define CPU_PROF_SCOPE_BEGIN(name) u32 name = cpu_prof_cycles()
  #define CPU_PROF_SCOPE_ACC(field, name) cpu_prof.field += cpu_prof_cycles() - (name)

  static inline void cpu_prof_reset(void)
  {
    memset(&cpu_prof, 0, sizeof(cpu_prof));
  }

  static inline void cpu_prof_print(void)
  {
    u32 frames;
    u32 total;
    u32 update;
    u32 core_arm;
    u32 core_thumb;
    u32 dynarec_total;
    u32 dynarec_lookup;
    u32 dynarec_translate;
    u32 dynarec_icache_sync;
    u32 scanline;
    u32 scanline_order;
    u32 scanline_bg;
    u32 scanline_bg_text_fast;
    u32 scanline_bg_text_mosaic;
    u32 scanline_bg_affine;
    u32 scanline_bg_bitmap;
    u32 scanline_bg_rest;
    u32 scanline_obj;
    u32 scanline_fx;
    u32 scanline_blank;
    u32 scanline_affine;
    u32 scanline_rest;
    u32 sound;
    u32 timers;
    u32 serial;
    u32 dma;
    u32 irq;
    u32 exec;
    u32 update_other;
    u32 arm_to_thumb;
    u32 thumb_to_arm;
    u32 pc_region_switches;
    u32 gamepak_page_loads;
    u32 dynarec_frames;
    u32 dynarec_hits;
    u32 dynarec_misses;
    u32 dynarec_arm_blocks;
    u32 dynarec_thumb_blocks;
    u32 dynarec_arm_ram_blocks;
    u32 dynarec_thumb_ram_blocks;
    u32 dynarec_flush_rom;
    u32 dynarec_flush_ram;
    u32 dynarec_sync_count;
    u32 i;

    if (cpu_prof.frames == 0)
      return;

    frames = cpu_prof.frames;
    total = cpu_prof.total_cycles / frames;
    update = cpu_prof.update_cycles / frames;
    core_arm = cpu_prof.core_arm_cycles / frames;
    core_thumb = cpu_prof.core_thumb_cycles / frames;
    dynarec_total = cpu_prof.dynarec_total_cycles / frames;
    dynarec_lookup = cpu_prof.dynarec_lookup_cycles / frames;
    dynarec_translate = cpu_prof.dynarec_translate_cycles / frames;
    dynarec_icache_sync = cpu_prof.dynarec_icache_sync_cycles / frames;
    scanline = cpu_prof.scanline_cycles / frames;
    scanline_order = cpu_prof.scanline_order_cycles / frames;
    scanline_bg = cpu_prof.scanline_bg_cycles / frames;
    scanline_bg_text_fast = cpu_prof.scanline_bg_text_fast_cycles / frames;
    scanline_bg_text_mosaic = cpu_prof.scanline_bg_text_mosaic_cycles / frames;
    scanline_bg_affine = cpu_prof.scanline_bg_affine_cycles / frames;
    scanline_bg_bitmap = cpu_prof.scanline_bg_bitmap_cycles / frames;
    scanline_obj = cpu_prof.scanline_obj_cycles / frames;
    scanline_fx = cpu_prof.scanline_effect_cycles / frames;
    scanline_blank = cpu_prof.scanline_blank_cycles / frames;
    scanline_affine = cpu_prof.scanline_affine_cycles / frames;
    sound = cpu_prof.sound_cycles / frames;
    timers = cpu_prof.timer_cycles / frames;
    serial = cpu_prof.serial_cycles / frames;
    dma = cpu_prof.dma_cycles / frames;
    irq = cpu_prof.irq_cycles / frames;
    arm_to_thumb = cpu_prof.core_arm_to_thumb_switches / frames;
    thumb_to_arm = cpu_prof.core_thumb_to_arm_switches / frames;
    pc_region_switches = cpu_prof.core_pc_region_switches / frames;
    gamepak_page_loads = cpu_prof.core_gamepak_page_loads / frames;
    dynarec_frames = cpu_prof.dynarec_frames / frames;
    dynarec_hits = cpu_prof.dynarec_lookup_hits / frames;
    dynarec_misses = cpu_prof.dynarec_lookup_misses / frames;
    dynarec_arm_blocks = cpu_prof.dynarec_translate_arm_blocks / frames;
    dynarec_thumb_blocks = cpu_prof.dynarec_translate_thumb_blocks / frames;
    dynarec_arm_ram_blocks = cpu_prof.dynarec_translate_arm_ram_blocks / frames;
    dynarec_thumb_ram_blocks = cpu_prof.dynarec_translate_thumb_ram_blocks / frames;
    dynarec_flush_rom = cpu_prof.dynarec_flush_rom_count / frames;
    dynarec_flush_ram = cpu_prof.dynarec_flush_ram_count / frames;
    dynarec_sync_count = cpu_prof.dynarec_icache_sync_count / frames;
    exec = (total > update) ? (total - update) : 0;
    scanline_rest = scanline;
    if (scanline_rest > scanline_order) scanline_rest -= scanline_order; else scanline_rest = 0;
    if (scanline_rest > scanline_bg) scanline_rest -= scanline_bg; else scanline_rest = 0;
    if (scanline_rest > scanline_obj) scanline_rest -= scanline_obj; else scanline_rest = 0;
    if (scanline_rest > scanline_fx) scanline_rest -= scanline_fx; else scanline_rest = 0;
    if (scanline_rest > scanline_blank) scanline_rest -= scanline_blank; else scanline_rest = 0;
    if (scanline_rest > scanline_affine) scanline_rest -= scanline_affine; else scanline_rest = 0;
    scanline_bg_rest = scanline_bg;
    if (scanline_bg_rest > scanline_bg_text_fast) scanline_bg_rest -= scanline_bg_text_fast; else scanline_bg_rest = 0;
    if (scanline_bg_rest > scanline_bg_text_mosaic) scanline_bg_rest -= scanline_bg_text_mosaic; else scanline_bg_rest = 0;
    if (scanline_bg_rest > scanline_bg_affine) scanline_bg_rest -= scanline_bg_affine; else scanline_bg_rest = 0;
    if (scanline_bg_rest > scanline_bg_bitmap) scanline_bg_rest -= scanline_bg_bitmap; else scanline_bg_rest = 0;
    update_other = update;
    if (update_other > scanline) update_other -= scanline; else update_other = 0;
    if (update_other > sound) update_other -= sound; else update_other = 0;
    if (update_other > timers) update_other -= timers; else update_other = 0;
    if (update_other > serial) update_other -= serial; else update_other = 0;
    if (update_other > dma) update_other -= dma; else update_other = 0;
    if (update_other > irq) update_other -= irq; else update_other = 0;

    ESP_LOGI("CPU_PROF", "=== CPU Profile (%u frames, %u Kcyc/frame) ===",
             frames, total / 1000);
    ESP_LOGI("CPU_PROF", "  ARM/frame: %u  Thumb/frame: %u  Total/frame: %u",
             cpu_prof.arm_count / frames,
             cpu_prof.thumb_count / frames,
             (cpu_prof.arm_count + cpu_prof.thumb_count) / frames);
    ESP_LOGI("CPU_PROF", "  Exec core:    %8u (%2u%%)",
             exec, total ? (exec * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    ARM core:   %8u (%2u%%) | %u cyc/insn",
         core_arm,
         total ? (core_arm * 100) / total : 0,
         cpu_prof.arm_count ? (cpu_prof.core_arm_cycles / cpu_prof.arm_count) : 0);
    ESP_LOGI("CPU_PROF", "    Thumb core: %8u (%2u%%) | %u cyc/insn",
         core_thumb,
         total ? (core_thumb * 100) / total : 0,
         cpu_prof.thumb_count ? (cpu_prof.core_thumb_cycles / cpu_prof.thumb_count) : 0);
    ESP_LOGI("CPU_PROF", "    mode sw:    A->T %u  T->A %u  pc_reg %u  page_ld %u",
         arm_to_thumb, thumb_to_arm, pc_region_switches, gamepak_page_loads);
        if (cpu_prof.dynarec_frames) {
       u32 dynarec_exec = (dynarec_total > update) ? (dynarec_total - update) : dynarec_total;
       ESP_LOGI("CPU_PROF", "    Dynarec:    %8u (%2u%%)",
         dynarec_exec, total ? (dynarec_exec * 100) / total : 0);
       ESP_LOGI("CPU_PROF", "      lookup:   %8u | hit %u miss %u",
         dynarec_lookup, dynarec_hits, dynarec_misses);
       ESP_LOGI("CPU_PROF", "      xlate:    %8u | arm %u thumb %u | ram arm %u thumb %u",
         dynarec_translate,
         dynarec_arm_blocks,
         dynarec_thumb_blocks,
         dynarec_arm_ram_blocks,
         dynarec_thumb_ram_blocks);
       ESP_LOGI("CPU_PROF", "      sync:     %8u | icache %u flush rom %u ram %u | active %u",
         dynarec_icache_sync,
         dynarec_sync_count,
         dynarec_flush_rom,
         dynarec_flush_ram,
         dynarec_frames);
        }
    ESP_LOGI("CPU_PROF", "  update_gba:   %8u (%2u%%)",
             update, total ? (update * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    scanline:   %8u (%2u%%)",
             scanline, total ? (scanline * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      bg:       %8u (%2u%%)",
           scanline_bg, total ? (scanline_bg * 100) / total : 0);
        ESP_LOGI("CPU_PROF", "        text_fast:   %8u (%2u%%)",
           scanline_bg_text_fast, total ? (scanline_bg_text_fast * 100) / total : 0);
        ESP_LOGI("CPU_PROF", "        text_mosaic: %8u (%2u%%)",
           scanline_bg_text_mosaic, total ? (scanline_bg_text_mosaic * 100) / total : 0);
        ESP_LOGI("CPU_PROF", "        affine_bg:   %8u (%2u%%)",
           scanline_bg_affine, total ? (scanline_bg_affine * 100) / total : 0);
        ESP_LOGI("CPU_PROF", "        bitmap_bg:   %8u (%2u%%)",
           scanline_bg_bitmap, total ? (scanline_bg_bitmap * 100) / total : 0);
        ESP_LOGI("CPU_PROF", "        bg_rest:     %8u (%2u%%)",
           scanline_bg_rest, total ? (scanline_bg_rest * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      obj:      %8u (%2u%%)",
           scanline_obj, total ? (scanline_obj * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      effects:  %8u (%2u%%)",
           scanline_fx, total ? (scanline_fx * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      order:    %8u (%2u%%)",
           scanline_order, total ? (scanline_order * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      affine:   %8u (%2u%%)",
           scanline_affine, total ? (scanline_affine * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      blank:    %8u (%2u%%)",
         scanline_blank, total ? (scanline_blank * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "      rest:     %8u (%2u%%)",
         scanline_rest, total ? (scanline_rest * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    sound:      %8u (%2u%%)",
             sound, total ? (sound * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    timers:     %8u (%2u%%)",
             timers, total ? (timers * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    serial:     %8u (%2u%%)",
             serial, total ? (serial * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    dma:        %8u (%2u%%)",
             dma, total ? (dma * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "    irq/misc:   %8u (%2u%%)",
             irq + update_other,
             total ? ((irq + update_other) * 100) / total : 0);
    ESP_LOGI("CPU_PROF", "  PC region (per frame):");
    for (i = 0; i < 16; i++)
    {
      if (cpu_prof.pc_region[i])
        ESP_LOGI("CPU_PROF", "    [0x%X]: %u", i, cpu_prof.pc_region[i] / frames);
    }

    cpu_prof_reset();
  }
#else
  #define CPU_PROF_TOTAL_BEGIN()
  #define CPU_PROF_TOTAL_END()
  #define CPU_PROF_ARM()
  #define CPU_PROF_THUMB()
  #define CPU_PROF_PC_REGION(pc)
  #define CPU_PROF_FRAME()
  #define CPU_PROF_INC(field)
  #define CPU_PROF_SCOPE_BEGIN(name)
  #define CPU_PROF_SCOPE_ACC(field, name)
#endif


#ifdef MEMORY_STATS_ANALYZE
  // Collects memory stats by region, access type, etc.

  u32 memory_region_access_read_u8[16];
  u32 memory_region_access_read_s8[16];
  u32 memory_region_access_read_u16[16];
  u32 memory_region_access_read_s16[16];
  u32 memory_region_access_read_u32[16];
  u32 memory_region_access_write_u8[16];
  u32 memory_region_access_write_u16[16];
  u32 memory_region_access_write_u32[16];

  #define STATS_MEMORY_ACCESS(op, size, region) \
    memory_region_access_##op##_##size[region]++;

#else

  #define STATS_MEMORY_ACCESS(write, u32, region)

#endif

#ifdef REGISTER_USAGE_ANALYZE

  u64 instructions_total = 0;
  u64 arm_reg_freq[16];
  u64 arm_reg_access_total = 0;
  u64 arm_instructions_total = 0;
  u64 thumb_reg_freq[16];
  u64 thumb_reg_access_total = 0;
  u64 thumb_instructions_total = 0;

  // mla/long mla's addition operand are not counted yet.

  #define using_register(instruction_set, register, type)                     \
    instruction_set##_reg_freq[register]++;                                   \
    instruction_set##_reg_access_total++                                      \

  #define using_register_list(instruction_set, rlist, count)                  \
  {                                                                           \
    u32 i;                                                                    \
    for(i = 0; i < count; i++)                                                \
    {                                                                         \
      if((reg_list >> i) & 0x01)                                              \
      {                                                                       \
        using_register(instruction_set, i, memory_target);                    \
      }                                                                       \
    }                                                                         \
  }                                                                           \

  #define using_instruction(instruction_set)                                  \
    instruction_set##_instructions_total++;                                   \
    instructions_total++                                                      \

  int sort_tagged_element(const void *_a, const void *_b)
  {
    const u64 *a = _a;
    const u64 *b = _b;

    return (int)(b[1] - a[1]);
  }

  void print_register_usage(void)
  {
    u32 i;
    u64 arm_reg_freq_tagged[32];
    u64 thumb_reg_freq_tagged[32];
    double percent;
    double percent_total = 0.0;

    for(i = 0; i < 16; i++)
    {
      arm_reg_freq_tagged[i * 2] = i;
      arm_reg_freq_tagged[(i * 2) + 1] = arm_reg_freq[i];
      thumb_reg_freq_tagged[i * 2] = i;
      thumb_reg_freq_tagged[(i * 2) + 1] = thumb_reg_freq[i];
    }

    qsort(arm_reg_freq_tagged, 16, sizeof(u64) * 2, sort_tagged_element);
    qsort(thumb_reg_freq_tagged, 16, sizeof(u64) * 2, sort_tagged_element);

    printf("ARM register usage (%lf%% ARM instructions):\n",
     (arm_instructions_total * 100.0) / instructions_total);
    for(i = 0; i < 16; i++)
    {
      percent = (arm_reg_freq_tagged[(i * 2) + 1] * 100.0) /
       arm_reg_access_total;
      percent_total += percent;
      printf("r%02d: %lf%% (-- %lf%%)\n",
       (u32)arm_reg_freq_tagged[(i * 2)], percent, percent_total);
    }

    percent_total = 0.0;

    printf("\nThumb register usage (%lf%% Thumb instructions):\n",
     (thumb_instructions_total * 100.0) / instructions_total);
    for(i = 0; i < 16; i++)
    {
      percent = (thumb_reg_freq_tagged[(i * 2) + 1] * 100.0) /
       thumb_reg_access_total;
      percent_total += percent;
      printf("r%02d: %lf%% (-- %lf%%)\n",
       (u32)thumb_reg_freq_tagged[(i * 2)], percent, percent_total);
    }

    memset(arm_reg_freq, 0, sizeof(u64) * 16);
    memset(thumb_reg_freq, 0, sizeof(u64) * 16);
    arm_reg_access_total = 0;
    thumb_reg_access_total = 0;
  }

#else  /* REGISTER_USAGE_ANALYZE */

  #define using_register(instruction_set, register, type)
  #define using_register_list(instruction_set, rlist, count)
  #define using_instruction(instruction_set)

#endif  /* REGISTER_USAGE_ANALYZE */


#ifdef TRACE_INSTRUCTIONS
  static void interp_trace_instruction(u32 pc, u32 mode)
  {
    if (mode)
      printf("Executed arm %x\n", pc);
    else
      printf("Executed thumb %x\n", pc);
    #ifdef TRACE_REGISTERS
    print_regs();
    #endif
  }
#endif

