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

/* kernel32.dll: BeginUpdateResource, UpdateResource and EndUpdateResource - changing the resources of a PE file. The handle is a movable global block that holds (like
 * Windows') a pointer to a block with the file name; the resources wanted (the ones the file has, unless it is to be emptied, then the changes) are kept in memory and the
 * resource section is written at the end: in the section the file has (the sections after it move when it grows or shrinks), or in a new one at the end. */

typedef struct RsrcItem {
  WCHAR *type_name, *name_name;      /* a string (in capitals), or NULL when the id is meant */
  WORD type_id, name_id, lang;
  DWORD codepage, size;
  BYTE *data;
} RsrcItem;
typedef struct RsrcUpdate { WCHAR *path; RsrcItem *items; DWORD count, capacity; } RsrcUpdate;
typedef struct RsrcQueue { void *unknown[6]; HGLOBAL file_name; } RsrcQueue;      /* (the layout Windows gives the handle's block) */
#define RSRC_MAGIC 0x52535243u

static WCHAR *rsrc_key(LPCWSTR s, WORD *id) {
  *id = 0;
  if (((ULONG_PTR)s >> 16) == 0) { *id = (WORD)(ULONG_PTR)s; return NULL; }
  if (s[0] == '#') {      /* "#123" is the id 123 */
    DWORD v = 0;
    const WCHAR *c = s + 1;
    int ok = *c != 0;
    for (; *c; ++c) { if (*c < '0' || *c > '9' || v > 65535) { ok = 0; break; } v = v * 10 + (DWORD)(*c - '0'); }
    if (ok && v <= 65535) { *id = (WORD)v; return NULL; }
  }
  const SIZE_T n = wcslen(s);
  WCHAR *r = HeapAlloc(heap(), 0, (n + 1) * 2);
  if (!r) return NULL;
  for (SIZE_T i = 0; i <= n; ++i) r[i] = (WCHAR)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
  return r;
}
static int rsrc_wcscmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { ++a; ++b; } return *a < *b ? -1 : *a > *b; }
static int rsrc_same(const RsrcItem *a, const WCHAR *type_name, WORD type_id, const WCHAR *name_name, WORD name_id, WORD lang) {
  if (a->lang != lang) return 0;
  if ((a->type_name != NULL) != (type_name != NULL) || (a->name_name != NULL) != (name_name != NULL)) return 0;
  if (type_name ? rsrc_wcscmp(a->type_name, type_name) != 0 : a->type_id != type_id) return 0;
  if (name_name ? rsrc_wcscmp(a->name_name, name_name) != 0 : a->name_id != name_id) return 0;
  return 1;
}
static void rsrc_free_item(RsrcItem *it) {
  if (it->type_name) HeapFree(heap(), 0, it->type_name);
  if (it->name_name) HeapFree(heap(), 0, it->name_name);
  if (it->data) HeapFree(heap(), 0, it->data);
}
static void rsrc_free(RsrcUpdate *u) {
  if (!u) return;
  for (DWORD i = 0; i < u->count; ++i) rsrc_free_item(&u->items[i]);
  if (u->items) HeapFree(heap(), 0, u->items);
  if (u->path) HeapFree(heap(), 0, u->path);
  HeapFree(heap(), 0, u);
}

static DWORD rsrc_rd32(const BYTE *p) { DWORD v; memcpy(&v, p, 4); return v; }
static WORD rsrc_rd16(const BYTE *p) { WORD v; memcpy(&v, p, 2); return v; }
static void rsrc_wr32(BYTE *p, DWORD v) { memcpy(p, &v, 4); }
static void rsrc_wr16(BYTE *p, WORD v) { memcpy(p, &v, 2); }

/* The layout of a PE file's headers that the update needs (offsets into the file image). */
typedef struct PeInfo {
  SIZE_T nt, optional, sections;      /* the NT headers, the optional header, the section table */
  int is64;
  DWORD section_count, section_align, file_align, dir_count;
  SIZE_T dirs;      /* the data directories */
} PeInfo;
static int rsrc_pe_parse(const BYTE *f, SIZE_T size, PeInfo *pe) {
  if (size < 0x40 || f[0] != 'M' || f[1] != 'Z') return 0;
  const DWORD nt = rsrc_rd32(f + 0x3C);
  if (nt > size || size - nt < 24 || memcmp(f + nt, "PE\0\0", 4) != 0) return 0;
  pe->nt = nt;
  pe->section_count = rsrc_rd16(f + nt + 6);
  const DWORD optional_size = rsrc_rd16(f + nt + 20);
  pe->optional = nt + 24;
  if (optional_size > size - pe->optional || optional_size < 96) return 0;
  const WORD magic = rsrc_rd16(f + pe->optional);
  if (magic != 0x10b && magic != 0x20b) return 0;
  pe->is64 = magic == 0x20b;
  const SIZE_T fixed = pe->is64 ? 112 : 96;
  if (optional_size < fixed) return 0;
  pe->section_align = rsrc_rd32(f + pe->optional + 32);
  pe->file_align = rsrc_rd32(f + pe->optional + 36);
  pe->dir_count = rsrc_rd32(f + pe->optional + (pe->is64 ? 108 : 92));
  if (pe->dir_count > 16 || fixed + (SIZE_T)pe->dir_count * 8 > optional_size) pe->dir_count = pe->dir_count > 16 ? 16 : (DWORD)((optional_size - fixed) / 8);
  pe->dirs = pe->optional + fixed;
  pe->sections = pe->optional + optional_size;
  if (pe->section_count > 96 || (SIZE_T)pe->section_count * 40 > size - pe->sections) return 0;
  if (pe->section_align == 0 || (pe->section_align & (pe->section_align - 1)) || pe->file_align == 0 || (pe->file_align & (pe->file_align - 1))) return 0;
  return 1;
}
static DWORD rsrc_align(DWORD v, DWORD a) { return (v + a - 1) & ~(a - 1); }

/* The resources the file has, read into the update. */
static BOOL rsrc_load_existing(RsrcUpdate *u, const BYTE *f, SIZE_T size, const PeInfo *pe) {
  if (pe->dir_count <= 2) return TRUE;
  const DWORD rva = rsrc_rd32(f + pe->dirs + 16), dsize = rsrc_rd32(f + pe->dirs + 20);
  if (!rva || !dsize) return TRUE;
  /* the file offset of a relative address */
  #define RVA_TO_OFFSET(rva_, out_) do { out_ = 0; for (DWORD s_ = 0; s_ < pe->section_count; ++s_) { const BYTE *h_ = f + pe->sections + 40u * s_; \
      const DWORD va_ = rsrc_rd32(h_ + 12), vs_ = rsrc_rd32(h_ + 8), raw_ = rsrc_rd32(h_ + 16), ptr_ = rsrc_rd32(h_ + 20); \
      const DWORD span_ = vs_ > raw_ ? vs_ : raw_; if ((rva_) >= va_ && (rva_) - va_ < span_ && (rva_) - va_ < raw_) { out_ = (SIZE_T)ptr_ + ((rva_) - va_); break; } } } while (0)
  SIZE_T root;
  RVA_TO_OFFSET(rva, root);
  if (!root || root > size || size - root < 16) return TRUE;
  const BYTE *base = f + root;
  const SIZE_T room = size - root;
  const DWORD n1 = (DWORD)rsrc_rd16(base + 12) + rsrc_rd16(base + 14);
  for (DWORD i = 0; i < n1 && 16 + (SIZE_T)(i + 1) * 8 <= room; ++i) {
    const BYTE *e1 = base + 16 + (SIZE_T)i * 8;
    const DWORD id1 = rsrc_rd32(e1), off1 = rsrc_rd32(e1 + 4);
    if (!(off1 & 0x80000000u) || (off1 & 0x7FFFFFFF) > room - 16) continue;
    const BYTE *d2 = base + (off1 & 0x7FFFFFFF);
    const SIZE_T room2 = room - (SIZE_T)(d2 - base);
    const DWORD n2 = (DWORD)rsrc_rd16(d2 + 12) + rsrc_rd16(d2 + 14);
    for (DWORD j = 0; j < n2 && 16 + (SIZE_T)(j + 1) * 8 <= room2; ++j) {
      const BYTE *e2 = d2 + 16 + (SIZE_T)j * 8;
      const DWORD id2 = rsrc_rd32(e2), off2 = rsrc_rd32(e2 + 4);
      if (!(off2 & 0x80000000u) || (off2 & 0x7FFFFFFF) > room - 16) continue;
      const BYTE *d3 = base + (off2 & 0x7FFFFFFF);
      const SIZE_T room3 = room - (SIZE_T)(d3 - base);
      const DWORD n3 = (DWORD)rsrc_rd16(d3 + 12) + rsrc_rd16(d3 + 14);
      for (DWORD k = 0; k < n3 && 16 + (SIZE_T)(k + 1) * 8 <= room3; ++k) {
        const BYTE *e3 = d3 + 16 + (SIZE_T)k * 8;
        const DWORD id3 = rsrc_rd32(e3), off3 = rsrc_rd32(e3 + 4);
        if ((off3 & 0x80000000u) || off3 > room - 16) continue;
        const BYTE *de = base + off3;
        SIZE_T data_at;
        const DWORD data_rva = rsrc_rd32(de), data_size = rsrc_rd32(de + 4);
        RVA_TO_OFFSET(data_rva, data_at);
        if (!data_at || data_at > size || size - data_at < data_size) continue;
        if (u->count == u->capacity) {
          const DWORD cap = u->capacity ? u->capacity * 2 : 16;
          RsrcItem *n = HeapAlloc(heap(), HEAP_ZERO_MEMORY, (SIZE_T)cap * sizeof(RsrcItem));
          if (!n) return FALSE;
          if (u->items) { memcpy(n, u->items, (SIZE_T)u->count * sizeof(RsrcItem)); HeapFree(heap(), 0, u->items); }
          u->items = n;
          u->capacity = cap;
        }
        RsrcItem *it = &u->items[u->count];
        memset(it, 0, sizeof(*it));
        for (int pass = 0; pass < 2; ++pass) {
          const DWORD id = pass ? id2 : id1;
          if (id & 0x80000000u) {      /* a name: a length and the characters */
            const DWORD at = id & 0x7FFFFFFF;
            if (at > room - 2) goto skip;
            const WORD len = rsrc_rd16(base + at);
            if ((SIZE_T)len * 2 > room - at - 2) goto skip;
            WCHAR *s = HeapAlloc(heap(), 0, ((SIZE_T)len + 1) * 2);
            if (!s) return FALSE;
            memcpy(s, base + at + 2, (SIZE_T)len * 2);
            s[len] = 0;
            if (pass) it->name_name = s; else it->type_name = s;
          } else if (pass) it->name_id = (WORD)id; else it->type_id = (WORD)id;
        }
        it->lang = (WORD)id3;
        it->codepage = rsrc_rd32(de + 8);
        it->size = data_size;
        it->data = HeapAlloc(heap(), 0, data_size ? data_size : 1);
        if (!it->data) return FALSE;
        memcpy(it->data, f + data_at, data_size);
        u->count++;
        continue;
skip:
        rsrc_free_item(it);
        memset(it, 0, sizeof(*it));
      }
    }
  }
  #undef RVA_TO_OFFSET
  return TRUE;
}

/* Capitals first, strings sorted as upper case; then ids in order. */
static int rsrc_cmp_key(const WCHAR *an, WORD aid, const WCHAR *bn, WORD bid) {
  if (an && bn) return rsrc_wcscmp(an, bn);
  if (an) return -1;
  if (bn) return 1;
  return aid < bid ? -1 : aid > bid;
}
static int rsrc_cmp_item(const RsrcItem *a, const RsrcItem *b) {
  int c = rsrc_cmp_key(a->type_name, a->type_id, b->type_name, b->type_id);
  if (c) return c;
  c = rsrc_cmp_key(a->name_name, a->name_id, b->name_name, b->name_id);
  if (c) return c;
  return a->lang < b->lang ? -1 : a->lang > b->lang;
}
static void rsrc_sort(RsrcUpdate *u) {      /* (insertion: the lists are short) */
  for (DWORD i = 1; i < u->count; ++i) {
    RsrcItem t = u->items[i];
    DWORD j = i;
    while (j > 0 && rsrc_cmp_item(&u->items[j - 1], &t) > 0) { u->items[j] = u->items[j - 1]; --j; }
    u->items[j] = t;
  }
}

/* The resource section's contents for a section that starts at `rva`: directories of three levels, the data entries, the names, then the data. Returns the bytes (NULL on
 * failure) and their count. Every directory has version 4.0 and no time stamp. */
static BYTE *rsrc_build(RsrcUpdate *u, DWORD rva, DWORD *out_size, DWORD *named_top, DWORD *id_top) {
  rsrc_sort(u);
  /* count the directories: one root, one per type, one per (type, name) */
  DWORD types = 0, names = 0;
  for (DWORD i = 0; i < u->count; ++i) {
    const int new_type = i == 0 || rsrc_cmp_key(u->items[i - 1].type_name, u->items[i - 1].type_id, u->items[i].type_name, u->items[i].type_id) != 0;
    const int new_name = new_type || rsrc_cmp_key(u->items[i - 1].name_name, u->items[i - 1].name_id, u->items[i].name_name, u->items[i].name_id) != 0;
    types += new_type;
    names += new_name;
  }
  SIZE_T dir_bytes = 16 + (SIZE_T)types * 8 + (SIZE_T)types * 16 + (SIZE_T)names * 8 + (SIZE_T)names * 16 + (SIZE_T)u->count * 8;
  SIZE_T name_bytes = 0;
  for (DWORD i = 0; i < u->count; ++i) {
    if (u->items[i].type_name) name_bytes += 2 + wcslen(u->items[i].type_name) * 2;
    if (u->items[i].name_name) name_bytes += 2 + wcslen(u->items[i].name_name) * 2;
  }
  SIZE_T data_at = dir_bytes + (SIZE_T)u->count * 16 + name_bytes;
  data_at = (data_at + 3) & ~(SIZE_T)3;
  SIZE_T total = data_at;
  for (DWORD i = 0; i < u->count; ++i) {
    if (u->items[i].size > 0x10000000u - total) return NULL;
    total += (((SIZE_T)u->items[i].size + 3) & ~(SIZE_T)3);
  }
  if (total > 0x10000000u) return NULL;
  BYTE *out = HeapAlloc(heap(), HEAP_ZERO_MEMORY, total ? total : 1);
  if (!out) return NULL;
  SIZE_T dir_at = 0, entry_data = dir_bytes, name_at = dir_bytes + (SIZE_T)u->count * 16, data_pos = data_at;
  /* the root; the table of each level is written when its first entry is, after the ones above (a simple preorder over the sorted items) */
  SIZE_T next_dir = 16 + (SIZE_T)types * 8;      /* where the next directory goes */
  /* top level: how many entries are named */
  DWORD top_named = 0, top_ids = 0;
  for (DWORD i = 0; i < u->count; ++i) {
    if (i && rsrc_cmp_key(u->items[i - 1].type_name, u->items[i - 1].type_id, u->items[i].type_name, u->items[i].type_id) == 0) continue;
    if (u->items[i].type_name) top_named++; else top_ids++;
  }
  rsrc_wr32(out + dir_at + 8, 0);
  rsrc_wr16(out + 8, 4);      /* major version */
  rsrc_wr16(out + 12, (WORD)top_named);
  rsrc_wr16(out + 14, (WORD)top_ids);
  DWORD type_index = 0;
  for (DWORD i = 0; i < u->count;) {
    DWORD j = i;
    while (j < u->count && rsrc_cmp_key(u->items[i].type_name, u->items[i].type_id, u->items[j].type_name, u->items[j].type_id) == 0) ++j;
    /* the entry for the type in the root */
    BYTE *te = out + 16 + (SIZE_T)type_index * 8;
    if (u->items[i].type_name) {
      const SIZE_T l = wcslen(u->items[i].type_name);
      rsrc_wr32(te, 0x80000000u | (DWORD)name_at);
      rsrc_wr16(out + name_at, (WORD)l);
      memcpy(out + name_at + 2, u->items[i].type_name, l * 2);
      name_at += 2 + l * 2;
    } else rsrc_wr32(te, u->items[i].type_id);
    const SIZE_T type_dir = next_dir;
    rsrc_wr32(te + 4, 0x80000000u | (DWORD)type_dir);
    /* names under this type */
    DWORD names_here = 0, named_here = 0, ids_here = 0;
    for (DWORD k = i; k < j; ++k) {
      if (k > i && rsrc_cmp_key(u->items[k - 1].name_name, u->items[k - 1].name_id, u->items[k].name_name, u->items[k].name_id) == 0) continue;
      names_here++;
      if (u->items[k].name_name) named_here++; else ids_here++;
    }
    rsrc_wr16(out + type_dir + 8, 4);
    rsrc_wr16(out + type_dir + 12, (WORD)named_here);
    rsrc_wr16(out + type_dir + 14, (WORD)ids_here);
    next_dir = type_dir + 16 + (SIZE_T)names_here * 8;
    DWORD name_index = 0;
    for (DWORD k = i; k < j;) {
      DWORD m = k;
      while (m < j && rsrc_cmp_key(u->items[k].name_name, u->items[k].name_id, u->items[m].name_name, u->items[m].name_id) == 0) ++m;
      BYTE *ne = out + type_dir + 16 + (SIZE_T)name_index * 8;
      if (u->items[k].name_name) {
        const SIZE_T l = wcslen(u->items[k].name_name);
        rsrc_wr32(ne, 0x80000000u | (DWORD)name_at);
        rsrc_wr16(out + name_at, (WORD)l);
        memcpy(out + name_at + 2, u->items[k].name_name, l * 2);
        name_at += 2 + l * 2;
      } else rsrc_wr32(ne, u->items[k].name_id);
      const SIZE_T name_dir = next_dir;
      rsrc_wr32(ne + 4, 0x80000000u | (DWORD)name_dir);
      rsrc_wr16(out + name_dir + 8, 4);
      rsrc_wr16(out + name_dir + 14, (WORD)(m - k));
      next_dir = name_dir + 16 + (SIZE_T)(m - k) * 8;
      for (DWORD l2 = k; l2 < m; ++l2) {
        BYTE *le = out + name_dir + 16 + (SIZE_T)(l2 - k) * 8;
        rsrc_wr32(le, u->items[l2].lang);
        rsrc_wr32(le + 4, (DWORD)entry_data);
        BYTE *de = out + entry_data;
        rsrc_wr32(de, rva + (DWORD)data_pos);
        rsrc_wr32(de + 4, u->items[l2].size);
        rsrc_wr32(de + 8, u->items[l2].codepage);
        entry_data += 16;
        memcpy(out + data_pos, u->items[l2].data, u->items[l2].size);
        data_pos += ((SIZE_T)u->items[l2].size + 3) & ~(SIZE_T)3;
      }
      name_index++;
      k = m;
    }
    type_index++;
    i = j;
  }
  *out_size = (DWORD)total;
  *named_top = top_named;
  *id_top = top_ids;
  return out;
}

/* Rewrites the file's image in memory with the resource section made from u. */
static BYTE *rsrc_rewrite(RsrcUpdate *u, const BYTE *f, SIZE_T size, SIZE_T *out_size) {
  PeInfo pe;
  if (!rsrc_pe_parse(f, size, &pe)) { set_error(193); return NULL; }
  if (pe.dir_count <= 2) { set_error(193); return NULL; }
  const DWORD rsrc_rva = rsrc_rd32(f + pe.dirs + 16);
  int rs = -1;
  for (DWORD s = 0; s < pe.section_count && rs < 0; ++s) {
    const BYTE *h = f + pe.sections + 40u * s;
    const DWORD va = rsrc_rd32(h + 12), vs = rsrc_rd32(h + 8), raw = rsrc_rd32(h + 16);
    const DWORD span = vs > raw ? vs : raw;
    if (rsrc_rva && rsrc_rva >= va && rsrc_rva - va < span) rs = (int)s;
  }
  if (rs < 0) for (DWORD s = 0; s < pe.section_count && rs < 0; ++s) if (memcmp(f + pe.sections + 40u * s, ".rsrc\0\0\0", 8) == 0) rs = (int)s;
  DWORD va, old_ptr = 0, old_raw = 0, old_span = 0;
  if (rs >= 0) {
    const BYTE *h = f + pe.sections + 40u * (DWORD)rs;
    va = rsrc_rd32(h + 12);
    const DWORD vs = rsrc_rd32(h + 8);
    old_raw = rsrc_rd32(h + 16);
    old_ptr = rsrc_rd32(h + 20);
    old_span = rsrc_align(vs > old_raw ? vs : old_raw, pe.section_align);
  } else {      /* a new section after the last: it needs room in the headers */
    DWORD end_va = 0, first_ptr = 0xFFFFFFFFu;
    for (DWORD s = 0; s < pe.section_count; ++s) {
      const BYTE *h = f + pe.sections + 40u * s;
      const DWORD hv = rsrc_rd32(h + 12), vs = rsrc_rd32(h + 8), raw = rsrc_rd32(h + 16), ptr = rsrc_rd32(h + 20);
      const DWORD e = hv + rsrc_align(vs > raw ? vs : raw, pe.section_align);
      if (e > end_va) end_va = e;
      if (raw && ptr < first_ptr) first_ptr = ptr;
    }
    if (pe.sections + 40u * (pe.section_count + 1) > first_ptr && pe.section_count) { set_error(193); return NULL; }
    va = rsrc_align(end_va ? end_va : pe.section_align, pe.section_align);
  }
  DWORD blob_size = 0, named = 0, ids = 0;
  BYTE *blob = rsrc_build(u, va, &blob_size, &named, &ids);
  if (!blob) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  const DWORD raw_size = rsrc_align(blob_size ? blob_size : 1, pe.file_align);
  const DWORD new_span = rsrc_align(blob_size ? blob_size : 1, pe.section_align);
  SIZE_T tail_at, tail_len;
  DWORD new_ptr;
  if (rs >= 0) {
    new_ptr = old_ptr;
    tail_at = (SIZE_T)old_ptr + old_raw;
    tail_len = tail_at < size ? size - tail_at : 0;
  } else {
    new_ptr = rsrc_align((DWORD)size, pe.file_align);
    tail_at = size;
    tail_len = 0;
  }
  if (old_ptr > size) { HeapFree(heap(), 0, blob); set_error(193); return NULL; }
  const SIZE_T total = (SIZE_T)new_ptr + raw_size + tail_len;
  if (total > 0x20000000u) { HeapFree(heap(), 0, blob); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  BYTE *out = HeapAlloc(heap(), HEAP_ZERO_MEMORY, total);
  if (!out) { HeapFree(heap(), 0, blob); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  memcpy(out, f, rs >= 0 ? old_ptr : size);
  memcpy(out + new_ptr, blob, blob_size);
  if (tail_len) memcpy(out + new_ptr + raw_size, f + tail_at, tail_len);
  HeapFree(heap(), 0, blob);
  /* the headers */
  const LONG64 shift_va = (LONG64)new_span - (LONG64)old_span, shift_ptr = (LONG64)raw_size - (LONG64)old_raw;
  if (rs >= 0) {
    BYTE *h = out + pe.sections + 40u * (DWORD)rs;
    const DWORD old_va = rsrc_rd32(h + 12);
    rsrc_wr32(h + 8, blob_size);
    rsrc_wr32(h + 16, raw_size);
    rsrc_wr32(h + 20, new_ptr);
    for (DWORD s = 0; s < pe.section_count; ++s) {      /* the sections after it move */
      BYTE *o = out + pe.sections + 40u * s;
      if ((int)s == rs) continue;
      if (rsrc_rd32(o + 12) > old_va) {
        rsrc_wr32(o + 12, (DWORD)((LONG64)rsrc_rd32(o + 12) + shift_va));
        if (rsrc_rd32(o + 20)) rsrc_wr32(o + 20, (DWORD)((LONG64)rsrc_rd32(o + 20) + shift_ptr));
      }
    }
    for (DWORD d = 0; d < pe.dir_count; ++d) {      /* the directories that point after it */
      BYTE *e = out + pe.dirs + 8u * d;
      if (d != 2 && rsrc_rd32(e) >= old_va + old_span && rsrc_rd32(e)) rsrc_wr32(e, (DWORD)((LONG64)rsrc_rd32(e) + shift_va));
    }
  } else {
    BYTE *h = out + pe.sections + 40u * pe.section_count;
    memcpy(h, ".rsrc\0\0\0", 8);
    rsrc_wr32(h + 8, blob_size);
    rsrc_wr32(h + 12, va);
    rsrc_wr32(h + 16, raw_size);
    rsrc_wr32(h + 20, new_ptr);
    rsrc_wr32(h + 36, 0x40000040u);      /* initialized data, readable */
    rsrc_wr16(out + pe.nt + 6, (WORD)(pe.section_count + 1));
  }
  /* the resource directory itself, the size of the image */
  rsrc_wr32(out + pe.dirs + 16, va);
  rsrc_wr32(out + pe.dirs + 20, blob_size);
  DWORD image_end = 0;
  const DWORD sections = rs >= 0 ? pe.section_count : pe.section_count + 1;
  for (DWORD s = 0; s < sections; ++s) {
    const BYTE *h = out + pe.sections + 40u * s;
    const DWORD hv = rsrc_rd32(h + 12), vs = rsrc_rd32(h + 8), raw = rsrc_rd32(h + 16);
    const DWORD e = hv + rsrc_align(vs > raw ? vs : raw, pe.section_align);
    if (e > image_end) image_end = e;
  }
  rsrc_wr32(out + pe.optional + 56, image_end);
  rsrc_wr32(out + pe.optional + 8, (DWORD)((LONG64)rsrc_rd32(out + pe.optional + 8) + (rs >= 0 ? shift_ptr : (LONG64)raw_size)));
  rsrc_wr32(out + pe.optional + 64, 0);      /* (no check sum) */
  *out_size = total;
  return out;
}

EXPORT HANDLE WINAPI BeginUpdateResourceW(LPCWSTR file, BOOL delete_existing) {
  if (!file) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = CreateFileW(file, 0x80000000u, 7, NULL, 3, 0x80, NULL);      /* (it has to be there) */
  if (h == INVALID_HANDLE_VALUE) return NULL;
  RsrcUpdate *u = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(*u));
  if (!u) { CloseHandle(h); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  const SIZE_T n = wcslen(file);
  u->path = HeapAlloc(heap(), 0, (n + 1) * 2);
  if (u->path) memcpy(u->path, file, (n + 1) * 2);
  BOOL ok = u->path != NULL;
  if (ok && !delete_existing) {      /* the resources the file has now (it has to be an image) */
    const DWORD len = GetFileSize(h, NULL);
    BYTE *data = len == INVALID_FILE_SIZE || len > 0x20000000u ? NULL : HeapAlloc(heap(), 0, (SIZE_T)len + 1);
    DWORD got = 0;
    PeInfo pe;
    if (!data || (len && (!ReadFile(h, data, len, &got, NULL) || got != len))) { ok = FALSE; if (data) set_error(GetLastError()); }
    else if (!rsrc_pe_parse(data, len, &pe)) { ok = FALSE; set_error(len ? 193 : 1006); }
    else ok = rsrc_load_existing(u, data, len, &pe);
    if (data) HeapFree(heap(), 0, data);
  }
  CloseHandle(h);
  HGLOBAL block = ok ? GlobalAlloc(0x42 /* GHND */, sizeof(RsrcQueue)) : NULL, name = ok ? GlobalAlloc(0x42, (n + 1) * 2) : NULL;
  RsrcQueue *q = block ? GlobalLock(block) : NULL;
  WCHAR *nm = name ? GlobalLock(name) : NULL;
  if (!q || !nm) {
    if (ok) set_error(ERROR_NOT_ENOUGH_MEMORY);
    if (q) GlobalUnlock(block);
    if (block) GlobalFree(block);
    if (nm) GlobalUnlock(name);
    if (name) GlobalFree(name);
    rsrc_free(u);
    return NULL;
  }
  memcpy(nm, file, (n + 1) * 2);
  GlobalUnlock(name);
  q->unknown[0] = u;
  q->unknown[1] = (void *)(ULONG_PTR)RSRC_MAGIC;
  q->file_name = name;
  GlobalUnlock(block);
  return block;
}
EXPORT HANDLE WINAPI BeginUpdateResourceA(LPCSTR file, BOOL delete_existing) {
  WCHAR *w = file ? to_wide(file) : NULL;
  if (!w) { set_error(file ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = BeginUpdateResourceW(w, delete_existing);
  const DWORD e = GetLastError();
  free_wide(w);
  if (!h) set_error(e);
  return h;
}

static RsrcUpdate *rsrc_of(HANDLE h, RsrcQueue **queue) {
  if (!h) return NULL;
  RsrcQueue *q = GlobalLock(h);
  if (!q) return NULL;
  *queue = q;
  if ((ULONG_PTR)q->unknown[1] != RSRC_MAGIC || !q->unknown[0]) { GlobalUnlock(h); return NULL; }
  return q->unknown[0];
}

EXPORT BOOL WINAPI UpdateResourceW(HANDLE h, LPCWSTR type, LPCWSTR name, WORD lang, LPVOID data, DWORD size) {
  RsrcQueue *q;
  RsrcUpdate *u = rsrc_of(h, &q);
  if (!u) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!type || !name) { GlobalUnlock(h); set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (data && size == 0) data = NULL;
  WORD tid, nid;
  WCHAR *tn = rsrc_key(type, &tid), *nn = rsrc_key(name, &nid);
  if (((ULONG_PTR)type >> 16 && !tn && type[0] != '#') || ((ULONG_PTR)name >> 16 && !nn && name[0] != '#')) goto nomem;
  for (DWORD i = 0; i < u->count; ++i) {
    if (!rsrc_same(&u->items[i], tn, tid, nn, nid, lang)) continue;
    rsrc_free_item(&u->items[i]);      /* replaced, or (without data) deleted */
    memmove(&u->items[i], &u->items[i + 1], (SIZE_T)(u->count - i - 1) * sizeof(RsrcItem));
    u->count--;
    break;
  }
  if (data) {
    if (u->count == u->capacity) {
      const DWORD cap = u->capacity ? u->capacity * 2 : 16;
      RsrcItem *n = HeapAlloc(heap(), HEAP_ZERO_MEMORY, (SIZE_T)cap * sizeof(RsrcItem));
      if (!n) goto nomem;
      if (u->items) { memcpy(n, u->items, (SIZE_T)u->count * sizeof(RsrcItem)); HeapFree(heap(), 0, u->items); }
      u->items = n;
      u->capacity = cap;
    }
    RsrcItem *it = &u->items[u->count];
    memset(it, 0, sizeof(*it));
    it->data = HeapAlloc(heap(), 0, size);
    if (!it->data) goto nomem;
    memcpy(it->data, data, size);
    it->size = size;
    it->type_name = tn; it->type_id = tid; it->name_name = nn; it->name_id = nid;
    it->lang = lang;
    it->codepage = 0;
    u->count++;
    tn = nn = NULL;
  }
  if (tn) HeapFree(heap(), 0, tn);
  if (nn) HeapFree(heap(), 0, nn);
  GlobalUnlock(h);
  return TRUE;
nomem:
  if (tn) HeapFree(heap(), 0, tn);
  if (nn) HeapFree(heap(), 0, nn);
  GlobalUnlock(h);
  set_error(ERROR_NOT_ENOUGH_MEMORY);
  return FALSE;
}
EXPORT BOOL WINAPI UpdateResourceA(HANDLE h, LPCSTR type, LPCSTR name, WORD lang, LPVOID data, DWORD size) {
  WCHAR *t = ((ULONG_PTR)type >> 16) ? to_wide(type) : (WCHAR *)type, *n = ((ULONG_PTR)name >> 16) ? to_wide(name) : (WCHAR *)name;
  BOOL r = FALSE;
  if (t && n) r = UpdateResourceW(h, t, n, lang, data, size);
  else set_error(ERROR_NOT_ENOUGH_MEMORY);
  const DWORD e = GetLastError();
  if (((ULONG_PTR)type >> 16) && t) free_wide(t);
  if (((ULONG_PTR)name >> 16) && n) free_wide(n);
  if (!r) set_error(e);
  return r;
}

EXPORT BOOL WINAPI EndUpdateResourceW(HANDLE h, BOOL discard) {
  RsrcQueue *q;
  RsrcUpdate *u = rsrc_of(h, &q);
  if (!u) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  BOOL ok = TRUE;
  DWORD error = 0;
  if (!discard) {
    HANDLE f = CreateFileW(u->path, 0xC0000000u, 0, NULL, 3, 0x80, NULL);
    if (f == INVALID_HANDLE_VALUE) { ok = FALSE; error = GetLastError(); }
    else {
      const DWORD len = GetFileSize(f, NULL);
      BYTE *data = len == INVALID_FILE_SIZE || len > 0x20000000u ? NULL : HeapAlloc(heap(), 0, (SIZE_T)len + 1);
      DWORD got = 0;
      if (!data || (len && (!ReadFile(f, data, len, &got, NULL) || got != len))) { ok = FALSE; error = data ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY; }
      else {
        SIZE_T out_size = 0;
        set_error(0);
        BYTE *out = rsrc_rewrite(u, data, len, &out_size);
        if (!out) { ok = FALSE; error = GetLastError(); }
        else {
          DWORD written = 0;
          if (SetFilePointer(f, 0, NULL, 0) == INVALID_SET_FILE_POINTER || !WriteFile(f, out, (DWORD)out_size, &written, NULL) || written != out_size || !SetEndOfFile(f)) {
            ok = FALSE;
            error = GetLastError();
          }
          HeapFree(heap(), 0, out);
        }
      }
      if (data) HeapFree(heap(), 0, data);
      CloseHandle(f);
    }
  }
  HGLOBAL name = q->file_name;
  rsrc_free(u);
  q->unknown[0] = NULL;
  GlobalUnlock(h);
  if (name) GlobalFree(name);
  GlobalFree(h);
  if (!ok) set_error(error ? error : 193);
  return ok;
}
EXPORT BOOL WINAPI EndUpdateResourceA(HANDLE h, BOOL discard) { return EndUpdateResourceW(h, discard); }
