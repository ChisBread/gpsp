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

#include "common.h"
#include <stdint.h>

const u8 *state_mem_read_ptr;
u8 *state_mem_write_ptr;

typedef struct {
  gba_state_write_callback_t callback;
  void *context;
  bool enabled;
  bool failed;
} savestate_write_state_t;

static savestate_write_state_t s_state_writer;

typedef struct {
  gba_state_read_callback_t callback;
  void *context;
  bool enabled;
  bool failed;
} savestate_read_state_t;

static savestate_read_state_t s_state_reader;

static size_t savestate_ptr_to_offset(const u8 *p)
{
  return (size_t)(uintptr_t)p;
}

static u8 *savestate_offset_to_ptr(size_t offset)
{
  return (u8 *)(uintptr_t)offset;
}

static void savestate_writer_begin_memory(void)
{
  s_state_writer.callback = NULL;
  s_state_writer.context = NULL;
  s_state_writer.enabled = false;
  s_state_writer.failed = false;
}

static void savestate_writer_begin_callback(gba_state_write_callback_t callback,
                                            void *context)
{
  s_state_writer.callback = callback;
  s_state_writer.context = context;
  s_state_writer.enabled = true;
  s_state_writer.failed = false;
}

static void savestate_writer_end(void)
{
  s_state_writer.callback = NULL;
  s_state_writer.context = NULL;
  s_state_writer.enabled = false;
}

static void savestate_reader_begin_memory(void)
{
  s_state_reader.callback = NULL;
  s_state_reader.context = NULL;
  s_state_reader.enabled = false;
  s_state_reader.failed = false;
}

static void savestate_reader_begin_callback(gba_state_read_callback_t callback,
                                            void *context)
{
  s_state_reader.callback = callback;
  s_state_reader.context = context;
  s_state_reader.enabled = true;
  s_state_reader.failed = false;
}

static void savestate_reader_end(void)
{
  s_state_reader.callback = NULL;
  s_state_reader.context = NULL;
  s_state_reader.enabled = false;
}

void savestate_write_u8(u8 **p, u8 value)
{
  savestate_write_bytes(p, &value, 1);
}

void savestate_write_u32(u8 **p, u32 value)
{
  u8 bytes[4];
  bytes[0] = (u8)(value);
  bytes[1] = (u8)(value >> 8);
  bytes[2] = (u8)(value >> 16);
  bytes[3] = (u8)(value >> 24);
  savestate_write_bytes(p, bytes, sizeof(bytes));
}

void savestate_write_bytes(u8 **p, const void *data, size_t size)
{
  size_t offset;

  if (!p || !data || size == 0) {
    return;
  }

  if (!s_state_writer.enabled) {
    memcpy(*p, data, size);
    *p += size;
    return;
  }

  offset = savestate_ptr_to_offset(*p);
  if (!s_state_writer.failed &&
      (!s_state_writer.callback ||
       !s_state_writer.callback(s_state_writer.context, offset, data, size))) {
    s_state_writer.failed = true;
  }
  *p = savestate_offset_to_ptr(offset + size);
}

void savestate_patch_u32(u8 *p, u32 value)
{
  u8 bytes[4];
  bytes[0] = (u8)(value);
  bytes[1] = (u8)(value >> 8);
  bytes[2] = (u8)(value >> 16);
  bytes[3] = (u8)(value >> 24);

  if (!s_state_writer.enabled) {
    memcpy(p, bytes, sizeof(bytes));
    return;
  }

  if (!s_state_writer.failed &&
      (!s_state_writer.callback ||
       !s_state_writer.callback(s_state_writer.context,
                                savestate_ptr_to_offset(p),
                                bytes,
                                sizeof(bytes)))) {
    s_state_writer.failed = true;
  }
}

size_t savestate_ptr_diff(const u8 *endp, const u8 *startp)
{
  if (!s_state_writer.enabled) {
    return (size_t)(endp - startp);
  }

  return savestate_ptr_to_offset(endp) - savestate_ptr_to_offset(startp);
}

u8 savestate_read_u8(const u8 *p)
{
  u8 value = 0;

  if (!p) {
    return 0;
  }

  if (!s_state_reader.enabled) {
    return *p;
  }

  if (!s_state_reader.failed &&
      (!s_state_reader.callback ||
       !s_state_reader.callback(s_state_reader.context,
                                savestate_ptr_to_offset(p),
                                &value,
                                1))) {
    s_state_reader.failed = true;
    return 0;
  }

  return value;
}

u32 savestate_read_u32(const u8 *p)
{
  u8 bytes[4];

  if (!p) {
    return 0;
  }

  if (!s_state_reader.enabled) {
    return ((u32)p[3] << 24) | ((u32)p[2] << 16) |
           ((u32)p[1] << 8) | ((u32)p[0]);
  }

  if (!s_state_reader.failed &&
      (!s_state_reader.callback ||
       !s_state_reader.callback(s_state_reader.context,
                                savestate_ptr_to_offset(p),
                                bytes,
                                sizeof(bytes)))) {
    s_state_reader.failed = true;
    return 0;
  }

  return ((u32)bytes[3] << 24) | ((u32)bytes[2] << 16) |
         ((u32)bytes[1] << 8) | ((u32)bytes[0]);
}

bool savestate_read_bytes(const u8 *p, void *data, size_t size)
{
  if (!p || !data) {
    return false;
  }

  if (size == 0) {
    return true;
  }

  if (!s_state_reader.enabled) {
    memcpy(data, p, size);
    return true;
  }

  if (!s_state_reader.failed &&
      (!s_state_reader.callback ||
       !s_state_reader.callback(s_state_reader.context,
                                savestate_ptr_to_offset(p),
                                data,
                                size))) {
    s_state_reader.failed = true;
    return false;
  }

  return true;
}

static size_t savestate_read_cstring_len(const u8 *p)
{
  size_t len = 0;

  while (savestate_read_u8(p + len) != 0) {
    len++;
  }

  return len;
}

static bool savestate_key_equals(const u8 *p, const char *key, size_t keyl)
{
  size_t i;

  if (!key) {
    return false;
  }

  for (i = 0; i < keyl; i++) {
    if (savestate_read_u8(p + i) != (u8)key[i]) {
      return false;
    }
  }

  return true;
}

bool bson_contains_key(const u8 *srcp, const char *key, u8 keytype)
{
  unsigned keyl = strlen(key) + 1;
  unsigned doclen = bson_read_u32(srcp);
  const u8* p = &srcp[4];
  while (savestate_read_u8(p) != 0 && (p - srcp) < doclen) {
    u8 tp = savestate_read_u8(p);
    unsigned tlen = (unsigned)savestate_read_cstring_len(&p[1]) + 1;
    if (keyl == tlen && savestate_key_equals(&p[1], key, tlen))
      return tp == keytype;  // Found it, check type
    p += 1 + tlen;
    if (tp == BSON_TYPE_DOC || tp == BSON_TYPE_ARR)
      p += bson_read_u32(p);
    else if (tp == BSON_TYPE_BIN)
      p += bson_read_u32(p) + 1 + 4;
    else if (tp == BSON_TYPE_INT32)
      p += 4;
  }
  return false;
}

const u8* bson_find_key(const u8 *srcp, const char *key)
{
  unsigned keyl = strlen(key) + 1;
  unsigned doclen = bson_read_u32(srcp);
  const u8* p = &srcp[4];
  while (savestate_read_u8(p) != 0 && (p - srcp) < doclen) {
    u8 tp = savestate_read_u8(p);
    unsigned tlen = (unsigned)savestate_read_cstring_len(&p[1]) + 1;
    if (keyl == tlen && savestate_key_equals(&p[1], key, tlen))
      return &p[tlen + 1];
    p += 1 + tlen;
    if (tp == BSON_TYPE_DOC || tp == BSON_TYPE_ARR)
      p += bson_read_u32(p);
    else if (tp == BSON_TYPE_BIN)
      p += bson_read_u32(p) + 1 + 4;
    else if (tp == BSON_TYPE_INT32)
      p += 4;
  }
  return NULL;
}

bool bson_read_int32(const u8 *srcp, const char *key, u32* value)
{
  const u8* p = srcp ? bson_find_key(srcp, key) : NULL;
  if (!p)
    return false;
  *value = bson_read_u32(p);
  return true;
}

bool bson_read_int32_array(const u8 *srcp, const char *key, u32* value, unsigned cnt)
{
  const u8* p = srcp ? bson_find_key(srcp, key) : NULL;
  if (p) {
    unsigned arrsz = bson_read_u32(p);
    p += 4;
    if (arrsz < 5)
      return false;
    arrsz = (arrsz - 5) >> 3;
    while (arrsz--) {
      p += 4;   // type and name
      *value++ = bson_read_u32(p);
      p += 4;   // value
    }
    return true;
  }
  *value = bson_read_u32(p);
  return false;
}

bool bson_read_bytes(const u8 *srcp, const char *key, void* buffer, unsigned cnt)
{
  const u8* p = srcp ? bson_find_key(srcp, key) : NULL;
  if (p) {
    unsigned bufsz = bson_read_u32(p);
    if (bufsz != cnt)
      return false;

    // Skip byte array type and size
    if (!savestate_read_bytes(&p[5], buffer, cnt)) {
      return false;
    }
    return true;
  }
  return false;
}

static bool gba_load_state_core(const u8 *srcptr)
{
  u32 i, tmp;
  u32 docsize = bson_read_u32(srcptr);
  if (docsize != GBA_STATE_MEM_SIZE)
    return false;

  if (!bson_read_int32(srcptr, "info-magic", &tmp) || tmp != GBA_STATE_MAGIC)
    return false;
  if (!bson_read_int32(srcptr, "info-version", &tmp) || tmp != GBA_STATE_VERSION)
    return false;

  // Validate that the state file makes sense before unconditionally reading it.
  if (!cpu_check_savestate(srcptr) ||
      !input_check_savestate(srcptr) ||
      !main_check_savestate(srcptr) ||
      !memory_check_savestate(srcptr) ||
      !sound_check_savestate(srcptr))
     return false;

  if (!(cpu_read_savestate(srcptr) &&
      input_read_savestate(srcptr) &&
      main_read_savestate(srcptr) &&
      memory_read_savestate(srcptr) &&
      sound_read_savestate(srcptr)))
  {
     // TODO: this should not happen if the validation above is accurate.
     return false;
  }

  // Generate converted palette (since it is not saved)
  for(i = 0; i < 512; i++)
  {
     palette_ram_converted[i] = convert_palette(eswap16(palette_ram[i]));
  }

  video_reload_counters();

  // Reset most of the frame state and dynarec state
#ifdef HAVE_DYNAREC
  if (dynarec_enable)
    flush_dynarec_caches();
#endif

  instruction_count = 0;
  reg[OAM_UPDATED] = 1;

  return true;
}

bool gba_load_state(const void* src)
{
  bool ok;

  savestate_reader_begin_memory();
  ok = gba_load_state_core((const u8 *)src);
  savestate_reader_end();
  return ok;
}

bool gba_load_state_from_callback(gba_state_read_callback_t callback, void *context)
{
  bool ok;

  if (!callback) {
    return false;
  }

  savestate_reader_begin_callback(callback, context);
  ok = gba_load_state_core(savestate_offset_to_ptr(0));
  ok = ok && !s_state_reader.failed;
  savestate_reader_end();
  return ok;
}

void gba_save_state(void* dst)
{
  u8 *stptr = (u8*)dst;
  u8 *wrptr = (u8*)dst;

  savestate_writer_begin_memory();

  // Initial bson size
  bson_write_u32(wrptr, 0);

  // Add some info fields
  bson_write_int32(wrptr, "info-magic", GBA_STATE_MAGIC);
  bson_write_int32(wrptr, "info-version", GBA_STATE_VERSION);

  wrptr += cpu_write_savestate(wrptr);
  wrptr += input_write_savestate(wrptr);
  wrptr += main_write_savestate(wrptr);
  wrptr += memory_write_savestate(wrptr);
  wrptr += sound_write_savestate(wrptr);

  // The padding space is pushed into a padding field for easy parsing
  {
    unsigned padsize = GBA_STATE_MEM_SIZE - (unsigned)savestate_ptr_diff(wrptr, stptr);
    padsize -= 1 + 9 + 4 + 1 + 1;
    savestate_write_u8(&wrptr, 0x05);    // Byte array
    bson_write_cstring(wrptr, "zpadding");
    bson_write_u32(wrptr, padsize);
    savestate_write_u8(&wrptr, 0);
    wrptr += padsize;
  }

  savestate_write_u8(&wrptr, 0);

  // Update the doc size  
  savestate_patch_u32(stptr, (u32)savestate_ptr_diff(wrptr, stptr));
  savestate_writer_end();
}

bool gba_save_state_to_callback(gba_state_write_callback_t callback, void *context)
{
  u8 *stptr;
  u8 *wrptr;
  unsigned padsize;

  if (!callback) {
    return false;
  }

  savestate_writer_begin_callback(callback, context);

  stptr = savestate_offset_to_ptr(0);
  wrptr = stptr;

  bson_write_u32(wrptr, 0);
  bson_write_int32(wrptr, "info-magic", GBA_STATE_MAGIC);
  bson_write_int32(wrptr, "info-version", GBA_STATE_VERSION);

  wrptr += cpu_write_savestate(wrptr);
  wrptr += input_write_savestate(wrptr);
  wrptr += main_write_savestate(wrptr);
  wrptr += memory_write_savestate(wrptr);
  wrptr += sound_write_savestate(wrptr);

  padsize = GBA_STATE_MEM_SIZE - (unsigned)savestate_ptr_diff(wrptr, stptr);
  padsize -= 1 + 9 + 4 + 1 + 1;
  savestate_write_u8(&wrptr, 0x05);
  bson_write_cstring(wrptr, "zpadding");
  bson_write_u32(wrptr, padsize);
  savestate_write_u8(&wrptr, 0);
  wrptr = savestate_offset_to_ptr(savestate_ptr_to_offset(wrptr) + padsize);

  savestate_write_u8(&wrptr, 0);
  savestate_patch_u32(stptr, (u32)savestate_ptr_diff(wrptr, stptr));

  {
    bool ok = !s_state_writer.failed;
    savestate_writer_end();
    return ok;
  }
}


