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

/* Dynamic shader linkage of Shader Model 5 (HLSL interfaces and classes) for d3d11.dll, as a driver does it: the shader is specialised for the class
 * instances bound with it, into a shader with no interfaces that the kernel compiles like any other.
 *
 * What a shader with interfaces is (the DXBC that fxc writes):
 *   - the IFCE part: the class types (name, the constant vectors, textures and samplers one instance takes), the class instances the shader declares
 *     (name, type, where its data is) and the interface slots: for each slot, which function table each class type that may be bound there calls through;
 *   - in the program: dcl_function_body fb#, dcl_function_table ft# = {fb ...} (a table's bodies by call site), dcl_interface fp#[array][call sites] =
 *     {ft ...} (fp0's slots come first, then fp1's ...), `fcall fp#[index][call site]` (index immediate, or a register for an interface array declared
 *     dynamically indexed), and after the main program the bodies, each from its `label fb#` to the next. A body reads its instance's data through the
 *     `this` register of its slot: this[slot] = (constant buffer, offset in it in vectors, first texture, first sampler) - the four numbers of
 *     D3D11_CLASS_INSTANCE_DESC - typically moved into temporaries and used as the indices of cb[][], t[] and s[].
 *
 * The specialised shader (dl_specialise): each fcall is replaced by the body its slot's instance calls, inlined (a call through an array indexed by a
 * register by a chain of `ieq` / `if_nz`, one branch for each element); `this` becomes the immediate values of the instance; an index that is a temporary
 * component known to hold a constant at that point (moved there from `this` or a literal, not written since on any path) becomes that constant, so
 * cb[r1.x + 0][r0.w + 1] is cb[0][4] when the instance's data is at cb0[3]. A body that returns from inside it is wrapped in a loop that runs once (its
 * returns: breaks, through a flag out of loops inside it). The declarations of bodies, tables and interfaces and the IFCE part are left out; the shader
 * gets DL_DEPTH more temporaries for the flags and the index of a chain. Slots with no instance bound call nothing (the outputs of the call are left as
 * they were: Direct3D says the result is undefined).
 *
 * Everything read is checked: a container, an IFCE part or a program that is cut short or wrong gives DL_INVALID, never a read outside the bytes given.
 * DL_UNSUPPORTED: what this cannot do - an index left a register (not a constant there) where the kernel needs a constant is not caught here but by the
 * kernel's compiler; here: more than the limits below, a class type bound where the slot does not take it. Portable C (d3d11.dll, and the host test
 * tests/DxbcLinkTest.cpp, which compiles the result with the kernel's DXBC front end and runs it). */
#ifndef TCOS_DXBC_LINK_H
#define TCOS_DXBC_LINK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define DL_OK 0
#define DL_INVALID 1
#define DL_UNSUPPORTED 2

#define DL_NAME 64u
#define DL_MAX_TYPES 64u
#define DL_MAX_INSTANCES 256u
#define DL_MAX_SLOTS 253u                 /* D3D11_SHADER_MAX_INTERFACES */
#define DL_MAX_ENTRIES 2048u              /* (slot, class type) pairs */
#define DL_MAX_BODIES 1024u
#define DL_MAX_TABLES 1024u
#define DL_MAX_TABLE_WORDS 4096u          /* the bodies of all tables */
#define DL_MAX_INTERFACES 253u
#define DL_MAX_WORDS 16384u               /* the program words looked at */
#define DL_MAX_INSTRUCTIONS 16384u
#define DL_OUT_WORDS 65536u               /* the specialised program */
#define DL_OUT_BYTES (DL_OUT_WORDS * 4u + 65536u)
#define DL_DEPTH 40u                      /* calls inside bodies inside ... (and subroutines of call / callc: 32 deep in Direct3D) */
#define DL_TEMPS 256u                     /* the temporaries tracked (what the kernel's compiler has) */
#define DL_FRAMES 64u
#define DL_NONE 0xFFFFFFFFu
#define DL_MAX_SLOT_INDEX 128u           /* t# / s# / u# / cb# numbers tracked */
#define DL_MAX_DYNAMIC 3u                 /* indices left registers in one instruction */
#define DL_MAX_BRANCHES 256u              /* the copies of such an instruction */

typedef struct DlType {
  char name[DL_NAME];
  uint32_t cb_stride, textures, samplers;
} DlType;

/* A class instance the shader declares (an array of `count` of them: element n's data follows element n - 1's). cb, cb_offset, texture, sampler: 0xFFFF
 * where the instance has none. */
typedef struct DlInstance {
  char name[DL_NAME];
  uint32_t type, count, cb, cb_offset, texture, sampler;
} DlInstance;

typedef struct DlInfo {
  uint32_t type_count, instance_count, slot_count;
  DlType types[DL_MAX_TYPES];
  DlInstance instances[DL_MAX_INSTANCES];
  uint32_t slot_first[DL_MAX_SLOTS + 1];           /* the entries of slot s: slot_first[s] .. slot_first[s + 1] - 1 */
  uint32_t entry_type[DL_MAX_ENTRIES], entry_table[DL_MAX_ENTRIES];
} DlInfo;

/* What is bound to an interface slot: an instance of class type `type` (its number in DlInfo.types) and its `this` (constant buffer, offset, texture,
 * sampler). */
typedef struct DlBinding {
  int bound;
  uint32_t type;
  uint32_t this_values[4];
} DlBinding;

/* The working memory of dl_specialise (large: the caller allocates it). */
typedef struct DlWork {
  uint32_t in[DL_MAX_WORDS];                       /* the program's words (after the version and length) */
  const uint32_t *w;
  uint32_t words;
  int temps_declared;
  uint32_t ins_at[DL_MAX_INSTRUCTIONS], ins_count;
  uint32_t main_end;                               /* the first body's label (or ins_count) */
  uint32_t body_first[DL_MAX_BODIES], body_end[DL_MAX_BODIES];
  uint32_t sub_label[DL_MAX_BODIES], sub_first[DL_MAX_BODIES], sub_end[DL_MAX_BODIES], sub_count;   /* the subroutines of call / callc: label number, first instruction, end */
  uint32_t table_first[DL_MAX_TABLES], table_count[DL_MAX_TABLES], table_bodies[DL_MAX_TABLE_WORDS], table_words;
  uint32_t fp_array[DL_MAX_INTERFACES], fp_sites[DL_MAX_INTERFACES], fp_base[DL_MAX_INTERFACES], fp_declared[DL_MAX_INTERFACES];
  uint32_t temps, extra_base;
  uint32_t max_depth;                              /* the deepest calls written (their flags' temporaries) */
  uint32_t temps_at[8], temps_at_count;           /* where the counts of dcl_temps are in out */
  /* the slots the program declares, by kind (constant buffers, textures, samplers, unordered access views): what an index left a register can name */
  uint8_t declared[4][DL_MAX_SLOT_INDEX];
  /* while an instruction with such indices is written once for each value they can have: the operand tokens (where in the words), the dimension and the index given */
  const uint32_t *force_at[DL_MAX_DYNAMIC];
  uint32_t force_dim[DL_MAX_DYNAMIC], force_value[DL_MAX_DYNAMIC], force_count;
  const DlInfo *info;
  const DlBinding *bind;
  uint32_t bind_count;
  /* the constants known to be in temporaries: known[t] bit c, value[t][c] */
  uint8_t known[DL_TEMPS];
  uint32_t value[DL_TEMPS][4];
  struct {
    int kind;                                      /* 0 if, 1 switch, 2 loop */
    uint8_t saved[DL_TEMPS];
    uint8_t writes[DL_TEMPS];
  } frames[DL_FRAMES];
  uint32_t frame_count;
  uint32_t out[DL_OUT_WORDS], out_count;
  int status;
  uint8_t dxbc[DL_OUT_BYTES];
  size_t dxbc_size;
} DlWork;

/* ---- reading ------------------------------------------------------------------------------------------------------------------------------------------- */

static uint32_t dl_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void dl_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* The part `fourcc` of a DXBC container: its bytes; 0 when there is none, -1 when the container is malformed. */
static int dl_find_part(const uint8_t *dxbc, size_t size, const char *fourcc, const uint8_t **data, uint32_t *bytes) {
  if (!dxbc || size < 32 || memcmp(dxbc, "DXBC", 4) != 0) return -1;
  const uint32_t total = dl_le32(dxbc + 24), parts = dl_le32(dxbc + 28);
  if (total > size || total < 32 || parts > 64 || (size_t)parts * 4 > total - 32) return -1;
  int found = 0;
  for (uint32_t i = 0; i < parts; ++i) {      /* (every part is checked, not only those before the one looked for) */
    const uint32_t at = dl_le32(dxbc + 32 + 4 * i);
    if (at > total || total - at < 8) return -1;
    const uint32_t n = dl_le32(dxbc + at + 4);
    if (n > total - at - 8) return -1;
    if (!found && memcmp(dxbc + at, fourcc, 4) == 0) { *data = dxbc + at + 8; *bytes = n; found = 1; }
  }
  return found;
}

/* A NUL-terminated name at `at` of the part (at most DL_NAME - 1 characters). */
static int dl_name(const uint8_t *part, uint32_t bytes, uint32_t at, char *out) {
  for (uint32_t k = 0; k < DL_NAME; ++k) {
    if (at >= bytes || k == DL_NAME - 1) return 0;
    out[k] = (char)part[at];
    if (part[at] == 0) return 1;
    ++at;
  }
  return 0;
}

/* The IFCE part (classes, instances and interface slots) into `info`. DL_OK with slot_count 0 for a shader that has none. */
static int dl_read_interfaces(const uint8_t *dxbc, size_t size, DlInfo *info) {
  memset(info, 0, sizeof *info);
  const uint8_t *p = NULL;
  uint32_t n = 0;
  const int found = dl_find_part(dxbc, size, "IFCE", &p, &n);
  if (found < 0) return DL_INVALID;
  if (found == 0) return DL_OK;
  if (n < 36) return DL_INVALID;
  const uint32_t instances = dl_le32(p), types = dl_le32(p + 4), records = dl_le32(p + 8), slots = dl_le32(p + 12);
  const uint32_t instance_at = dl_le32(p + 16), type_at = dl_le32(p + 20), record_at = dl_le32(p + 24);
  if (instances > DL_MAX_INSTANCES || types > DL_MAX_TYPES || slots > DL_MAX_SLOTS || records > slots) return DL_UNSUPPORTED;
  /* the class types: name, (an unused half), the constant vectors, textures and samplers of one instance */
  if (type_at > n || (n - type_at) / 12 < types) return DL_INVALID;
  for (uint32_t i = 0; i < types; ++i) {
    const uint8_t *t = p + type_at + 12 * i;
    DlType *ty = &info->types[i];
    if (!dl_name(p, n, dl_le32(t), ty->name)) return DL_INVALID;
    ty->cb_stride = (uint32_t)t[6] | (uint32_t)t[7] << 8;
    ty->textures = (uint32_t)t[8] | (uint32_t)t[9] << 8;
    ty->samplers = (uint32_t)t[10] | (uint32_t)t[11] << 8;
  }
  info->type_count = types;
  /* the instances: name, type, count, constant buffer, offset, texture, sampler */
  if (instance_at > n || (n - instance_at) / 16 < instances) return DL_INVALID;
  for (uint32_t i = 0; i < instances; ++i) {
    const uint8_t *t = p + instance_at + 16 * i;
    DlInstance *in = &info->instances[i];
    if (!dl_name(p, n, dl_le32(t), in->name)) return DL_INVALID;
    in->type = (uint32_t)t[4] | (uint32_t)t[5] << 8;
    in->count = (uint32_t)t[6] | (uint32_t)t[7] << 8;
    in->cb = (uint32_t)t[8] | (uint32_t)t[9] << 8;
    in->cb_offset = (uint32_t)t[10] | (uint32_t)t[11] << 8;
    in->texture = (uint32_t)t[12] | (uint32_t)t[13] << 8;
    in->sampler = (uint32_t)t[14] | (uint32_t)t[15] << 8;
    if (in->type >= types || in->count == 0) return DL_INVALID;
  }
  info->instance_count = instances;
  /* the slot records: the slots they cover (one after the other), the class types they take, and the table each calls through */
  if (record_at > n || (n - record_at) / 16 < records) return DL_INVALID;
  uint32_t slot = 0, entries = 0;
  for (uint32_t r = 0; r < records; ++r) {
    const uint8_t *t = p + record_at + 16 * r;
    const uint32_t covered = dl_le32(t), count = dl_le32(t + 4), type_list = dl_le32(t + 8), table_list = dl_le32(t + 12);
    if (covered == 0 || covered > slots - slot || count > types) return DL_INVALID;
    if (type_list > n || (n - type_list) / 2 < count || table_list > n || (n - table_list) / 4 < count) return DL_INVALID;
    for (uint32_t s = 0; s < covered; ++s) {
      info->slot_first[slot] = entries;
      if (count > DL_MAX_ENTRIES - entries) return DL_UNSUPPORTED;
      for (uint32_t k = 0; k < count; ++k) {
        const uint32_t ty = (uint32_t)p[type_list + 2 * k] | (uint32_t)p[type_list + 2 * k + 1] << 8;
        if (ty >= types) return DL_INVALID;
        info->entry_type[entries] = ty;
        info->entry_table[entries] = dl_le32(p + table_list + 4 * k);
        ++entries;
      }
      ++slot;
    }
  }
  if (slot != slots) return DL_INVALID;
  info->slot_first[slots] = entries;
  info->slot_count = slots;
  return DL_OK;
}

/* The class type named `name`; DL_NONE when the shader has none of that name. */
static uint32_t dl_type_named(const DlInfo *info, const char *name) {
  for (uint32_t i = 0; i < info->type_count; ++i)
    if (strcmp(info->types[i].name, name) == 0) return i;
  return DL_NONE;
}

/* ---- the program ------------------------------------------------------------------------------------------------------------------------------------- */

enum {
  DL_OP_BREAK = 2, DL_OP_BREAKC = 3, DL_OP_CALL = 4, DL_OP_CALLC = 5, DL_OP_CASE = 6, DL_OP_DEFAULT = 10, DL_OP_ELSE = 18, DL_OP_ENDIF = 21,
  DL_OP_ENDLOOP = 22, DL_OP_ENDSWITCH = 23, DL_OP_IF = 31, DL_OP_IEQ = 32, DL_OP_LABEL = 44, DL_OP_LOOP = 48, DL_OP_CUSTOMDATA = 53, DL_OP_MOV = 54,
  DL_OP_RET = 62, DL_OP_RETC = 63, DL_OP_SWITCH = 76, DL_OP_DCL_TEMPS = 104, DL_OP_FCALL = 120, DL_OP_DCL_FUNCTION_BODY = 144, DL_OP_DCL_FUNCTION_TABLE = 145,
  DL_OP_DCL_INTERFACE = 146,
};
enum { DL_TYPE_TEMP = 0, DL_TYPE_IMMEDIATE32 = 4, DL_TYPE_IMMEDIATE64 = 5, DL_TYPE_LABEL = 10, DL_TYPE_FUNCTION_BODY = 17, DL_TYPE_INTERFACE = 19, DL_TYPE_THIS = 29 };

/* Declarations: their words are copied as they are. */
static int dl_is_declaration(uint32_t op) { return (op >= 88 && op <= 106) || (op >= 143 && op <= 163) || op == 206 || op == DL_OP_CUSTOMDATA; }

/* The instruction at word `at`: its opcode and length (0 when it does not fit). */
static uint32_t dl_ins_length(const uint32_t *w, uint32_t words, uint32_t at) {
  if (at >= words) return 0;
  const uint32_t token = w[at];
  uint32_t length = (token >> 24) & 0x7F;
  if ((token & 0x7FF) == DL_OP_CUSTOMDATA) {
    if (words - at < 2) return 0;
    length = w[at + 1];
  }
  if (length == 0 || length > words - at) return 0;
  return length;
}

/* The words of the operand at `at` (no more than `end`); 0 when it does not fit. */
static uint32_t dl_operand_length(const uint32_t *w, uint32_t end, uint32_t at, uint32_t depth) {
  if (at >= end || depth > 4) return 0;
  const uint32_t t = w[at];
  uint32_t k = at + 1;
  for (uint32_t ext = t; ext >> 31;) {
    if (k >= end) return 0;
    ext = w[k++];
  }
  const uint32_t type = (t >> 12) & 0xFF, dims = (t >> 20) & 3, components = t & 3;
  if (dims > 3 || components == 3) return 0;
  for (uint32_t i = 0; i < dims; ++i) {
    const uint32_t rep = (t >> (22 + 3 * i)) & 7;
    if (rep == 0 || rep == 3) { if (k >= end) return 0; ++k; }
    else if (rep == 1 || rep == 4) { if (end - k < 2) return 0; k += 2; }
    else if (rep != 2) return 0;
    if (rep >= 2) {
      const uint32_t sub = dl_operand_length(w, end, k, depth + 1);
      if (!sub) return 0;
      k += sub;
    }
  }
  if (type == DL_TYPE_IMMEDIATE32 || type == DL_TYPE_IMMEDIATE64) {
    const uint32_t values = (components == 2 ? 4u : components == 1 ? 1u : 0u) * (type == DL_TYPE_IMMEDIATE64 ? 2u : 1u);
    if (end - k < values) return 0;
    k += values;
  }
  return k - at;
}

/* The component an operand of one component (select_1, or the first of a swizzle or mask) names. */
static uint32_t dl_first_component(uint32_t t) {
  const uint32_t mode = (t >> 2) & 3, bits = (t >> 4) & 0xFF;
  if (mode == 0) {
    for (uint32_t c = 0; c < 4; ++c) if (bits >> c & 1) return c;
    return 0;
  }
  return bits & 3;
}

/* The temporary components the operand at `at` writes when it is a destination (a temporary in mask mode), as a mask; 0 for none. */
static uint32_t dl_written(const uint32_t *w, uint32_t at, uint32_t *temp) {
  const uint32_t t = w[at];
  if (((t >> 12) & 0xFF) != DL_TYPE_TEMP || ((t >> 20) & 3) != 1 || ((t >> 22) & 7) != 0 || (t & 3) != 2 || ((t >> 2) & 3) != 0) return 0;
  uint32_t k = at + 1;
  for (uint32_t ext = t; ext >> 31;) ext = w[k++];
  *temp = w[k];
  return (t >> 4) & 0xF;
}

static void dl_out(DlWork *k, uint32_t v) {
  if (k->out_count >= DL_OUT_WORDS) { k->status = DL_UNSUPPORTED; return; }
  k->out[k->out_count++] = v;
}

/* ---- what is known ---------------------------------------------------------------------------------------------------------------------------------- */

static void dl_forget(DlWork *k, uint32_t temp, uint32_t mask) {
  if (temp >= DL_TEMPS) return;
  k->known[temp] &= (uint8_t)~mask;
  for (uint32_t f = 0; f < k->frame_count; ++f) k->frames[f].writes[temp] |= (uint8_t)mask;
}
static void dl_forget_all(DlWork *k) {
  for (uint32_t t = 0; t < DL_TEMPS; ++t) dl_forget(k, t, 0xF);
}
static void dl_push(DlWork *k, int kind) {
  if (k->frame_count >= DL_FRAMES) { k->status = DL_UNSUPPORTED; return; }
  k->frames[k->frame_count].kind = kind;
  memcpy(k->frames[k->frame_count].saved, k->known, sizeof k->known);
  memset(k->frames[k->frame_count].writes, 0, sizeof k->known);
  ++k->frame_count;
}
/* The known values as at the frame's start, less what was written in it. */
static void dl_restore(DlWork *k) {
  if (!k->frame_count) { k->status = DL_INVALID; return; }
  for (uint32_t t = 0; t < DL_TEMPS; ++t) k->known[t] = (uint8_t)(k->frames[k->frame_count - 1].saved[t] & ~k->frames[k->frame_count - 1].writes[t]);
}
static void dl_pop(DlWork *k) {
  dl_restore(k);
  if (k->frame_count) --k->frame_count;
}

/* An instruction as it is written out: what it does to the known values. `loop_writes`: a loop's own (what its body writes, which the back edge brings
 * to its start; NULL: a loop that runs once). */
static void dl_track(DlWork *k, const uint32_t *ins, uint32_t length, const uint8_t *loop_writes) {
  const uint32_t op = ins[0] & 0x7FF;
  if (dl_is_declaration(op)) return;
  switch (op) {
    case DL_OP_IF: dl_push(k, 0); return;
    case DL_OP_ELSE: dl_restore(k); return;
    case DL_OP_ENDIF: case DL_OP_ENDSWITCH: case DL_OP_ENDLOOP: dl_pop(k); return;
    case DL_OP_SWITCH: dl_push(k, 1); return;
    case DL_OP_CASE: case DL_OP_DEFAULT: dl_restore(k); return;
    case DL_OP_CALL: case DL_OP_CALLC: dl_forget_all(k); return;      /* (a subroutine may write any temporary) */
    case DL_OP_LOOP:
      if (loop_writes)
        for (uint32_t t = 0; t < DL_TEMPS; ++t) dl_forget(k, t, loop_writes[t]);
      dl_push(k, 2);
      return;
    default: break;
  }
  uint32_t at = 1;
  for (uint32_t ext = ins[0]; ext >> 31 && at < length;) ext = ins[at++];
  uint32_t first_dst = DL_NONE, first_mask = 0;
  while (at < length) {
    const uint32_t n = dl_operand_length(ins, length, at, 0);
    if (!n) { dl_forget_all(k); return; }
    uint32_t temp = 0;
    const uint32_t mask = dl_written(ins, at, &temp);
    if (mask) {
      if (first_dst == DL_NONE) first_dst = temp, first_mask = mask;
      dl_forget(k, temp, mask);
    }
    at += n;
  }
  /* mov rN.mask, l(...) or a known temporary: the components are known (not with saturate) */
  if (op == DL_OP_MOV && !((ins[0] >> 13) & 1) && first_dst != DL_NONE && first_dst < DL_TEMPS) {
    uint32_t src = 1;
    for (uint32_t ext = ins[0]; ext >> 31 && src < length;) ext = ins[src++];
    src += dl_operand_length(ins, length, src, 0);
    if (src >= length) return;
    const uint32_t t = ins[src];
    if (t >> 31) return;      /* (a modifier) */
    const uint32_t type = (t >> 12) & 0xFF, components = t & 3, mode = (t >> 2) & 3, bits = (t >> 4) & 0xFF;
    uint32_t values[4];
    uint8_t have = 0;
    if (type == DL_TYPE_IMMEDIATE32 && components == 1 && src + 1 < length) {
      for (uint32_t c = 0; c < 4; ++c) values[c] = ins[src + 1];
      have = 0xF;
    } else if (type == DL_TYPE_IMMEDIATE32 && components == 2 && length - src >= 5) {
      for (uint32_t c = 0; c < 4; ++c) values[c] = ins[src + 1 + c];
      have = 0xF;
    }
    if (type == DL_TYPE_TEMP && components == 2 && ((t >> 20) & 3) == 1 && ((t >> 22) & 7) == 0 && src + 1 < length && ins[src + 1] < DL_TEMPS) {
      /* (a copy of a temporary: the components known there) */
      const uint32_t from_temp = ins[src + 1];
      uint32_t from_values[4];
      for (uint32_t c = 0; c < 4; ++c) from_values[c] = k->value[from_temp][c];
      const uint8_t from_known = k->known[from_temp];
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(first_mask >> c & 1)) continue;
        const uint32_t from = mode == 0 ? c : mode == 1 ? (bits >> (2 * c)) & 3 : bits & 3;
        if (!(from_known >> from & 1)) continue;
        k->value[first_dst][c] = from_values[from];
        k->known[first_dst] |= (uint8_t)(1u << c);
      }
      return;
    }
    if (have != 0xF) return;
    for (uint32_t c = 0; c < 4; ++c) {
      if (!(first_mask >> c & 1)) continue;
      const uint32_t from = mode == 0 ? c : mode == 1 ? (bits >> (2 * c)) & 3 : bits & 3;
      k->value[first_dst][c] = values[from];
      k->known[first_dst] |= (uint8_t)(1u << c);
    }
  }
}

/* What a source range of instructions writes (for a loop's back edge): an fcall inside it, everything. */
static void dl_range_writes(const DlWork *k, uint32_t first, uint32_t end, uint8_t *writes) {
  memset(writes, 0, DL_TEMPS);
  for (uint32_t i = first; i < end; ++i) {
    const uint32_t at = k->ins_at[i], length = dl_ins_length(k->w, k->words, at);
    const uint32_t *ins = k->w + at;
    const uint32_t op = ins[0] & 0x7FF;
    if (op == DL_OP_FCALL || op == DL_OP_CALL || op == DL_OP_CALLC) { memset(writes, 0xF, DL_TEMPS); return; }
    if (dl_is_declaration(op)) continue;
    uint32_t o = 1;
    for (uint32_t ext = ins[0]; ext >> 31 && o < length;) ext = ins[o++];
    while (o < length) {
      const uint32_t n = dl_operand_length(ins, length, o, 0);
      if (!n) { memset(writes, 0xF, DL_TEMPS); return; }
      uint32_t temp = 0;
      const uint32_t mask = dl_written(ins, o, &temp);
      if (mask && temp < DL_TEMPS) writes[temp] |= (uint8_t)mask;
      o += n;
    }
  }
}

/* ---- writing ----------------------------------------------------------------------------------------------------------------------------------------- */

/* The operand at w[*at] written out: `this` as the immediate values of the call's instance, a relative index that is a known constant as that constant.
 * `self`: the this values of the body being written (NULL outside bodies: a `this` there is refused). */
static void dl_write_operand(DlWork *k, const uint32_t *w, uint32_t end, uint32_t *at, const uint32_t *self, uint32_t depth) {
  const uint32_t n = dl_operand_length(w, end, *at, 0);
  if (!n || depth > 4) { k->status = DL_INVALID; return; }
  const uint32_t t = w[*at];
  const uint32_t type = (t >> 12) & 0xFF, dims = (t >> 20) & 3;
  if (type == DL_TYPE_THIS) {
    if (!self) { k->status = DL_INVALID; return; }
    /* l(x, y, z, w) of the components the operand selects; a modifier stays with it */
    const uint32_t mode = (t >> 2) & 3, bits = (t >> 4) & 0xFF, components = t & 3;
    dl_out(k, 0x00004002u | (t & 0x80000000u));
    uint32_t o = *at + 1;
    for (uint32_t ext = t; ext >> 31;) { ext = w[o++]; dl_out(k, ext); }
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t from = components != 2 ? 0 : mode == 0 ? c : mode == 1 ? (bits >> (2 * c)) & 3 : bits & 3;
      dl_out(k, self[from]);
    }
    *at += n;
    return;
  }
  const uint32_t token_at = k->out_count;
  dl_out(k, t);
  uint32_t o = *at + 1;
  for (uint32_t ext = t; ext >> 31;) { ext = w[o++]; dl_out(k, ext); }
  uint32_t token = t;
  for (uint32_t i = 0; i < dims; ++i) {
    const uint32_t rep = (t >> (22 + 3 * i)) & 7;
    if (rep == 0) { dl_out(k, w[o++]); continue; }
    if (rep == 1) { dl_out(k, w[o]); dl_out(k, w[o + 1]); o += 2; continue; }
    const uint32_t imm = rep == 3 ? w[o] : 0;
    const uint32_t sub_at = rep == 3 ? o + 1 : rep == 4 ? o + 2 : o;
    const uint32_t s = w[sub_at];
    const uint32_t sub_n = dl_operand_length(w, end, sub_at, 0);
    /* a temporary's component known to be a constant, or `this` itself: the index is that constant */
    uint32_t constant = 0;
    int folded = 0;
    for (uint32_t f = 0; f < k->force_count; ++f)
      if (k->force_at[f] == w + *at && k->force_dim[f] == i) constant = k->force_value[f] - imm, folded = 1;
    if (!folded && rep != 4 && (s & 3) == 2 && ((s >> 20) & 3) == 1 && ((s >> 22) & 7) == 0 && !(s >> 31)) {
      const uint32_t c = dl_first_component(s), number = w[sub_at + 1];
      if (((s >> 12) & 0xFF) == DL_TYPE_TEMP && number < DL_TEMPS && (k->known[number] >> c & 1)) constant = k->value[number][c], folded = 1;
      else if (((s >> 12) & 0xFF) == DL_TYPE_THIS && self) constant = self[c], folded = 1;
    }
    if (folded) {
      token &= ~(7u << (22 + 3 * i));
      dl_out(k, imm + constant);
      o = sub_at + sub_n;
      continue;
    }
    if (rep == 3) dl_out(k, w[o]);
    if (rep == 4) { dl_out(k, w[o]); dl_out(k, w[o + 1]); }
    uint32_t sub = sub_at;
    dl_write_operand(k, w, end, &sub, self, depth + 1);
    o = sub;
  }
  while (o < *at + n) dl_out(k, w[o++]);      /* (an immediate's values) */
  if (token_at < k->out_count) k->out[token_at] = token;
  *at += n;
}

/* Ends an instruction written from out[start]: its length in the token, and what it does to the known values. */
static void dl_finish(DlWork *k, uint32_t start, const uint8_t *loop_writes) {
  if (k->status != DL_OK) return;
  const uint32_t length = k->out_count - start;
  if (length > 127) { k->status = DL_UNSUPPORTED; return; }
  k->out[start] = (k->out[start] & ~(0x7Fu << 24)) | length << 24;
  dl_track(k, k->out + start, length, loop_writes);
}

/* Instructions made here: one word, or with an operand of one temporary component. */
static void dl_emit0(DlWork *k, uint32_t op, const uint8_t *loop_writes) {
  const uint32_t start = k->out_count;
  dl_out(k, op | 1u << 24);
  dl_finish(k, start, loop_writes);
}
static uint32_t dl_temp_select(uint32_t c) { return 0x0010000Au | c << 4; }         /* rN.c (select_1) */
static uint32_t dl_temp_mask(uint32_t c) { return 0x00100002u | (1u << c) << 4; }  /* rN.c (a destination) */
/* if_nz / breakc_nz rT.c */
static void dl_emit_test(DlWork *k, uint32_t op, uint32_t temp, uint32_t c) {
  const uint32_t start = k->out_count;
  dl_out(k, op | 1u << 18);
  dl_out(k, dl_temp_select(c));
  dl_out(k, temp);
  dl_finish(k, start, NULL);
}
/* mov rT.c, l(v) */
static void dl_emit_set(DlWork *k, uint32_t temp, uint32_t c, uint32_t v) {
  const uint32_t start = k->out_count;
  dl_out(k, DL_OP_MOV);
  dl_out(k, dl_temp_mask(c));
  dl_out(k, temp);
  dl_out(k, 0x00004001u);
  dl_out(k, v);
  dl_finish(k, start, NULL);
}

static void dl_write_range(DlWork *k, uint32_t first, uint32_t end, const uint32_t *self, uint32_t depth, int sub);

/* The count of a dcl_temps: written when the program is done (the program's own, the one for indices left registers, one for each depth of calls used). */
static void dl_temps_count(DlWork *k) {
  if (k->temps_at_count >= 8) { k->status = DL_UNSUPPORTED; return; }
  k->temps_at[k->temps_at_count++] = k->out_count;
  dl_out(k, 0);
}

/* ---- indices left registers ---------------------------------------------------------------------------------------------------------------------- */

/* The kind of slot an operand type names when its first index is a slot number the kernel needs as a constant: 0 constant buffer (cb#[...]), 1 texture,
 * 2 sampler, 3 unordered access view; -1 for the others (a relative index the kernel takes: the element of a constant buffer, the immediate buffer, ...). */
static int dl_slot_kind(uint32_t type, uint32_t dims) {
  if (type == 8 && dims == 2) return 0;
  if (type == 7 && dims == 1) return 1;
  if (type == 6 && dims == 1) return 2;
  if (type == 30 && dims == 1) return 3;
  return -1;
}

typedef struct DlDynamic {
  const uint32_t *token;     /* the operand */
  uint32_t kind;             /* dl_slot_kind */
  uint32_t imm;              /* the index is imm + the register's component */
  uint32_t temp, component;  /* (-1 temp: `this`, not possible here) */
} DlDynamic;

/* The operands of an instruction whose slot index is a register that is not known to hold a constant there (DL_MAX_DYNAMIC at most: more is DL_UNSUPPORTED). */
static uint32_t dl_dynamic_operands(DlWork *k, const uint32_t *ins, uint32_t length, const uint32_t *self, DlDynamic *out) {
  uint32_t n = 0, at = 1;
  for (uint32_t ext = ins[0]; ext >> 31 && at < length;) ext = ins[at++];
  while (at < length) {
    const uint32_t words = dl_operand_length(ins, length, at, 0);
    if (!words) return 0;
    const uint32_t t = ins[at];
    const int kind = dl_slot_kind((t >> 12) & 0xFF, (t >> 20) & 3);
    const uint32_t rep = (t >> 22) & 7;
    if (kind >= 0 && (rep == 2 || rep == 3)) {
      uint32_t o = at + 1;
      for (uint32_t ext = t; ext >> 31 && o < length;) ext = ins[o++];
      const uint32_t imm = rep == 3 ? ins[o] : 0, sub_at = rep == 3 ? o + 1 : o;
      const uint32_t sub = sub_at < length ? ins[sub_at] : 0;
      int known = 0;
      if (sub_at + 1 < length && (sub & 3) == 2 && ((sub >> 20) & 3) == 1 && ((sub >> 22) & 7) == 0 && !(sub >> 31)) {
        const uint32_t c = dl_first_component(sub), number = ins[sub_at + 1], type = (sub >> 12) & 0xFF;
        if (type == DL_TYPE_THIS && self) known = 1;
        else if (type == DL_TYPE_TEMP && number < DL_TEMPS && (k->known[number] >> c & 1)) known = 1;
        else if (type == DL_TYPE_TEMP && number < DL_TEMPS && dl_operand_length(ins, length, sub_at, 0) == 2) {
          if (n >= DL_MAX_DYNAMIC) { k->status = DL_UNSUPPORTED; return 0; }
          out[n].token = ins + at, out[n].kind = (uint32_t)kind, out[n].imm = imm, out[n].temp = number, out[n].component = c;
          ++n;
          known = 1;
        }
      }
      if (!known) { k->status = DL_UNSUPPORTED; return 0; }      /* (an index the kernel cannot take and nothing here can make one) */
    }
    at += words;
  }
  return n;
}

/* The instruction (from the source words) written once for every combination of the values its dynamic slot indices can have (the slots the program
 * declares), each inside `if` the copied registers are those values. The registers are copied first (rX.x, .y, .w: the instruction may write them); rX.z
 * is the test. An index that names no declared slot leaves the instruction out (Direct3D says such an index is undefined). */
static void dl_write_instruction(DlWork *k, const uint32_t *ins, uint32_t length, const uint32_t *self, const uint8_t *loop_writes);
static void dl_write_branches(DlWork *k, const uint32_t *ins, uint32_t length, const uint32_t *self, const DlDynamic *d, uint32_t count, uint32_t level) {
  static const uint32_t copy_component[DL_MAX_DYNAMIC] = {0, 1, 3};
  const uint32_t x = k->extra_base;
  if (level == count) {
    dl_write_instruction(k, ins, length, self, NULL);
    return;
  }
  for (uint32_t v = 0; v < DL_MAX_SLOT_INDEX && k->status == DL_OK; ++v) {
    if (!k->declared[d[level].kind][v]) continue;
    const uint32_t start = k->out_count;
    dl_out(k, DL_OP_IEQ);
    dl_out(k, dl_temp_mask(2));
    dl_out(k, x);
    dl_out(k, dl_temp_select(copy_component[level]));
    dl_out(k, x);
    dl_out(k, 0x00004001u);
    dl_out(k, v - d[level].imm);
    dl_finish(k, start, NULL);
    dl_emit_test(k, DL_OP_IF, x, 2);
    k->force_at[k->force_count] = d[level].token;
    k->force_dim[k->force_count] = 0;
    k->force_value[k->force_count] = v;
    ++k->force_count;
    dl_write_branches(k, ins, length, self, d, count, level + 1);
    --k->force_count;
    dl_emit0(k, DL_OP_ENDIF, NULL);
  }
}

/* An instruction of the source written out (its operands through dl_write_operand). */
static void dl_write_instruction(DlWork *k, const uint32_t *ins, uint32_t length, const uint32_t *self, const uint8_t *loop_writes) {
  const uint32_t start = k->out_count;
  uint32_t o = 1;
  for (uint32_t ext = ins[0]; ext >> 31 && o < length;) ext = ins[o++];
  for (uint32_t n = 0; n < o; ++n) dl_out(k, ins[n]);
  while (o < length && k->status == DL_OK) dl_write_operand(k, ins, length, &o, self, 0);
  dl_finish(k, start, loop_writes);
}


/* A call through slot `slot`, call site `site`: the body its instance calls, inlined. */
static void dl_call_slot(DlWork *k, uint32_t fp, uint32_t slot, uint32_t site, uint32_t depth) {
  if (slot >= k->info->slot_count) { k->status = DL_INVALID; return; }
  if (slot >= k->bind_count || !k->bind[slot].bound) return;      /* (nothing bound: nothing is called) */
  const DlBinding *b = &k->bind[slot];
  uint32_t table = DL_NONE;
  for (uint32_t e = k->info->slot_first[slot]; e < k->info->slot_first[slot + 1]; ++e)
    if (k->info->entry_type[e] == b->type) table = k->info->entry_table[e];
  if (table == DL_NONE) { k->status = DL_UNSUPPORTED; return; }      /* (a class the slot does not take) */
  (void)fp;
  if (table >= DL_MAX_TABLES || site >= k->table_count[table]) { k->status = DL_INVALID; return; }
  const uint32_t body = k->table_bodies[k->table_first[table] + site];
  if (body >= DL_MAX_BODIES || k->body_first[body] == DL_NONE) { k->status = DL_INVALID; return; }
  if (depth >= DL_DEPTH) { k->status = DL_UNSUPPORTED; return; }
  dl_write_range(k, k->body_first[body], k->body_end[body], b->this_values, depth + 1, 0);
}

/* fcall fp#[index][site] */
static void dl_write_fcall(DlWork *k, const uint32_t *ins, uint32_t length, uint32_t depth) {
  if (length < 4) { k->status = DL_INVALID; return; }
  uint32_t at = 1;
  for (uint32_t ext = ins[0]; ext >> 31 && at < length;) ext = ins[at++];
  if (at >= length) { k->status = DL_INVALID; return; }
  const uint32_t site = ins[at++];
  if (at >= length) { k->status = DL_INVALID; return; }
  const uint32_t t = ins[at];
  if (((t >> 12) & 0xFF) != DL_TYPE_INTERFACE || ((t >> 20) & 3) != 2 || (t >> 31) || ((t >> 22) & 7) != 0 || length - at < 3) { k->status = DL_INVALID; return; }
  const uint32_t fp = ins[at + 1];
  if (fp >= DL_MAX_INTERFACES || !k->fp_declared[fp] || site >= k->fp_sites[fp]) { k->status = DL_INVALID; return; }
  const uint32_t rep = (t >> 25) & 7;
  if (rep == 0) {
    const uint32_t index = ins[at + 2];
    if (index >= k->fp_array[fp]) { k->status = DL_INVALID; return; }
    dl_call_slot(k, fp, k->fp_base[fp] + index, site, depth);
    return;
  }
  if (rep != 2 && rep != 3) { k->status = DL_UNSUPPORTED; return; }
  /* an array indexed by a register: known there, or one branch for each element (the index copied first: a body may write the register) */
  if (rep == 3 && length - at < 4) { k->status = DL_INVALID; return; }
  const uint32_t imm = rep == 3 ? ins[at + 2] : 0, sub_at = rep == 3 ? at + 3 : at + 2;
  if (sub_at >= length || dl_operand_length(ins, length, sub_at, 0) != 2) { k->status = DL_UNSUPPORTED; return; }
  const uint32_t s = ins[sub_at], number = ins[sub_at + 1];
  if (((s >> 12) & 0xFF) != DL_TYPE_TEMP || (s & 3) != 2 || ((s >> 20) & 3) != 1 || ((s >> 22) & 7) != 0) { k->status = DL_UNSUPPORTED; return; }
  const uint32_t c = dl_first_component(s);
  if (number < DL_TEMPS && (k->known[number] >> c & 1)) {
    const uint32_t index = imm + k->value[number][c];
    if (index < k->fp_array[fp]) dl_call_slot(k, fp, k->fp_base[fp] + index, site, depth);
    return;      /* (outside the array: undefined, nothing is called) */
  }
  const uint32_t scratch = k->extra_base + 1 + depth;
  {
    const uint32_t start = k->out_count;
    dl_out(k, DL_OP_MOV);
    dl_out(k, dl_temp_mask(2));
    dl_out(k, scratch);
    dl_out(k, s);
    dl_out(k, number);
    dl_finish(k, start, NULL);
  }
  for (uint32_t e = 0; e < k->fp_array[fp] && k->status == DL_OK; ++e) {
    const uint32_t start = k->out_count;
    dl_out(k, DL_OP_IEQ);
    dl_out(k, dl_temp_mask(1));
    dl_out(k, scratch);
    dl_out(k, dl_temp_select(2));
    dl_out(k, scratch);
    dl_out(k, 0x00004001u);
    dl_out(k, e - imm);
    dl_finish(k, start, NULL);
    dl_emit_test(k, DL_OP_IF, scratch, 1);
    dl_call_slot(k, fp, k->fp_base[fp] + e, site, depth);
    dl_emit0(k, DL_OP_ENDIF, NULL);
  }
}

/* call l# / callc_z / callc_nz c, l#: the subroutine inlined (inside if_z / if_nz c), with the `this` of the body it is called from. Direct3D has no
 * recursion: calls deeper than DL_DEPTH can only be one (DL_INVALID). */
static void dl_write_call(DlWork *k, const uint32_t *ins, uint32_t length, const uint32_t *self, uint32_t depth) {
  uint32_t at = 1;
  for (uint32_t ext = ins[0]; ext >> 31 && at < length;) ext = ins[at++];
  const int conditional = (ins[0] & 0x7FF) == DL_OP_CALLC;
  const uint32_t condition_at = at;
  if (conditional) {
    const uint32_t n = dl_operand_length(ins, length, at, 0);
    if (!n) { k->status = DL_INVALID; return; }
    at += n;
  }
  if (length < 2 || at != length - 2 || ins[at] != 0x0010A000u) { k->status = DL_INVALID; return; }
  uint32_t sub = DL_NONE;
  for (uint32_t n = 0; n < k->sub_count; ++n)
    if (k->sub_label[n] == ins[at + 1]) sub = n;
  if (sub == DL_NONE || depth + 1 >= DL_DEPTH) { k->status = DL_INVALID; return; }
  if (conditional) {
    const uint32_t start = k->out_count;
    dl_out(k, DL_OP_IF | (ins[0] & (1u << 18)));
    uint32_t o = condition_at;
    dl_write_operand(k, ins, length, &o, self, 0);
    dl_finish(k, start, NULL);
  }
  dl_write_range(k, k->sub_first[sub], k->sub_end[sub], self, depth + 1, 1);
  if (conditional) dl_emit0(k, DL_OP_ENDIF, NULL);
}

/* The source instructions first .. end - 1: written out with their fcalls inlined. In a body (`self` given) a return leaves the body: the last one is
 * left out, the others break out of the loop the body is wrapped in (and of the loops and switches inside it, through the flag in rT.x). */
static void dl_write_range(DlWork *k, uint32_t first, uint32_t end, const uint32_t *self, uint32_t depth, int sub) {
  const int body = self != NULL || sub;      /* (a body or a subroutine: a return leaves it) */
  const uint32_t flag_temp = k->extra_base + 1 + depth;      /* (x: returned) */
  if (depth > k->max_depth) k->max_depth = depth;
  int wrap = 0, flag = 0;
  if (body) {
    int nest = 0;
    for (uint32_t i = first; i < end; ++i) {
      const uint32_t op = k->w[k->ins_at[i]] & 0x7FF;
      if (op == DL_OP_LOOP || op == DL_OP_SWITCH) ++nest;
      else if (op == DL_OP_ENDLOOP || op == DL_OP_ENDSWITCH) --nest;
      else if ((op == DL_OP_RET && i + 1 != end) || op == DL_OP_RETC) { wrap = 1; if (nest > 0) flag = 1; }
    }
    if (flag) dl_emit_set(k, flag_temp, 0, 0);
    if (wrap) dl_emit0(k, DL_OP_LOOP, NULL);
  }
  int nest = 0;      /* the loops and switches of the body open here */
  for (uint32_t i = first; i < end && k->status == DL_OK; ++i) {
    const uint32_t at = k->ins_at[i], length = dl_ins_length(k->w, k->words, at);
    const uint32_t *ins = k->w + at;
    const uint32_t op = ins[0] & 0x7FF;
    if (op == DL_OP_DCL_FUNCTION_BODY || op == DL_OP_DCL_FUNCTION_TABLE || op == DL_OP_DCL_INTERFACE) continue;
    if (!k->temps_declared && !dl_is_declaration(op)) {
      /* (a program without temporaries gets those of the calls before its first instruction) */
      dl_out(k, DL_OP_DCL_TEMPS | 2u << 24);
      dl_temps_count(k);
      k->temps_declared = 1;
    }
    if (op == DL_OP_FCALL) { dl_write_fcall(k, ins, length, depth); continue; }
    if (op == DL_OP_CALL || op == DL_OP_CALLC) { dl_write_call(k, ins, length, self, depth); continue; }
    if (op == DL_OP_DCL_TEMPS) {
      if (length != 2) { k->status = DL_INVALID; return; }
      dl_out(k, ins[0]);
      dl_temps_count(k);
      continue;
    }
    if (dl_is_declaration(op)) {
      for (uint32_t n = 0; n < length; ++n) dl_out(k, ins[n]);
      continue;
    }
    if (body && (op == DL_OP_RET || op == DL_OP_RETC)) {
      if (op == DL_OP_RET && i + 1 == end) break;      /* (the end of the body) */
      if (op == DL_OP_RETC) {
        /* retc_z / retc_nz c: if_z / if_nz c, the return, endif */
        const uint32_t start = k->out_count;
        dl_out(k, DL_OP_IF | (ins[0] & (1u << 18)));
        uint32_t o = 1;
        for (uint32_t ext = ins[0]; ext >> 31 && o < length;) ext = ins[o++];
        if (o >= length) { k->status = DL_INVALID; return; }
        dl_write_operand(k, ins, length, &o, self, 0);
        dl_finish(k, start, NULL);
      }
      if (nest > 0) dl_emit_set(k, flag_temp, 0, 0xFFFFFFFFu);
      dl_emit0(k, DL_OP_BREAK, NULL);
      if (op == DL_OP_RETC) dl_emit0(k, DL_OP_ENDIF, NULL);
      continue;
    }
    uint8_t loop_writes[DL_TEMPS];
    if (op == DL_OP_LOOP) {
      /* (the back edge brings what the loop writes to its start) */
      uint32_t close = i + 1, open = 1;
      for (; close < end; ++close) {
        const uint32_t o2 = k->w[k->ins_at[close]] & 0x7FF;
        if (o2 == DL_OP_LOOP) ++open;
        else if (o2 == DL_OP_ENDLOOP && --open == 0) break;
      }
      dl_range_writes(k, i + 1, close, loop_writes);
    }
    DlDynamic dynamic[DL_MAX_DYNAMIC];
    const uint32_t dynamic_count = dl_dynamic_operands(k, ins, length, self, dynamic);
    if (k->status != DL_OK) return;
    if (dynamic_count) {
      /* (slot indices left registers: the instruction once for each slot they can name; flow control cannot be cut up so) */
      const int flow = op == DL_OP_IF || op == DL_OP_BREAKC || op == DL_OP_RETC || op == DL_OP_CALLC || op == DL_OP_SWITCH || op == 8 /* continuec */ || op == 13 /* discard */;
      uint32_t branches = 1;
      for (uint32_t n = 0; n < dynamic_count; ++n) {
        uint32_t slots = 0;
        for (uint32_t v = 0; v < DL_MAX_SLOT_INDEX; ++v) slots += k->declared[dynamic[n].kind][v];
        branches *= slots ? slots : 1;
      }
      if (flow || branches > DL_MAX_BRANCHES) { k->status = DL_UNSUPPORTED; return; }
      static const uint32_t copy_component[DL_MAX_DYNAMIC] = {0, 1, 3};
      for (uint32_t n = 0; n < dynamic_count; ++n) {
        const uint32_t start = k->out_count;
        dl_out(k, DL_OP_MOV);
        dl_out(k, dl_temp_mask(copy_component[n]));
        dl_out(k, k->extra_base);
        dl_out(k, dl_temp_select(dynamic[n].component));
        dl_out(k, dynamic[n].temp);
        dl_finish(k, start, NULL);
      }
      dl_write_branches(k, ins, length, self, dynamic, dynamic_count, 0);
      continue;
    }
    dl_write_instruction(k, ins, length, self, op == DL_OP_LOOP ? loop_writes : NULL);
    if (body) {
      if (op == DL_OP_LOOP || op == DL_OP_SWITCH) ++nest;
      if (op == DL_OP_ENDLOOP || op == DL_OP_ENDSWITCH) {
        --nest;
        if (flag) dl_emit_test(k, DL_OP_BREAKC, flag_temp, 0);      /* (a return inside it: on out) */
      }
    }
  }
  if (wrap && k->status == DL_OK) {
    dl_emit0(k, DL_OP_BREAK, NULL);
    dl_emit0(k, DL_OP_ENDLOOP, NULL);
  }
}

/* The shader `dxbc` specialised for the instances bound to its interface slots (`bind`, `bind_count` of them; info: dl_read_interfaces of it) into
 * k->dxbc / k->dxbc_size. */
static int dl_specialise(const uint8_t *dxbc, size_t size, const DlInfo *info, const DlBinding *bind, uint32_t bind_count, DlWork *k) {
  const uint8_t *code = NULL;
  uint32_t code_bytes = 0;
  int found = dl_find_part(dxbc, size, "SHEX", &code, &code_bytes);
  if (found == 0) found = dl_find_part(dxbc, size, "SHDR", &code, &code_bytes);
  if (found <= 0 || code_bytes < 8 || code_bytes % 4) return DL_INVALID;
  const uint32_t words = code_bytes / 4;
  if (words > DL_MAX_WORDS + 2 || dl_le32(code + 4) != words) return words > DL_MAX_WORDS + 2 ? DL_UNSUPPORTED : DL_INVALID;
  memset(k->known, 0, sizeof k->known);
  k->frame_count = 0;
  k->out_count = 0;
  k->status = DL_OK;
  k->info = info;
  k->bind = bind;
  k->bind_count = bind_count;
  /* the words as numbers (the part need not be aligned) */
  for (uint32_t i = 0; i + 2 < words; ++i) k->in[i] = dl_le32(code + 8 + 4 * i);
  k->w = k->in;
  k->words = words - 2;
  /* the instructions; the bodies, tables and interfaces */
  for (uint32_t i = 0; i < DL_MAX_BODIES; ++i) k->body_first[i] = DL_NONE, k->body_end[i] = DL_NONE;
  memset(k->table_count, 0, sizeof k->table_count);
  memset(k->fp_declared, 0, sizeof k->fp_declared);
  k->table_words = 0;
  k->ins_count = 0;
  k->main_end = DL_NONE;
  k->temps = 0;
  k->sub_count = 0;
  k->force_count = 0;
  memset(k->declared, 0, sizeof k->declared);
  uint32_t open_body = DL_NONE, open_sub = DL_NONE;
  for (uint32_t at = 0; at < k->words;) {
    const uint32_t length = dl_ins_length(k->w, k->words, at);
    if (!length) return DL_INVALID;
    if (k->ins_count >= DL_MAX_INSTRUCTIONS) return DL_UNSUPPORTED;
    const uint32_t *ins = k->w + at;
    const uint32_t op = ins[0] & 0x7FF;
    if (op == DL_OP_LABEL) {
      if (length != 3 || (ins[1] != 0x00111000u && ins[1] != 0x0010A000u)) return DL_INVALID;
      if (open_body != DL_NONE) k->body_end[open_body] = k->ins_count;
      if (open_sub != DL_NONE) k->sub_end[open_sub] = k->ins_count;
      open_body = open_sub = DL_NONE;
      if (k->main_end == DL_NONE) k->main_end = k->ins_count;
      if (ins[1] == 0x0010A000u) {
        /* (a subroutine of call / callc, label l#: written out after the main program as it is, with its fcalls inlined; the kernel inlines the calls) */
        if (k->sub_count >= DL_MAX_BODIES) return DL_UNSUPPORTED;
        open_sub = k->sub_count++;
        for (uint32_t n = 0; n < open_sub; ++n)
          if (k->sub_label[n] == ins[2]) return DL_INVALID;      /* (a label twice) */
        k->sub_label[open_sub] = ins[2];
        k->sub_first[open_sub] = k->ins_count + 1;
      } else {
        if (ins[2] >= DL_MAX_BODIES || k->body_first[ins[2]] != DL_NONE) return DL_INVALID;
        open_body = ins[2];
        k->body_first[open_body] = k->ins_count + 1;
      }
    } else if (op == DL_OP_DCL_FUNCTION_TABLE) {
      if (length < 3 || ins[1] >= DL_MAX_TABLES || k->table_count[ins[1]] || ins[2] != length - 3 || ins[2] == 0) return DL_INVALID;
      if (ins[2] > DL_MAX_TABLE_WORDS - k->table_words) return DL_UNSUPPORTED;
      k->table_first[ins[1]] = k->table_words;
      k->table_count[ins[1]] = ins[2];
      for (uint32_t n = 0; n < ins[2]; ++n) k->table_bodies[k->table_words++] = ins[3 + n];
    } else if (op == DL_OP_DCL_INTERFACE) {
      if (length < 4 || ins[1] >= DL_MAX_INTERFACES || k->fp_declared[ins[1]] || (ins[3] & 0xFFFF) != length - 4) return DL_INVALID;
      k->fp_declared[ins[1]] = 1;
      k->fp_sites[ins[1]] = ins[2];
      k->fp_array[ins[1]] = ins[3] >> 16;
      if (k->fp_array[ins[1]] == 0) return DL_INVALID;
    } else if (op == DL_OP_DCL_TEMPS) {
      if (length != 2) return DL_INVALID;
      k->temps = ins[1];
    } else if (op == 89 || op == 88 || op == 161 || op == 162 || op == 90 || (op >= 156 && op <= 158)) {
      /* dcl_constantbuffer / dcl_resource* / dcl_sampler / dcl_uav_*: the slot is the first index of the first operand */
      if (length >= 3) {
        const int kind = op == 89 ? 0 : op == 90 ? 2 : (op >= 156 && op <= 158) ? 3 : 1;
        const uint32_t slot = ins[2];
        if (slot < DL_MAX_SLOT_INDEX && (((ins[1] >> 22) & 7) == 0)) k->declared[kind][slot] = 1;
      }
    }
    k->ins_at[k->ins_count++] = at;
    at += length;
  }
  if (open_body != DL_NONE) k->body_end[open_body] = k->ins_count;
  if (open_sub != DL_NONE) k->sub_end[open_sub] = k->ins_count;
  if (k->main_end == DL_NONE) k->main_end = k->ins_count;
  /* the interfaces' slots: fp0's first, then fp1's ... */
  uint32_t slots = 0;
  for (uint32_t fp = 0; fp < DL_MAX_INTERFACES; ++fp) {
    if (!k->fp_declared[fp]) continue;
    k->fp_base[fp] = slots;
    if (k->fp_array[fp] > DL_MAX_SLOTS - slots) return DL_INVALID;
    slots += k->fp_array[fp];
  }
  if (slots != info->slot_count) return DL_INVALID;
  if (k->temps > DL_TEMPS - 6 - DL_DEPTH) return DL_UNSUPPORTED;      /* (the kernel keeps its last three temporaries for itself) */
  k->extra_base = k->temps;
  /* the program: its version and length, then the main program with the calls inlined */
  dl_out(k, dl_le32(code));
  dl_out(k, 0);
  k->temps_declared = k->temps != 0;
  k->max_depth = 0;
  k->temps_at_count = 0;
  dl_write_range(k, 0, k->main_end, NULL, 0, 0);
  if (k->status != DL_OK) return k->status;
  for (uint32_t n = 0; n < k->temps_at_count; ++n) k->out[k->temps_at[n]] = k->extra_base + 2 + k->max_depth;
  k->out[1] = k->out_count;
  /* the container: the parts as they were but IFCE, and the new program */
  const uint32_t parts = dl_le32(dxbc + 28);
  uint32_t kept = 0;
  for (uint32_t i = 0; i < parts; ++i) {
    const uint32_t at = dl_le32(dxbc + 32 + 4 * i);
    if (memcmp(dxbc + at, "IFCE", 4) != 0) ++kept;
  }
  size_t total = 32 + 4 * (size_t)kept;
  if (total > DL_OUT_BYTES) return DL_UNSUPPORTED;
  memcpy(k->dxbc, dxbc, 20);      /* (the magic and the checksum: nothing here checks it) */
  dl_put32(k->dxbc + 20, 1);
  dl_put32(k->dxbc + 28, kept);
  uint32_t index = 0;
  for (uint32_t i = 0; i < parts; ++i) {
    const uint32_t at = dl_le32(dxbc + 32 + 4 * i);
    const uint8_t *tag = dxbc + at;
    if (memcmp(tag, "IFCE", 4) == 0) continue;
    const int program_part = memcmp(tag, "SHEX", 4) == 0 || memcmp(tag, "SHDR", 4) == 0;
    const size_t bytes = program_part ? (size_t)k->out_count * 4 : dl_le32(tag + 4);
    if (8 + bytes > DL_OUT_BYTES - total) return DL_UNSUPPORTED;
    dl_put32(k->dxbc + 32 + 4 * index++, (uint32_t)total);
    memcpy(k->dxbc + total, tag, 4);
    dl_put32(k->dxbc + total + 4, (uint32_t)bytes);
    if (program_part)
      for (uint32_t n = 0; n < k->out_count; ++n) dl_put32(k->dxbc + total + 8 + 4 * n, k->out[n]);
    else
      memcpy(k->dxbc + total + 8, tag + 8, bytes);
    total += 8 + bytes;
  }
  dl_put32(k->dxbc + 24, (uint32_t)total);
  k->dxbc_size = total;
  return DL_OK;
}

#endif
