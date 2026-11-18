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
/* The part of dinput8.dll's game controller devices that is plain arithmetic, kept apart from the COM objects so that it is tested on the host
 * (tests/DirectInputJoystickTest.cpp): the objects a pad has, matching a program's data format (DIDATAFORMAT) to them, turning the pad's state
 * into the values DirectInput reports (axis ranges, dead zone, saturation, the POV hat, buttons), filling the program's state buffer, and the
 * buffer of changes GetDeviceData hands out. C99 with fixed-width types only: no Windows header is needed. Every offset and size a program
 * gives is checked before anything is written.
 *
 * Every pad looks the same to DirectInput (the layout of GamepadTypes.hpp): X/Y the left stick, Rx/Ry the right stick, Z the left trigger,
 * Rz the right trigger, one POV hat (the d-pad), and eleven buttons: south, east, west, north, left and right shoulder, back, start, the two
 * stick clicks and guide (the order Windows gives an Xbox pad's buttons, with guide added). Axes default to 0..65535, Y down-positive. */
#ifndef TCOS_DINPUT_JOYSTICK_CORE_H
#define TCOS_DINPUT_JOYSTICK_CORE_H
#include <stdint.h>

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } dj_guid;   /* (the layout of GUID) */

#define DJ_GUID_AXIS(n) {0xA36D02E0u + (n), 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}}
static const dj_guid dj_guid_x = DJ_GUID_AXIS(0x00), dj_guid_y = DJ_GUID_AXIS(0x01), dj_guid_z = DJ_GUID_AXIS(0x02), dj_guid_rz = DJ_GUID_AXIS(0x03),
                     dj_guid_slider = DJ_GUID_AXIS(0x04), dj_guid_button = DJ_GUID_AXIS(0x10), dj_guid_pov = DJ_GUID_AXIS(0x12),
                     dj_guid_rx = DJ_GUID_AXIS(0x14), dj_guid_ry = DJ_GUID_AXIS(0x15);

/* DirectInput's object type bits (dinput.h's DIDFT_*). */
enum {
  DJ_DIDFT_ABSAXIS = 0x02, DJ_DIDFT_AXIS = 0x03, DJ_DIDFT_PSHBUTTON = 0x04, DJ_DIDFT_BUTTON = 0x0C, DJ_DIDFT_POV = 0x10,
  DJ_DIDFT_ANYINSTANCE = 0x00FFFF00, DJ_DIDFT_OPTIONAL = (int)0x80000000u
};

/* The pad's state as NtTcosGamepad's GET_STATE gives it (src/Process/WindowsGamepad.hpp). */
typedef struct {
  uint32_t buttons;                        /* XInput bits and above */
  uint8_t left_trigger, right_trigger;
  int16_t left_x, left_y, right_x, right_y; /* up and right positive */
} dj_pad;

enum { DJ_KIND_AXIS, DJ_KIND_POV, DJ_KIND_BUTTON };
#define DJ_AXES 6
#define DJ_BUTTONS 11
#define DJ_OBJECTS (DJ_AXES + 1 + DJ_BUTTONS)

typedef struct {
  const dj_guid *guid;
  uint8_t kind;
  uint16_t instance;       /* among the objects of its kind */
  uint32_t default_offset; /* in DIJOYSTATE (c_dfDIJoystick) */
  uint16_t usage_page, usage;
  const char *name;
} dj_object;

static const dj_object dj_objects[DJ_OBJECTS] = {
    {&dj_guid_x, DJ_KIND_AXIS, 0, 0, 1, 0x30, "X Axis"},
    {&dj_guid_y, DJ_KIND_AXIS, 1, 4, 1, 0x31, "Y Axis"},
    {&dj_guid_z, DJ_KIND_AXIS, 2, 8, 1, 0x32, "Z Axis"},
    {&dj_guid_rx, DJ_KIND_AXIS, 3, 12, 1, 0x33, "X Rotation"},
    {&dj_guid_ry, DJ_KIND_AXIS, 4, 16, 1, 0x34, "Y Rotation"},
    {&dj_guid_rz, DJ_KIND_AXIS, 5, 20, 1, 0x35, "Z Rotation"},
    {&dj_guid_pov, DJ_KIND_POV, 0, 32, 1, 0x39, "Hat Switch"},
    {&dj_guid_button, DJ_KIND_BUTTON, 0, 48, 9, 1, "Button 0"},
    {&dj_guid_button, DJ_KIND_BUTTON, 1, 49, 9, 2, "Button 1"},
    {&dj_guid_button, DJ_KIND_BUTTON, 2, 50, 9, 3, "Button 2"},
    {&dj_guid_button, DJ_KIND_BUTTON, 3, 51, 9, 4, "Button 3"},
    {&dj_guid_button, DJ_KIND_BUTTON, 4, 52, 9, 5, "Button 4"},
    {&dj_guid_button, DJ_KIND_BUTTON, 5, 53, 9, 6, "Button 5"},
    {&dj_guid_button, DJ_KIND_BUTTON, 6, 54, 9, 7, "Button 6"},
    {&dj_guid_button, DJ_KIND_BUTTON, 7, 55, 9, 8, "Button 7"},
    {&dj_guid_button, DJ_KIND_BUTTON, 8, 56, 9, 9, "Button 8"},
    {&dj_guid_button, DJ_KIND_BUTTON, 9, 57, 9, 10, "Button 9"},
    {&dj_guid_button, DJ_KIND_BUTTON, 10, 58, 9, 11, "Button 10"},
};
/* The pad's button bits behind DirectInput's buttons 0..10 (GamepadTypes.hpp's Button). */
static const uint32_t dj_button_bits[DJ_BUTTONS] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080, 0x0400};

static uint32_t dj_object_type(uint32_t index) {
  const dj_object *o = &dj_objects[index];
  const uint32_t kind = o->kind == DJ_KIND_AXIS ? DJ_DIDFT_ABSAXIS : o->kind == DJ_KIND_POV ? DJ_DIDFT_POV : DJ_DIDFT_PSHBUTTON;
  return kind | ((uint32_t)o->instance << 8);
}
static uint32_t dj_value_size(uint32_t index) { return dj_objects[index].kind == DJ_KIND_BUTTON ? 1u : 4u; }

static int dj_guid_equal(const dj_guid *a, const dj_guid *b) {
  if (a->d1 != b->d1 || a->d2 != b->d2 || a->d3 != b->d3) return 0;
  for (int i = 0; i < 8; ++i) if (a->d4[i] != b->d4[i]) return 0;
  return 1;
}

/* Per-axis properties (DIPROP_RANGE, DIPROP_DEADZONE, DIPROP_SATURATION). */
typedef struct {
  int32_t min[DJ_AXES], max[DJ_AXES];
  uint32_t deadzone[DJ_AXES], saturation[DJ_AXES];   /* 0..10000 */
} dj_axis_props;

static void dj_default_props(dj_axis_props *p) {
  for (int i = 0; i < DJ_AXES; ++i) {
    p->min[i] = 0;
    p->max[i] = 65535;
    p->deadzone[i] = 0;
    p->saturation[i] = 10000;
  }
}

/* One entry of a program's data format, as DIOBJECTDATAFORMAT has it (the GUID already copied out of the program's memory, or null for any). */
typedef struct {
  const dj_guid *guid;
  uint32_t offset, type, flags;
} dj_format_entry;

#define DJ_MAX_FORMAT 64
typedef struct {
  uint32_t data_size;
  uint32_t count;
  uint8_t object[DJ_MAX_FORMAT];       /* which object each mapped entry is */
  uint32_t offset[DJ_MAX_FORMAT];      /* where its value goes in the program's buffer */
  int set;
} dj_format;

/* dj_set_format's answers. */
enum { DJ_OK = 0, DJ_INVALID = 1 };

/* Matches a data format to the objects: each entry takes the first unused object of a matching GUID, type and instance. An entry nothing matches
 * is left out if it is optional (DIDFT_OPTIONAL, as every entry of c_dfDIJoystick is) and fails the format otherwise. Every value must fit in
 * `data_size`, axes and the POV DWORD-aligned, and no two values may overlap. On failure `out` is unchanged. */
static int dj_set_format(dj_format *out, const dj_format_entry *entries, uint32_t entry_count, uint32_t data_size) {
  dj_format f;
  uint8_t used[DJ_OBJECTS] = {0};
  if (data_size == 0 || data_size > 0x10000 || (data_size & 3) != 0 || entry_count > 0x10000) return DJ_INVALID;
  f.data_size = data_size;
  f.count = 0;
  f.set = 1;
  for (uint32_t e = 0; e < entry_count; ++e) {
    const dj_format_entry *x = &entries[e];
    const uint32_t want_type = x->type & 0xFF, want_instance = x->type & DJ_DIDFT_ANYINSTANCE;
    int found = -1;
    for (uint32_t o = 0; o < DJ_OBJECTS && found < 0; ++o) {
      if (used[o]) continue;
      if (x->guid && !dj_guid_equal(x->guid, dj_objects[o].guid)) continue;
      const uint32_t type = dj_object_type(o);
      if (want_type && !(want_type & type & 0xFF)) continue;
      if (want_instance != DJ_DIDFT_ANYINSTANCE && want_instance != (type & DJ_DIDFT_ANYINSTANCE)) continue;
      found = (int)o;
    }
    if (found < 0) {
      if (x->type & (uint32_t)DJ_DIDFT_OPTIONAL) continue;
      return DJ_INVALID;
    }
    const uint32_t size = dj_value_size((uint32_t)found);
    if (x->offset > data_size || data_size - x->offset < size) return DJ_INVALID;
    if (size == 4 && (x->offset & 3) != 0) return DJ_INVALID;
    for (uint32_t m = 0; m < f.count; ++m) {      /* overlapping values */
      const uint32_t other = f.offset[m], other_size = dj_value_size(f.object[m]);
      if (x->offset < other + other_size && other < x->offset + size) return DJ_INVALID;
    }
    if (f.count == DJ_MAX_FORMAT) return DJ_INVALID;
    used[found] = 1;
    f.object[f.count] = (uint8_t)found;
    f.offset[f.count] = x->offset;
    ++f.count;
  }
  *out = f;
  return DJ_OK;
}

/* A stick axis or a trigger, as -32767..32767 (-32768 counts as -32767, so that the scale is symmetric and its centre exact), to the property's
 * range after the dead zone and saturation (sticks only: a trigger rests at its minimum). */
static int32_t dj_scale(int32_t raw, const dj_axis_props *p, int axis, int centred) {
  int64_t v = raw < -32767 ? -32767 : raw > 32767 ? 32767 : raw;
  if (centred) {
    const int64_t magnitude = v < 0 ? -v : v;
    const int64_t dz = (int64_t)p->deadzone[axis] * 32767 / 10000, sat = (int64_t)p->saturation[axis] * 32767 / 10000;
    if (magnitude <= dz) v = 0;
    else if (sat <= dz || magnitude >= sat) v = v < 0 ? -32767 : 32767;
    else {
      const int64_t scaled = (magnitude - dz) * 32767 / (sat - dz);
      v = v < 0 ? -scaled : scaled;
    }
  }
  const int64_t min = p->min[axis], max = p->max[axis];
  return (int32_t)(min + (v + 32767) * (max - min) / 65534);
}

static int32_t dj_trigger(uint8_t t) { return (int32_t)t * 65534 / 255 - 32767; }

/* The value of every object for a pad state: axes in their ranges, the POV in hundredths of a degree (0xFFFFFFFF centred), buttons 0x80 or 0. */
static void dj_values(const dj_pad *pad, const dj_axis_props *p, uint32_t values[DJ_OBJECTS]) {
  values[0] = (uint32_t)dj_scale(pad->left_x, p, 0, 1);
  values[1] = (uint32_t)dj_scale(-(int32_t)pad->left_y, p, 1, 1);   /* (DirectInput's Y is down-positive) */
  values[2] = (uint32_t)dj_scale(dj_trigger(pad->left_trigger), p, 2, 0);
  values[3] = (uint32_t)dj_scale(pad->right_x, p, 3, 1);
  values[4] = (uint32_t)dj_scale(-(int32_t)pad->right_y, p, 4, 1);
  values[5] = (uint32_t)dj_scale(dj_trigger(pad->right_trigger), p, 5, 0);
  const int up = (pad->buttons & 1) != 0, down = (pad->buttons & 2) != 0, left = (pad->buttons & 4) != 0, right = (pad->buttons & 8) != 0;
  const int y = up && !down ? 1 : down && !up ? -1 : 0, x = right && !left ? 1 : left && !right ? -1 : 0;
  static const uint32_t angles[3][3] = {{22500, 18000, 13500}, {27000, 0xFFFFFFFFu, 9000}, {31500, 0, 4500}};   /* [y + 1][x + 1] */
  values[DJ_AXES] = angles[y + 1][x + 1];
  for (int b = 0; b < DJ_BUTTONS; ++b) values[DJ_AXES + 1 + b] = (pad->buttons & dj_button_bits[b]) ? 0x80 : 0;
}

/* Writes the values into the program's state buffer as its format lays them out; everything else is zero. `buffer_size` must be the format's
 * data size (the caller checks: DIERR_INVALIDPARAM otherwise). */
static void dj_fill_state(const dj_format *f, const uint32_t values[DJ_OBJECTS], uint8_t *buffer) {
  for (uint32_t i = 0; i < f->data_size; ++i) buffer[i] = 0;
  for (uint32_t m = 0; m < f->count; ++m) {
    const uint32_t v = values[f->object[m]], at = f->offset[m];
    if (dj_value_size(f->object[m]) == 1) {
      buffer[at] = (uint8_t)v;
    } else {
      buffer[at] = (uint8_t)v;
      buffer[at + 1] = (uint8_t)(v >> 8);
      buffer[at + 2] = (uint8_t)(v >> 16);
      buffer[at + 3] = (uint8_t)(v >> 24);
    }
  }
}

/* The changes since the last look, for GetDeviceData (DIDEVICEOBJECTDATA: offset, data, time, sequence). */
#define DJ_MAX_BUFFER 1024
typedef struct {
  uint32_t offset, data, time, sequence;
} dj_event;
typedef struct {
  dj_event events[DJ_MAX_BUFFER];
  uint32_t capacity;       /* DIPROP_BUFFERSIZE, at most DJ_MAX_BUFFER; 0: not buffered */
  uint32_t head, count;
  uint32_t sequence;
  int overflow;
} dj_buffer;

/* Records every mapped object whose value differs between `before` and `now`. A full buffer drops the oldest event and notes the overflow. */
static void dj_record(dj_buffer *b, const dj_format *f, const uint32_t before[DJ_OBJECTS], const uint32_t now[DJ_OBJECTS], uint32_t time) {
  if (!b->capacity) return;
  for (uint32_t m = 0; m < f->count; ++m) {
    const uint32_t o = f->object[m];
    if (before[o] == now[o]) continue;
    if (b->count == b->capacity) {
      b->head = (b->head + 1) % b->capacity;
      --b->count;
      b->overflow = 1;
    }
    dj_event *e = &b->events[(b->head + b->count) % b->capacity];
    e->offset = f->offset[m];
    e->data = now[o];
    e->time = time;
    e->sequence = ++b->sequence;
    ++b->count;
  }
}

/* Takes (or with `peek` only looks at) up to `max` of the oldest events. */
static uint32_t dj_take(dj_buffer *b, dj_event *out, uint32_t max, int peek) {
  uint32_t n = 0;
  while (n < max && n < b->count) {
    if (out) out[n] = b->events[(b->head + n) % b->capacity];
    ++n;
  }
  if (!peek) {
    b->head = b->capacity ? (b->head + n) % b->capacity : 0;
    b->count -= n;
  }
  return n;
}

static void dj_set_buffer_size(dj_buffer *b, uint32_t size) {
  b->capacity = size > DJ_MAX_BUFFER ? DJ_MAX_BUFFER : size;
  b->head = b->count = 0;
  b->overflow = 0;
}

/* The object a DIPROPHEADER's dwHow/dwObj names: DIPH_BYOFFSET (an offset of the current format), DIPH_BYID (an object type and instance) or
 * DIPH_BYUSAGE (usage page | usage << 16); -1 if none. */
static int dj_find_object(const dj_format *f, uint32_t how, uint32_t which) {
  if (how == 1) {                                         /* by offset */
    if (!f->set) return -1;
    for (uint32_t m = 0; m < f->count; ++m) if (f->offset[m] == which) return f->object[m];
    return -1;
  }
  for (uint32_t o = 0; o < DJ_OBJECTS; ++o) {
    if (how == 2 && (dj_object_type(o) & 0x00FFFFFF) == (which & 0x00FFFFFF) && (which & 0xFF & dj_object_type(o))) return (int)o;
    if (how == 3 && dj_objects[o].usage_page == (which & 0xFFFF) && dj_objects[o].usage == (which >> 16)) return (int)o;
  }
  return -1;
}

#endif
