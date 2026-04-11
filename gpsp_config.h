
#ifndef GPSP_CONFIG_H
#define GPSP_CONFIG_H

#define GPSP_NAME                "gpSP"
#define GPSP_VERSION             "v1.1.0"
#define GPSP_NETPACKET_VERSION   "gpSP v1.0"

/* Default ROM buffer size in megabytes (this is a maximum value!) */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 32
#endif

/* Cache sizes and their config knobs */
#if defined(SMALL_TRANSLATION_CACHE)
  #ifdef ROM_HOT_ZONE
    #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2 + ROM_HOT_ZONE_SIZE)
  #else
    #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)
  #endif
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* Hot zone: extra PSRAM dedicated to surviving ROM cache flushes.
   On the first ROM flush the ring buffer of sampled-hit PCs is
   de-duplicated, sorted by frequency, and the hottest blocks are
   re-translated into a protected region.  Subsequent flushes only
   clear the area *after* the hot zone and re-insert the hot zone's
   hash entries — so hot blocks survive without re-translation.
   Enabled by defining ROM_HOT_ZONE at build time. */
#ifdef ROM_HOT_ZONE
  #define ROM_HOT_ZONE_SIZE          (1024 * 256)
  #define ROM_HOT_PC_RING_SIZE       1024
  #define ROM_HOT_DIR_MAX            2048 /* max blocks tracked in hot zone */
  #define ROM_HOT_SAMPLE_SHIFT       4    /* log2 of hit sampling rate (1/16) */
  #define ROM_HOT_ZONE_MAX_AGE       3    /* Path A flushes before forced rebuild */
  #define ROM_HOT_MIN_FREQ           2    /* min ring count to enter hot zone */
#else
  #define ROM_HOT_ZONE_SIZE          0
  #define ROM_HOT_PC_RING_SIZE       0
#endif

/* Should be an upperbound to the maximum number of bytes a single JIT'ed
   instruction can take. STM/LDM are tipically the biggest ones */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024 * 2)

/* Hash table size for ROM trans cache lookups */
#define ROM_BRANCH_HASH_BITS                           16
#define ROM_BRANCH_HASH_SIZE   (1 << ROM_BRANCH_HASH_BITS)

/* RFU Multiplayer config, do not mess around too much with it */
#define MAX_RFU_NETPLAYERS       32

/* Serial modes (multiplayer serial). */
#define MAX_SERMULT_NETPLAYERS    4

#endif
