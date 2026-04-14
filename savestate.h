/* gameplaySP
 *
 * Copyright (C) 2023 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stddef.h>
#include <stdbool.h>

typedef bool (*gba_state_write_callback_t)(void *context,
                                           size_t offset,
                                           const void *data,
                                           size_t size);

typedef bool (*gba_state_read_callback_t)(void *context,
                                          size_t offset,
                                          void *data,
                                          size_t size);

void savestate_write_u8(u8 **p, u8 value);
void savestate_write_u32(u8 **p, u32 value);
void savestate_write_bytes(u8 **p, const void *data, size_t size);
void savestate_patch_u32(u8 *p, u32 value);
size_t savestate_ptr_diff(const u8 *endp, const u8 *startp);
u8 savestate_read_u8(const u8 *p);
u32 savestate_read_u32(const u8 *p);
bool savestate_read_bytes(const u8 *p, void *data, size_t size);

#define BSON_TYPE_STR       0x02
#define BSON_TYPE_DOC       0x03
#define BSON_TYPE_ARR       0x04
#define BSON_TYPE_BIN       0x05
#define BSON_TYPE_INT32     0x10

#define bson_write_u32(p, value)                \
do {                                            \
  savestate_write_u32(&(p), (u32)(value));      \
} while (0)

#define bson_read_u32(p)                        \
  savestate_read_u32((const u8 *)(p))

#define bson_write_cstring(p, value)            \
do {                                            \
  size_t __slen = strlen(value) + 1;            \
  savestate_write_bytes(&(p), value, __slen);   \
} while (0)

#define bson_write_int32(p, key, value)         \
do {                                            \
  savestate_write_u8(&(p), 0x10);               \
  bson_write_cstring(p, key);                   \
  bson_write_u32(p, value);                     \
} while (0)

#define bson_write_int32array(p, key, arr, cnt) \
do {                                            \
  u32 _n;                                       \
  u32 *arrptr = (u32*)(arr);                    \
  savestate_write_u8(&(p), 0x4);                \
  bson_write_cstring(p, key);                   \
  bson_write_u32(p, 5 + (cnt) * 8);             \
  for (_n = 0; _n < (cnt); _n++) {              \
    char ak[3] = {                              \
      (char)('0' + (_n/10)),                    \
      (char)('0' + (_n%10)),                    \
       0 };                                     \
    bson_write_int32(p, ak, arrptr[_n]);        \
  }                                             \
  savestate_write_u8(&(p), 0);                  \
} while (0)

#define bson_write_bytes(p, key, value, vlen)   \
do {                                            \
  savestate_write_u8(&(p), 0x05);               \
  bson_write_cstring(p, key);                   \
  bson_write_u32(p, vlen);                      \
  savestate_write_u8(&(p), 0);                  \
  savestate_write_bytes(&(p), value, vlen);     \
} while (0)

#define bson_start_document(p, key, hdrptr)     \
do {                                            \
  savestate_write_u8(&(p), 0x03);               \
  bson_write_cstring(p, key);                   \
  hdrptr = p;                                   \
  bson_write_u32(p, 0);                         \
} while (0)

#define bson_finish_document(p, hdrptr)         \
do {                                            \
  u32 _sz = (u32)savestate_ptr_diff(p, hdrptr) + 1; \
  savestate_write_u8(&(p), 0);                  \
  savestate_patch_u32(hdrptr, _sz);             \
} while (0)

bool bson_contains_key(const u8 *srcp, const char *key, u8 keytype);
const u8* bson_find_key(const u8 *srcp, const char *key);
bool bson_read_int32(const u8 *srcp, const char *key, u32* value);
bool bson_read_int32_array(const u8 *srcp, const char *key, u32* value, unsigned cnt);
bool bson_read_bytes(const u8 *srcp, const char *key, void* buffer, unsigned cnt);

/* this is an upper limit, leave room for future (?) stuff */
#define GBA_STATE_MEM_SIZE                    (416*1024)
#define GBA_STATE_MAGIC                       0x06BAC0DE
#define GBA_STATE_VERSION                     0x00010004

bool gba_load_state(const void *src);
bool gba_load_state_from_callback(gba_state_read_callback_t callback, void *context);
void gba_save_state(void *dst);
bool gba_save_state_to_callback(gba_state_write_callback_t callback, void *context);

#endif

