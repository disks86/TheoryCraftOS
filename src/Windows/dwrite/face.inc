/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* dwrite.dll: the kernel's faces (NtTcosFont) as this DLL sees them: information, names, metrics, glyph boxes and kerning with caches, tables of the font file. */

#define MAX_FACES 4096
#define BOX_CACHE 2048
#define KERN_CACHE 2048

typedef struct BoxEntry { UINT32 tag; TcosGlyphBox box; } BoxEntry;      /* tag = glyph + 1, 0 = empty */
typedef struct KernEntry { UINT32 tag_left, tag_right; INT32 units; } KernEntry;

typedef struct FaceCache {
  int state;      /* 0 not asked yet, 1 usable, 2 the kernel has no such face */
  TcosFaceInfo info;
  DWRITE_FONT_METRICS metrics;
  WCHAR *names[4];      /* family, style, full, postscript (NULL until asked; "" if none) */
  BoxEntry *boxes;
  KernEntry *kerns;
  UINT32 table_count;
  struct TableEntry { UINT32 tag, offset, length; } *tables;
} FaceCache;

static FaceCache *g_faces[MAX_FACES];

#define TAG(a, b, c, d) ((UINT32)(a) | (UINT32)(b) << 8 | (UINT32)(c) << 16 | (UINT32)(d) << 24)      /* a tag as it is read from the file with a little endian load */

static UINT32 be32(const BYTE *p) { return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3]; }
static UINT32 be16(const BYTE *p) { return (UINT32)p[0] << 8 | p[1]; }

/* Bytes of the sfnt file of a face: returns the number copied (0 if the face has none). `*total` is the size of the file. */
static UINT32 face_read(UINT32 face, UINT32 offset, void *buffer, UINT32 size, UINT32 *total) {
  TcosFontRequest r;
  if (size > 0x100000) size = 0x100000;
  if (font_call(OP_FONT_DATA, face, offset, 0, 0, NULL, buffer, size, &r) != 0) return 0;
  if (total) *total = r.r0;
  return r.r1;
}

/* The table directory of the face (read once). Caller holds g_lock. */
static void face_load_directory(FaceCache *fc, UINT32 face) {
  if (fc->tables) return;
  BYTE head[12];
  UINT32 total = 0;
  if (face_read(face, 0, head, 12, &total) != 12) return;
  UINT32 base = 0;
  if (be32(head) == 0x74746366) {      /* 'ttcf': the first face of a collection */
    BYTE four[4];
    if (face_read(face, 12, four, 4, NULL) != 4) return;
    base = be32(four);
    if (face_read(face, base, head, 12, NULL) != 12) return;
  }
  const UINT32 count = be16(head + 4) > 64 ? 64 : be16(head + 4);
  BYTE *dir = mem_alloc(16 * (SIZE_T)count + 1);
  struct TableEntry *t = mem_array(count, sizeof(*t));
  if (!dir || !t || face_read(face, base + 12, dir, 16 * count, NULL) != 16 * count) { mem_free(dir); mem_free(t); return; }
  for (UINT32 i = 0; i < count; ++i) {
    t[i].tag = TAG(dir[16 * i], dir[16 * i + 1], dir[16 * i + 2], dir[16 * i + 3]);
    t[i].offset = be32(dir + 16 * i + 8);
    t[i].length = be32(dir + 16 * i + 12);
  }
  mem_free(dir);
  fc->tables = t;
  fc->table_count = count;
}

/* The table with tag `tag` (TAG(...)): its offset and length in the file. Caller holds g_lock. */
static int face_find_table(FaceCache *fc, UINT32 face, UINT32 tag, UINT32 *offset, UINT32 *length) {
  face_load_directory(fc, face);
  for (UINT32 i = 0; i < fc->table_count; ++i)
    if (fc->tables[i].tag == tag) { *offset = fc->tables[i].offset; *length = fc->tables[i].length; return 1; }
  return 0;
}

static WCHAR *face_fetch_name(UINT32 face, UINT32 which) {
  WCHAR buffer[256];
  TcosFontRequest r;
  NTSTATUS status = font_call(OP_FACE_STRING, face, which, 0, 0, NULL, buffer, sizeof(buffer), &r);
  if (status == 0) return wdup(buffer);
  if (status == STATUS_BUFFER_TOO_SMALL_ && r.r0 < 0x10000) {
    WCHAR *big = mem_array(r.r0 + 1, sizeof(WCHAR));
    if (big && font_call(OP_FACE_STRING, face, which, 0, 0, NULL, big, (r.r0 + 1) * 2, NULL) == 0) return big;
    mem_free(big);
  }
  return wdup(L"");
}

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The face's record, filled in on first use; NULL for a face the kernel does not have. Caller holds g_lock. */
static FaceCache *face_get(UINT32 face) {
  if (face >= MAX_FACES) return NULL;
  FaceCache *fc = g_faces[face];
  if (!fc) {
    fc = OBJECT_NEW(FaceCache);
    if (!fc) return NULL;
    g_faces[face] = fc;
  }
  if (fc->state == 0) {
    fc->state = 2;
    if (font_call(OP_FACE_INFO, face, 0, 0, 0, NULL, &fc->info, sizeof(fc->info), NULL) != 0 || fc->info.units_per_em == 0) return NULL;
    const TcosFaceInfo *i = &fc->info;
    DWRITE_FONT_METRICS *m = &fc->metrics;
    m->designUnitsPerEm = (UINT16)i->units_per_em;
    m->ascent = (UINT16)i->ascent;
    m->descent = (UINT16)i->descent;
    m->lineGap = (INT16)i->line_gap;
    m->capHeight = (UINT16)(i->cap_height ? i->cap_height : i->ascent * 7 / 10);
    m->xHeight = (UINT16)(i->x_height ? i->x_height : i->ascent / 2);
    /* the underline and strikeout measures come from the font's 'post' and 'OS/2' tables; a font without them gets Arial's proportions */
    m->underlinePosition = (INT16)-(INT32)(i->units_per_em * 106 / 1000);
    m->underlineThickness = (UINT16)(i->units_per_em * 73 / 1000);
    m->strikethroughPosition = (INT16)(m->xHeight / 2 + m->underlineThickness / 2);
    m->strikethroughThickness = m->underlineThickness;
    UINT32 offset, length;
    BYTE data[32];
    if (face_find_table(fc, face, TAG('p', 'o', 's', 't'), &offset, &length) && length >= 12 && face_read(face, offset, data, 12, NULL) == 12) {
      const INT16 position = (INT16)be16(data + 8), thickness = (INT16)be16(data + 10);
      if (thickness > 0) { m->underlinePosition = position; m->underlineThickness = (UINT16)thickness; }
    }
    if (face_find_table(fc, face, TAG('O', 'S', '/', '2'), &offset, &length) && length >= 30 && face_read(face, offset, data, 30, NULL) == 30) {
      const INT16 size = (INT16)be16(data + 26), position = (INT16)be16(data + 28);
      if (size > 0) { m->strikethroughThickness = (UINT16)size; m->strikethroughPosition = position; }
    }
    fc->state = 1;
  }
  return fc->state == 1 ? fc : NULL;
}

static int face_exists(UINT32 face) {
  EnterCriticalSection(&g_lock);
  const int ok = face_get(face) != NULL;
  LeaveCriticalSection(&g_lock);
  return ok;
}

/* Copies of face information (the cache itself may only be read with g_lock held). */
static int face_info(UINT32 face, TcosFaceInfo *info, DWRITE_FONT_METRICS *metrics) {
  EnterCriticalSection(&g_lock);
  FaceCache *fc = face_get(face);
  if (fc) {
    if (info) *info = fc->info;
    if (metrics) *metrics = fc->metrics;
  }
  LeaveCriticalSection(&g_lock);
  return fc != NULL;
}

/* A new copy of the face's name `which` (0 family, 1 style, 2 full, 3 PostScript). */
static WCHAR *face_name(UINT32 face, UINT32 which) {
  WCHAR *r = NULL;
  EnterCriticalSection(&g_lock);
  FaceCache *fc = face_get(face);
  if (fc && which < 4) {
    if (!fc->names[which]) fc->names[which] = face_fetch_name(face, which);
    r = fc->names[which] ? wdup(fc->names[which]) : NULL;
  }
  LeaveCriticalSection(&g_lock);
  return r;
}

/* Glyphs of code points. */
static int face_map(UINT32 face, const UINT32 *codepoints, UINT32 count, UINT16 *glyphs) {
  for (UINT32 done = 0; done < count;) {
    const UINT32 n = count - done > 65536 ? 65536 : count - done;
    if (font_call(OP_MAP_CODEPOINTS, face, 0, 0, n, codepoints + done, glyphs + done, n * 2, NULL) != 0) return 0;
    done += n;
  }
  return 1;
}

/* The design-unit advance and box of glyphs, cached. */
static int face_boxes(UINT32 face, const UINT16 *glyphs, UINT32 count, TcosGlyphBox *boxes) {
  EnterCriticalSection(&g_lock);
  FaceCache *fc = face_get(face);
  int ok = fc != NULL;
  if (ok && !fc->boxes) fc->boxes = mem_array(BOX_CACHE, sizeof(BoxEntry));
  ok = ok && fc->boxes;
  UINT16 *want = ok ? mem_array(count ? count : 1, 2) : NULL;
  TcosGlyphBox *got = ok ? mem_array(count ? count : 1, sizeof(TcosGlyphBox)) : NULL;
  ok = ok && want && got;
  UINT32 missing = 0;
  for (UINT32 i = 0; ok && i < count; ++i) {
    BoxEntry *e = &fc->boxes[glyphs[i] % BOX_CACHE];
    if (e->tag == (UINT32)glyphs[i] + 1) { boxes[i] = e->box; continue; }
    want[missing++] = glyphs[i];
  }
  if (ok && missing) {
    ok = font_call(OP_GLYPH_BOXES, face, 0, 0, missing, want, got, missing * (UINT32)sizeof(TcosGlyphBox), NULL) == 0;
    UINT32 next = 0;
    for (UINT32 i = 0; ok && i < count; ++i) {
      BoxEntry *e = &fc->boxes[glyphs[i] % BOX_CACHE];
      if (e->tag == (UINT32)glyphs[i] + 1) { boxes[i] = e->box; continue; }
      /* (the n-th miss is the n-th answer; a glyph that missed twice in one call is looked up twice, harmless) */
      boxes[i] = got[next++];
      e->tag = (UINT32)glyphs[i] + 1;
      e->box = boxes[i];
    }
  }
  mem_free(want);
  mem_free(got);
  LeaveCriticalSection(&g_lock);
  return ok;
}

/* Kerning in design units between two glyphs of a face, cached. */
static INT32 face_kerning(UINT32 face, UINT16 left, UINT16 right) {
  INT32 units = 0;
  EnterCriticalSection(&g_lock);
  FaceCache *fc = face_get(face);
  if (fc && !fc->kerns) fc->kerns = mem_array(KERN_CACHE, sizeof(KernEntry));
  if (fc && fc->kerns) {
    KernEntry *e = &fc->kerns[((UINT32)left * 31 + right) % KERN_CACHE];
    if (e->tag_left == (UINT32)left + 1 && e->tag_right == (UINT32)right + 1) units = e->units;
    else {
      TcosFontRequest r;
      if (font_call(OP_KERNING, face, left, right, 0, NULL, NULL, 0, &r) == 0) units = (INT32)r.r0;
      e->tag_left = (UINT32)left + 1;
      e->tag_right = (UINT32)right + 1;
      e->units = units;
    }
  }
  LeaveCriticalSection(&g_lock);
  return units;
}

static float units_to_em(UINT32 face, INT32 units, float size) {
  DWRITE_FONT_METRICS m;
  if (!face_info(face, NULL, &m) || m.designUnitsPerEm == 0) return 0;
  return (float)units * size / (float)m.designUnitsPerEm;
}

/* A string of the font's 'name' table (NULL when the font has none of that id): English (US) preferred, then any Windows or Unicode record, then Macintosh Roman. */
static WCHAR *face_name_record(UINT32 face, UINT32 name_id) {
  UINT32 offset, length;
  BYTE head[6];
  WCHAR *result = NULL;
  EnterCriticalSection(&g_lock);
  FaceCache *fc = face_get(face);
  const int found = fc && face_find_table(fc, face, TAG('n', 'a', 'm', 'e'), &offset, &length);
  LeaveCriticalSection(&g_lock);
  if (!found || length < 6 || face_read(face, offset, head, 6, NULL) != 6) return NULL;
  const UINT32 count = be16(head + 2) > 512 ? 512 : be16(head + 2), strings = be16(head + 4);
  if (6 + 12 * count > length) return NULL;
  BYTE *records = mem_alloc(12 * (SIZE_T)count + 1);
  if (!records || face_read(face, offset + 6, records, 12 * count, NULL) != 12 * count) { mem_free(records); return NULL; }
  int best = -1, best_score = 0;
  for (UINT32 i = 0; i < count; ++i) {
    const BYTE *r = records + 12 * i;
    if (be16(r + 6) != name_id) continue;
    const UINT32 platform = be16(r), language = be16(r + 4);
    const int score = platform == 3 && language == 0x409 ? 5 : platform == 3 ? 4 : platform == 0 ? 3 : platform == 1 && language == 0 ? 2 : platform == 1 ? 1 : 0;
    if (score > best_score) { best_score = score; best = (int)i; }
  }
  if (best >= 0) {
    const BYTE *r = records + 12 * best;
    const UINT32 platform = be16(r), len = be16(r + 8), at = strings + be16(r + 10);
    if (len > 0 && len <= 4096 && at <= length && len <= length - at) {
      BYTE *raw = mem_alloc(len + 2);
      if (raw && face_read(face, offset + at, raw, len, NULL) == len) {
        const int wide = platform == 3 || platform == 0;
        const UINT32 n = wide ? len / 2 : len;
        result = mem_array((SIZE_T)n + 1, sizeof(WCHAR));
        for (UINT32 i = 0; result && i < n; ++i) result[i] = wide ? (WCHAR)(raw[2 * i] << 8 | raw[2 * i + 1]) : (WCHAR)raw[i];
      }
      mem_free(raw);
    }
  }
  mem_free(records);
  return result;
}
