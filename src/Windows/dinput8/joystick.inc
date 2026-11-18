/* dinput8.dll's game controller devices: one per occupied player slot of the kernel's game controller service, read with NtTcosGamepad
 * (src/Process/WindowsGamepad.hpp). The arithmetic is joystick_core.h's; this is the COM side: IDirectInputDevice8A/W, the device instances
 * EnumDevices reports, and the properties programs ask for (ranges, dead zone, saturation, buffer size, VID/PID, names, the device path
 * with "IG_" for pads that XInput also serves, so that a program using both - SDL - counts them once). No force feedback (rumble goes through
 * XInput), no action mapping. Included by dinput8.c. */
#include "joystick_core.h"

__declspec(dllimport) NTSTATUS NTAPI NtTcosGamepad(ULONG64 *arguments);

#define PAD_SLOTS_ 8
enum { PAD_GET_STATE = 1, PAD_GET_INFO = 3, PAD_GET_NAME = 5 };
enum { PAD_PROTOCOL_XBOX360 = 1, PAD_PROTOCOL_XBOXONE = 2 };

typedef struct {
  DWORD protocol, vendor, product;
  char name[64];
} pad_info;

/* What the slot holds; FALSE if it is empty (or the service is not there). */
static BOOL pad_query(DWORD slot, pad_info *info) {
  ULONG64 w[8] = {PAD_GET_INFO, slot};
  if (NtTcosGamepad(w) != 0) return FALSE;
  info->protocol = (DWORD)w[4];
  info->vendor = (DWORD)(w[5] & 0xFFFF);
  info->product = (DWORD)((w[5] >> 16) & 0xFFFF);
  ULONG64 n[8] = {PAD_GET_NAME, slot, (ULONG64)(ULONG_PTR)info->name, sizeof info->name - 1};
  info->name[0] = 0;
  if (NtTcosGamepad(n) == 0) info->name[n[7] < sizeof info->name - 1 ? n[7] : sizeof info->name - 1] = 0;
  if (!info->name[0]) lstrcpyA(info->name, "Game Controller");
  return TRUE;
}

static BOOL pad_state(DWORD slot, dj_pad *pad) {
  ULONG64 w[8] = {PAD_GET_STATE, slot};
  if (NtTcosGamepad(w) != 0) return FALSE;
  pad->buttons = (uint32_t)w[5];
  pad->left_trigger = (uint8_t)w[6];
  pad->right_trigger = (uint8_t)(w[6] >> 8);
  pad->left_x = (int16_t)(uint16_t)w[7];
  pad->left_y = (int16_t)(uint16_t)(w[7] >> 16);
  pad->right_x = (int16_t)(uint16_t)(w[7] >> 32);
  pad->right_y = (int16_t)(uint16_t)(w[7] >> 48);
  return TRUE;
}

/* The GUIDs a pad is known by: its instance (the slot) and its product (DirectInput's "PIDVID" form, which programs take the USB ids from). */
static void pad_instance_guid(DWORD slot, GUID *g) {
  static const GUID base = {0x7C05A000, 0x0B5E, 0x11F1, {'T', 'C', 'O', 'S', 'P', 'A', 'D', 0}};
  *g = base;
  g->Data1 += slot;
  g->Data4[7] = (BYTE)slot;
}
static void pad_product_guid(const pad_info *info, GUID *g) {
  static const GUID base = {0, 0, 0, {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'}};
  *g = base;
  g->Data1 = info->vendor | (info->product << 16);
}
/* The slot an instance GUID names, or -1. */
static int pad_slot_of(const GUID *g) {
  for (DWORD slot = 0; slot < PAD_SLOTS_; ++slot) {
    GUID instance;
    pad_instance_guid(slot, &instance);
    if (same_guid(g, &instance)) return (int)slot;
  }
  return -1;
}
/* The slot whose pad has this product GUID (the first), or -1. */
static int pad_slot_of_product(const GUID *g) {
  for (DWORD slot = 0; slot < PAD_SLOTS_; ++slot) {
    pad_info info;
    GUID product;
    if (!pad_query(slot, &info)) continue;
    pad_product_guid(&info, &product);
    if (same_guid(g, &product)) return (int)slot;
  }
  return -1;
}

#define PAD_DEVTYPE_ (DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID)

static void widen(WCHAR *out, const char *in, size_t capacity) {
  size_t i = 0;
  for (; in[i] && i + 1 < capacity; ++i) out[i] = (WCHAR)(BYTE)in[i];
  out[i] = 0;
}
/* Appends text, a 4-digit hex number (upper or lower case) or a decimal number to a string being built (always terminated; stops at `capacity`). */
typedef struct { char *out; size_t used, capacity; } text_builder;
static void tb_put(text_builder *b, char c) { if (b->used + 1 < b->capacity) { b->out[b->used++] = c; b->out[b->used] = 0; } }
static void tb_text(text_builder *b, const char *s) { while (*s) tb_put(b, *s++); }
static void tb_hex4(text_builder *b, unsigned v, BOOL upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  for (int shift = 12; shift >= 0; shift -= 4) tb_put(b, digits[(v >> shift) & 0xF]);
}
static void tb_decimal(text_builder *b, unsigned v) {
  char digits[12];
  int n = 0;
  do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 12);
  while (n) tb_put(b, digits[--n]);
}

static void copy_narrow(char *out, const char *in, size_t capacity) {
  size_t i = 0;
  for (; in[i] && i + 1 < capacity; ++i) out[i] = in[i];
  out[i] = 0;
}

/* Fills a DIDEVICEINSTANCEA or W (either size: DirectInput 3's lacks the force feedback driver and the usage). */
static BOOL fill_instance(DWORD slot, const pad_info *info, void *out, BOOL wide) {
  if (wide) {
    DIDEVICEINSTANCEW *d = out;
    if (d->dwSize != sizeof(DIDEVICEINSTANCEW) && d->dwSize != sizeof(DIDEVICEINSTANCE_DX3W)) return FALSE;
    const DWORD size = d->dwSize;
    ZeroMemory(d, size);
    d->dwSize = size;
    pad_instance_guid(slot, &d->guidInstance);
    pad_product_guid(info, &d->guidProduct);
    d->dwDevType = PAD_DEVTYPE_;
    widen(d->tszInstanceName, info->name, MAX_PATH);
    widen(d->tszProductName, info->name, MAX_PATH);
    if (size == sizeof(DIDEVICEINSTANCEW)) { d->wUsagePage = 1; d->wUsage = 5; }
  } else {
    DIDEVICEINSTANCEA *d = out;
    if (d->dwSize != sizeof(DIDEVICEINSTANCEA) && d->dwSize != sizeof(DIDEVICEINSTANCE_DX3A)) return FALSE;
    const DWORD size = d->dwSize;
    ZeroMemory(d, size);
    d->dwSize = size;
    pad_instance_guid(slot, &d->guidInstance);
    pad_product_guid(info, &d->guidProduct);
    d->dwDevType = PAD_DEVTYPE_;
    copy_narrow(d->tszInstanceName, info->name, MAX_PATH);
    copy_narrow(d->tszProductName, info->name, MAX_PATH);
    if (size == sizeof(DIDEVICEINSTANCEA)) { d->wUsagePage = 1; d->wUsage = 5; }
  }
  return TRUE;
}

/* EnumDevices' part for game controllers: the occupied slots, for the classes and types they belong to. */
static HRESULT enum_pads(DWORD type, void *callback, void *context, DWORD flags, BOOL wide) {
  if (flags & DIEDFL_FORCEFEEDBACK) return DI_OK;      /* (no force feedback) */
  if (type != DI8DEVCLASS_ALL && type != DI8DEVCLASS_GAMECTRL && (type & 0xFF) != DI8DEVTYPE_GAMEPAD) return DI_OK;
  for (DWORD slot = 0; slot < PAD_SLOTS_; ++slot) {
    pad_info info;
    if (!pad_query(slot, &info)) continue;
    if (wide) {
      DIDEVICEINSTANCEW d;
      d.dwSize = sizeof d;
      fill_instance(slot, &info, &d, TRUE);
      if (((LPDIENUMDEVICESCALLBACKW)callback)(&d, context) == DIENUM_STOP) break;
    } else {
      DIDEVICEINSTANCEA d;
      d.dwSize = sizeof d;
      fill_instance(slot, &info, &d, FALSE);
      if (((LPDIENUMDEVICESCALLBACKA)callback)(&d, context) == DIENUM_STOP) break;
    }
  }
  return DI_OK;
}

/* ------------------------------------------------------------------------------------------------- the device */

static const GUID kIidDevice8A = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
static const GUID kIidDevice8W = {0x54D41081, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
static const GUID kGuidHidClass = {0x745A17A0, 0x74D3, 0x11D0, {0xB6, 0xFE, 0x00, 0xA0, 0xC9, 0x0F, 0x57, 0xDA}};

typedef struct {
  const void *vtbl_a;           /* IDirectInputDevice8A (first) */
  const void *vtbl_w;
  LONG refs;
  DWORD slot;
  pad_info info;
  CRITICAL_SECTION lock;        /* everything below */
  dj_format format;
  dj_axis_props props;
  uint32_t last[DJ_OBJECTS];
  BOOL have_last, acquired;
  HANDLE event;
  dj_buffer buffer;             /* (16 KiB: the object is on the heap) */
} PadDevice;

static PadDevice *dev_a(void *iface) { return (PadDevice *)iface; }
static PadDevice *dev_w(void *iface) { return (PadDevice *)((BYTE *)iface - offsetof(PadDevice, vtbl_w)); }

static HRESULT dev_query(PadDevice *d, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!riid) return E_POINTER;
  if (same_guid(riid, &kIidUnknown) || same_guid(riid, &kIidDevice8A)) *out = &d->vtbl_a;
  else if (same_guid(riid, &kIidDevice8W)) *out = &d->vtbl_w;
  else return E_NOINTERFACE;
  InterlockedIncrement(&d->refs);
  return S_OK;
}
static ULONG dev_add_ref(PadDevice *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG dev_release(PadDevice *d) {
  const LONG left = InterlockedDecrement(&d->refs);
  if (left == 0) {
    DeleteCriticalSection(&d->lock);
    HeapFree(GetProcessHeap(), 0, d);
  }
  return (ULONG)left;
}

/* Reads the pad, records what changed (for GetDeviceData) and signals the event. (Locked.) FALSE if the pad is gone. */
static BOOL dev_refresh(PadDevice *d, uint32_t values[DJ_OBJECTS]) {
  dj_pad pad;
  if (!pad_state(d->slot, &pad)) {
    d->acquired = FALSE;          /* (lost: the program acquires again once the pad is back) */
    return FALSE;
  }
  dj_values(&pad, &d->props, values);
  if (d->have_last) {
    const uint32_t before_count = d->buffer.count, before_sequence = d->buffer.sequence;
    dj_record(&d->buffer, &d->format, d->last, values, GetTickCount());
    if (d->event && (d->buffer.count != before_count || d->buffer.sequence != before_sequence)) SetEvent(d->event);
  }
  CopyMemory(d->last, values, sizeof d->last);
  d->have_last = TRUE;
  return TRUE;
}

static HRESULT dev_capabilities(PadDevice *d, DIDEVCAPS *caps) {
  if (!caps) return E_POINTER;
  if (caps->dwSize != sizeof(DIDEVCAPS) && caps->dwSize != sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
  const DWORD size = caps->dwSize;
  ZeroMemory(caps, size);
  caps->dwSize = size;
  pad_info info;
  caps->dwFlags = DIDC_POLLEDDEVICE | DIDC_POLLEDDATAFORMAT | (pad_query(d->slot, &info) ? DIDC_ATTACHED : 0);
  caps->dwDevType = PAD_DEVTYPE_;
  caps->dwAxes = DJ_AXES;
  caps->dwButtons = DJ_BUTTONS;
  caps->dwPOVs = 1;
  return DI_OK;
}

/* The offset an object has now: in the program's format if it is mapped there, else in c_dfDIJoystick. */
static DWORD dev_object_offset(PadDevice *d, uint32_t object) {
  if (d->format.set) {
    for (uint32_t m = 0; m < d->format.count; ++m) if (d->format.object[m] == object) return d->format.offset[m];
  }
  return dj_objects[object].default_offset;
}

static BOOL fill_object(PadDevice *d, uint32_t o, void *out, BOOL wide) {
  const dj_object *obj = &dj_objects[o];
  if (wide) {
    DIDEVICEOBJECTINSTANCEW *x = out;
    if (x->dwSize != sizeof(DIDEVICEOBJECTINSTANCEW) && x->dwSize != sizeof(DIDEVICEOBJECTINSTANCE_DX3W)) return FALSE;
    const DWORD size = x->dwSize;
    ZeroMemory(x, size);
    x->dwSize = size;
    CopyMemory(&x->guidType, obj->guid, sizeof(GUID));
    x->dwOfs = dev_object_offset(d, o);
    x->dwType = dj_object_type(o);
    x->dwFlags = obj->kind == DJ_KIND_AXIS ? DIDOI_ASPECTPOSITION : 0;
    widen(x->tszName, obj->name, MAX_PATH);
    if (size == sizeof(DIDEVICEOBJECTINSTANCEW)) { x->wUsagePage = obj->usage_page; x->wUsage = obj->usage; }
  } else {
    DIDEVICEOBJECTINSTANCEA *x = out;
    if (x->dwSize != sizeof(DIDEVICEOBJECTINSTANCEA) && x->dwSize != sizeof(DIDEVICEOBJECTINSTANCE_DX3A)) return FALSE;
    const DWORD size = x->dwSize;
    ZeroMemory(x, size);
    x->dwSize = size;
    CopyMemory(&x->guidType, obj->guid, sizeof(GUID));
    x->dwOfs = dev_object_offset(d, o);
    x->dwType = dj_object_type(o);
    x->dwFlags = obj->kind == DJ_KIND_AXIS ? DIDOI_ASPECTPOSITION : 0;
    copy_narrow(x->tszName, obj->name, MAX_PATH);
    if (size == sizeof(DIDEVICEOBJECTINSTANCEA)) { x->wUsagePage = obj->usage_page; x->wUsage = obj->usage; }
  }
  return TRUE;
}

static HRESULT dev_enum_objects(PadDevice *d, void *callback, void *context, DWORD flags, BOOL wide) {
  if (!callback) return DIERR_INVALIDPARAM;
  const DWORD kinds = flags & 0xFF;
  for (uint32_t o = 0; o < DJ_OBJECTS; ++o) {
    const uint32_t type = dj_object_type(o) & 0xFF;
    if (kinds != DIDFT_ALL && !(kinds & type)) continue;
    BOOL stop;
    EnterCriticalSection(&d->lock);
    if (wide) {
      DIDEVICEOBJECTINSTANCEW x;
      x.dwSize = sizeof x;
      fill_object(d, o, &x, TRUE);
      LeaveCriticalSection(&d->lock);
      stop = ((LPDIENUMDEVICEOBJECTSCALLBACKW)callback)(&x, context) == DIENUM_STOP;
    } else {
      DIDEVICEOBJECTINSTANCEA x;
      x.dwSize = sizeof x;
      fill_object(d, o, &x, FALSE);
      LeaveCriticalSection(&d->lock);
      stop = ((LPDIENUMDEVICEOBJECTSCALLBACKA)callback)(&x, context) == DIENUM_STOP;
    }
    if (stop) break;
  }
  return DI_OK;
}

static HRESULT dev_object_info(PadDevice *d, void *out, DWORD which, DWORD how, BOOL wide) {
  if (!out) return E_POINTER;
  EnterCriticalSection(&d->lock);
  const int o = how == DIPH_DEVICE ? -1 : dj_find_object(&d->format, how, which);
  HRESULT hr = DIERR_OBJECTNOTFOUND;
  if (how > DIPH_BYUSAGE) hr = DIERR_INVALIDPARAM;
  else if (o >= 0) hr = fill_object(d, (uint32_t)o, out, wide) ? DI_OK : DIERR_INVALIDPARAM;
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_set_format(PadDevice *d, const DIDATAFORMAT *format) {
  if (!format) return E_POINTER;
  if (format->dwSize != sizeof(DIDATAFORMAT) || format->dwObjSize != sizeof(DIOBJECTDATAFORMAT) || format->dwNumObjs > DJ_MAX_FORMAT * 4 ||
      (format->dwNumObjs && !format->rgodf))
    return DIERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  HRESULT hr = DI_OK;
  if (d->acquired) {
    hr = DIERR_ACQUIRED;
  } else {
    dj_format_entry entries[DJ_MAX_FORMAT * 4];
    dj_guid guids[DJ_MAX_FORMAT * 4];
    for (DWORD i = 0; i < format->dwNumObjs; ++i) {
      const DIOBJECTDATAFORMAT *x = &format->rgodf[i];
      if (x->pguid) CopyMemory(&guids[i], x->pguid, sizeof(GUID));
      entries[i].guid = x->pguid ? &guids[i] : NULL;
      entries[i].offset = x->dwOfs;
      entries[i].type = x->dwType;
      entries[i].flags = x->dwFlags;
    }
    if (dj_set_format(&d->format, entries, format->dwNumObjs, format->dwDataSize) != DJ_OK) hr = DIERR_INVALIDPARAM;
    d->have_last = FALSE;
    dj_set_buffer_size(&d->buffer, d->buffer.capacity);
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_acquire(PadDevice *d) {
  EnterCriticalSection(&d->lock);
  HRESULT hr;
  pad_info info;
  if (!d->format.set) hr = DIERR_INVALIDPARAM;
  else if (d->acquired) hr = S_FALSE;
  else if (!pad_query(d->slot, &info)) hr = DIERR_UNPLUGGED;
  else {
    d->acquired = TRUE;
    d->have_last = FALSE;
    uint32_t values[DJ_OBJECTS];
    dev_refresh(d, values);            /* (the starting point of the buffered changes) */
    hr = DI_OK;
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT dev_unacquire(PadDevice *d) {
  EnterCriticalSection(&d->lock);
  const BOOL was = d->acquired;
  d->acquired = FALSE;
  LeaveCriticalSection(&d->lock);
  return was ? DI_OK : DI_NOEFFECT;
}

static HRESULT dev_poll(PadDevice *d) {
  EnterCriticalSection(&d->lock);
  HRESULT hr = DIERR_NOTACQUIRED;
  if (d->acquired) {
    uint32_t values[DJ_OBJECTS];
    hr = dev_refresh(d, values) ? DI_OK : DIERR_INPUTLOST;
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_state(PadDevice *d, DWORD size, void *data) {
  if (!data) return E_POINTER;
  EnterCriticalSection(&d->lock);
  HRESULT hr;
  uint32_t values[DJ_OBJECTS];
  if (!d->format.set || size != d->format.data_size) hr = DIERR_INVALIDPARAM;
  else if (!d->acquired) hr = DIERR_NOTACQUIRED;
  else if (!dev_refresh(d, values)) hr = DIERR_INPUTLOST;
  else {
    dj_fill_state(&d->format, values, data);
    hr = DI_OK;
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_data(PadDevice *d, DWORD object_size, DIDEVICEOBJECTDATA *out, DWORD *count, DWORD flags) {
  if (!count) return E_POINTER;
  if (object_size != sizeof(DIDEVICEOBJECTDATA) && object_size != sizeof(DIDEVICEOBJECTDATA_DX3)) return DIERR_INVALIDPARAM;
  if (flags & ~DIGDD_PEEK) return DIERR_INVALIDPARAM;
  EnterCriticalSection(&d->lock);
  HRESULT hr = DI_OK;
  uint32_t values[DJ_OBJECTS];
  if (!d->buffer.capacity) hr = DIERR_NOTBUFFERED;
  else if (!d->acquired) hr = DIERR_NOTACQUIRED;
  else if (!dev_refresh(d, values)) hr = DIERR_INPUTLOST;
  if (hr != DI_OK) {
    LeaveCriticalSection(&d->lock);
    *count = 0;
    return hr;
  }
  /* The oldest `*count` events (all of them for a flush: no buffer and INFINITE), copied out; taken unless this is a peek. */
  const BOOL overflow = d->buffer.overflow, peek = (flags & DIGDD_PEEK) != 0;
  const DWORD n = *count < d->buffer.count ? *count : d->buffer.count;
  for (DWORD i = 0; out && i < n; ++i) {
    const dj_event *e = &d->buffer.events[(d->buffer.head + i) % d->buffer.capacity];
    DIDEVICEOBJECTDATA x;
    ZeroMemory(&x, sizeof x);
    x.dwOfs = e->offset;
    x.dwData = e->data;
    x.dwTimeStamp = e->time;
    x.dwSequence = e->sequence;
    CopyMemory((BYTE *)out + (SIZE_T)i * object_size, &x, object_size);
  }
  if (!peek) {
    (void)dj_take(&d->buffer, NULL, n, 0);
    d->buffer.overflow = 0;
  }
  LeaveCriticalSection(&d->lock);
  *count = n;
  return overflow ? DI_BUFFEROVERFLOW : DI_OK;
}

/* ------------------------------------------------------------------------------------------------- properties */

static BOOL header_ok(const DIPROPHEADER *h, DWORD size) { return h && h->dwHeaderSize == sizeof(DIPROPHEADER) && h->dwSize == size; }

/* The axes a header names: one (its index) or all of them (DIPH_DEVICE); -1 if it names no axis. */
static int prop_axis(PadDevice *d, const DIPROPHEADER *h, BOOL *all) {
  *all = h->dwHow == DIPH_DEVICE && h->dwObj == 0;
  if (*all) return 0;
  const int o = dj_find_object(&d->format, h->dwHow, h->dwObj);
  return o >= 0 && o < DJ_AXES ? o : -1;
}

static HRESULT dev_get_property(PadDevice *d, REFGUID property, DIPROPHEADER *h) {
  if (!property || !h) return E_POINTER;
  const ULONG_PTR id = (ULONG_PTR)property;
  if (id > 0xFFFF) return DIERR_UNSUPPORTED;      /* (properties are small numbers: MAKEDIPROP) */
  EnterCriticalSection(&d->lock);
  HRESULT hr = DI_OK;
  BOOL all;
  switch (id) {
    case 1: /* DIPROP_BUFFERSIZE */
    case 2: /* DIPROP_AXISMODE */
    case 15: /* DIPROP_JOYSTICKID */
    case 24: /* DIPROP_VIDPID */ {
      if (!header_ok(h, sizeof(DIPROPDWORD)) || h->dwHow != DIPH_DEVICE) { hr = DIERR_INVALIDPARAM; break; }
      DIPROPDWORD *p = (DIPROPDWORD *)h;
      p->dwData = id == 1 ? d->buffer.capacity : id == 2 ? DIPROPAXISMODE_ABS : id == 15 ? d->slot : (d->info.vendor | (d->info.product << 16));
      break;
    }
    case 3: /* DIPROP_GRANULARITY */
    case 5: /* DIPROP_DEADZONE */
    case 6: /* DIPROP_SATURATION */ {
      if (!header_ok(h, sizeof(DIPROPDWORD))) { hr = DIERR_INVALIDPARAM; break; }
      const int a = prop_axis(d, h, &all);
      if (all || a < 0) { hr = all ? DIERR_INVALIDPARAM : DIERR_OBJECTNOTFOUND; break; }
      DIPROPDWORD *p = (DIPROPDWORD *)h;
      p->dwData = id == 3 ? 1 : id == 5 ? d->props.deadzone[a] : d->props.saturation[a];
      break;
    }
    case 4: /* DIPROP_RANGE */
    case 18: /* DIPROP_PHYSICALRANGE */
    case 19: /* DIPROP_LOGICALRANGE */ {
      if (!header_ok(h, sizeof(DIPROPRANGE))) { hr = DIERR_INVALIDPARAM; break; }
      const int a = prop_axis(d, h, &all);
      if (all || a < 0) { hr = all ? DIERR_INVALIDPARAM : DIERR_OBJECTNOTFOUND; break; }
      DIPROPRANGE *p = (DIPROPRANGE *)h;
      if (id == 4) { p->lMin = d->props.min[a]; p->lMax = d->props.max[a]; }
      else { p->lMin = -32767; p->lMax = 32767; }
      break;
    }
    case 13: /* DIPROP_INSTANCENAME */
    case 14: /* DIPROP_PRODUCTNAME */
    case 26: /* DIPROP_TYPENAME */ {
      if (!header_ok(h, sizeof(DIPROPSTRING)) || h->dwHow != DIPH_DEVICE) { hr = DIERR_INVALIDPARAM; break; }
      DIPROPSTRING *p = (DIPROPSTRING *)h;
      if (id == 26) {
        char type[32] = {0};
        text_builder b = {type, 0, sizeof type};
        tb_text(&b, "VID_");
        tb_hex4(&b, (unsigned)d->info.vendor, TRUE);
        tb_text(&b, "&PID_");
        tb_hex4(&b, (unsigned)d->info.product, TRUE);
        widen(p->wsz, type, MAX_PATH);
      } else {
        widen(p->wsz, d->info.name, MAX_PATH);
      }
      break;
    }
    case 12: /* DIPROP_GUIDANDPATH */ {
      if (!header_ok(h, sizeof(DIPROPGUIDANDPATH)) || h->dwHow != DIPH_DEVICE) { hr = DIERR_INVALIDPARAM; break; }
      DIPROPGUIDANDPATH *p = (DIPROPGUIDANDPATH *)h;
      p->guidClass = kGuidHidClass;
      /* (A pad XInput serves too has "IG_" in its path, as on Windows: programs that use both skip it here.) */
      const BOOL xinput = d->info.protocol == PAD_PROTOCOL_XBOX360 || d->info.protocol == PAD_PROTOCOL_XBOXONE;
      char path[128] = {0};
      text_builder b = {path, 0, sizeof path};
      tb_text(&b, "\\\\?\\hid#vid_");
      tb_hex4(&b, (unsigned)d->info.vendor, FALSE);
      tb_text(&b, "&pid_");
      tb_hex4(&b, (unsigned)d->info.product, FALSE);
      if (xinput) tb_text(&b, "&ig_00");
      tb_text(&b, "#tcos&");
      tb_decimal(&b, (unsigned)d->slot);
      tb_text(&b, "#{4d1e55b2-f16f-11cf-88cb-001111000030}");
      widen(p->wszPath, path, MAX_PATH);
      break;
    }
    default:
      hr = DIERR_UNSUPPORTED;
      break;
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_set_property(PadDevice *d, REFGUID property, const DIPROPHEADER *h) {
  if (!property || !h) return E_POINTER;
  const ULONG_PTR id = (ULONG_PTR)property;
  if (id > 0xFFFF) return DIERR_UNSUPPORTED;
  EnterCriticalSection(&d->lock);
  HRESULT hr = DI_OK;
  BOOL all;
  switch (id) {
    case 1: /* DIPROP_BUFFERSIZE */ {
      if (!header_ok(h, sizeof(DIPROPDWORD)) || h->dwHow != DIPH_DEVICE) { hr = DIERR_INVALIDPARAM; break; }
      if (d->acquired) { hr = DIERR_ACQUIRED; break; }
      dj_set_buffer_size(&d->buffer, ((const DIPROPDWORD *)h)->dwData);
      if (((const DIPROPDWORD *)h)->dwData > DJ_MAX_BUFFER) hr = DI_PROPNOEFFECT;   /* (the buffer is capped) */
      break;
    }
    case 2: /* DIPROP_AXISMODE */
      if (!header_ok(h, sizeof(DIPROPDWORD))) { hr = DIERR_INVALIDPARAM; break; }
      hr = ((const DIPROPDWORD *)h)->dwData == DIPROPAXISMODE_ABS ? DI_OK : DIERR_UNSUPPORTED;   /* (a pad's axes are absolute) */
      break;
    case 5: /* DIPROP_DEADZONE */
    case 6: /* DIPROP_SATURATION */ {
      if (!header_ok(h, sizeof(DIPROPDWORD))) { hr = DIERR_INVALIDPARAM; break; }
      const DWORD value = ((const DIPROPDWORD *)h)->dwData;
      if (value > 10000) { hr = DIERR_INVALIDPARAM; break; }
      const int a = prop_axis(d, h, &all);
      if (a < 0) { hr = DIERR_OBJECTNOTFOUND; break; }
      for (int i = all ? 0 : a; i <= (all ? DJ_AXES - 1 : a); ++i) {
        if (id == 5) d->props.deadzone[i] = value;
        else d->props.saturation[i] = value;
      }
      break;
    }
    case 4: /* DIPROP_RANGE */ {
      if (!header_ok(h, sizeof(DIPROPRANGE))) { hr = DIERR_INVALIDPARAM; break; }
      const DIPROPRANGE *p = (const DIPROPRANGE *)h;
      if (p->lMin >= p->lMax) { hr = DIERR_INVALIDPARAM; break; }
      const int a = prop_axis(d, h, &all);
      if (a < 0) { hr = DIERR_OBJECTNOTFOUND; break; }
      for (int i = all ? 0 : a; i <= (all ? DJ_AXES - 1 : a); ++i) { d->props.min[i] = p->lMin; d->props.max[i] = p->lMax; }
      d->have_last = FALSE;       /* (values in the old range are not compared with new ones) */
      break;
    }
    case 9: /* DIPROP_AUTOCENTER */
    case 7: /* DIPROP_FFGAIN */
      hr = header_ok(h, sizeof(DIPROPDWORD)) ? DI_OK : DIERR_INVALIDPARAM;   /* (no force feedback: nothing to change) */
      break;
    default:
      hr = DIERR_UNSUPPORTED;
      break;
  }
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_cooperative(PadDevice *d, HWND window, DWORD flags) {
  (void)d; (void)window;
  const DWORD exclusive = flags & (DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE), focus = flags & (DISCL_FOREGROUND | DISCL_BACKGROUND);
  if ((exclusive != DISCL_EXCLUSIVE && exclusive != DISCL_NONEXCLUSIVE) || (focus != DISCL_FOREGROUND && focus != DISCL_BACKGROUND)) return DIERR_INVALIDPARAM;
  return DI_OK;
}

static HRESULT dev_event(PadDevice *d, HANDLE event) {
  EnterCriticalSection(&d->lock);
  const HRESULT hr = d->acquired ? DIERR_ACQUIRED : DI_OK;
  if (hr == DI_OK) d->event = event;
  LeaveCriticalSection(&d->lock);
  return hr;
}

static HRESULT dev_device_info(PadDevice *d, void *out, BOOL wide) {
  if (!out) return E_POINTER;
  return fill_instance(d->slot, &d->info, out, wide) ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT dev_initialize(PadDevice *d, HINSTANCE instance, DWORD version, REFGUID guid) {
  const HRESULT hr = check_version(instance, version);
  if (FAILED(hr)) return hr;
  if (!guid) return E_POINTER;
  return pad_slot_of(guid) == (int)d->slot ? DI_OK : DIERR_DEVICENOTREG;
}

/* The A table. */
#define DA_(name) static HRESULT WINAPI name
static HRESULT WINAPI da_query(IDirectInputDevice8A *i, REFIID riid, void **out) { return dev_query(dev_a(i), riid, out); }
static ULONG WINAPI da_add_ref(IDirectInputDevice8A *i) { return dev_add_ref(dev_a(i)); }
static ULONG WINAPI da_release(IDirectInputDevice8A *i) { return dev_release(dev_a(i)); }
DA_(da_caps)(IDirectInputDevice8A *i, LPDIDEVCAPS c) { return dev_capabilities(dev_a(i), c); }
DA_(da_enum_objects)(IDirectInputDevice8A *i, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ctx, DWORD flags) { return dev_enum_objects(dev_a(i), (void *)cb, ctx, flags, FALSE); }
DA_(da_get_property)(IDirectInputDevice8A *i, REFGUID p, LPDIPROPHEADER h) { return dev_get_property(dev_a(i), p, h); }
DA_(da_set_property)(IDirectInputDevice8A *i, REFGUID p, LPCDIPROPHEADER h) { return dev_set_property(dev_a(i), p, h); }
DA_(da_acquire)(IDirectInputDevice8A *i) { return dev_acquire(dev_a(i)); }
DA_(da_unacquire)(IDirectInputDevice8A *i) { return dev_unacquire(dev_a(i)); }
DA_(da_state)(IDirectInputDevice8A *i, DWORD size, LPVOID data) { return dev_state(dev_a(i), size, data); }
DA_(da_data)(IDirectInputDevice8A *i, DWORD size, LPDIDEVICEOBJECTDATA out, LPDWORD count, DWORD flags) { return dev_data(dev_a(i), size, out, count, flags); }
DA_(da_format)(IDirectInputDevice8A *i, LPCDIDATAFORMAT f) { return dev_set_format(dev_a(i), f); }
DA_(da_event)(IDirectInputDevice8A *i, HANDLE e) { return dev_event(dev_a(i), e); }
DA_(da_coop)(IDirectInputDevice8A *i, HWND w, DWORD flags) { return dev_cooperative(dev_a(i), w, flags); }
DA_(da_object_info)(IDirectInputDevice8A *i, LPDIDEVICEOBJECTINSTANCEA o, DWORD which, DWORD how) { return dev_object_info(dev_a(i), o, which, how, FALSE); }
DA_(da_device_info)(IDirectInputDevice8A *i, LPDIDEVICEINSTANCEA o) { return dev_device_info(dev_a(i), o, FALSE); }
DA_(da_panel)(IDirectInputDevice8A *i, HWND w, DWORD flags) { (void)i; (void)w; return flags ? DIERR_INVALIDPARAM : DI_OK; }
DA_(da_init)(IDirectInputDevice8A *i, HINSTANCE inst, DWORD v, REFGUID g) { return dev_initialize(dev_a(i), inst, v, g); }
DA_(da_create_effect)(IDirectInputDevice8A *i, REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer) {
  (void)i; (void)g; (void)e; (void)outer;
  if (out) *out = NULL;
  return DIERR_UNSUPPORTED;
}
DA_(da_enum_effects)(IDirectInputDevice8A *i, LPDIENUMEFFECTSCALLBACKA cb, LPVOID ctx, DWORD type) { (void)i; (void)ctx; (void)type; return cb ? DI_OK : DIERR_INVALIDPARAM; }
DA_(da_effect_info)(IDirectInputDevice8A *i, LPDIEFFECTINFOA info, REFGUID g) { (void)i; (void)info; (void)g; return DIERR_DEVICENOTREG; }
DA_(da_ff_state)(IDirectInputDevice8A *i, LPDWORD out) { (void)i; (void)out; return DIERR_UNSUPPORTED; }
DA_(da_ff_command)(IDirectInputDevice8A *i, DWORD flags) { (void)i; (void)flags; return DIERR_UNSUPPORTED; }
DA_(da_enum_created)(IDirectInputDevice8A *i, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ctx, DWORD flags) { (void)i; (void)ctx; return cb && !flags ? DI_OK : DIERR_INVALIDPARAM; }
DA_(da_escape)(IDirectInputDevice8A *i, LPDIEFFESCAPE e) { (void)i; (void)e; return DIERR_UNSUPPORTED; }
DA_(da_poll)(IDirectInputDevice8A *i) { return dev_poll(dev_a(i)); }
DA_(da_send)(IDirectInputDevice8A *i, DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) { (void)i; (void)size; (void)data; (void)count; (void)flags; return DIERR_UNSUPPORTED; }
DA_(da_effects_file)(IDirectInputDevice8A *i, LPCSTR file, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ctx, DWORD flags) { (void)i; (void)file; (void)cb; (void)ctx; (void)flags; return DIERR_UNSUPPORTED; }
DA_(da_write_file)(IDirectInputDevice8A *i, LPCSTR file, DWORD n, LPDIFILEEFFECT e, DWORD flags) { (void)i; (void)file; (void)n; (void)e; (void)flags; return DIERR_UNSUPPORTED; }
DA_(da_build_map)(IDirectInputDevice8A *i, LPDIACTIONFORMATA f, LPCSTR user, DWORD flags) { (void)i; (void)f; (void)user; (void)flags; return DIERR_UNSUPPORTED; }
DA_(da_set_map)(IDirectInputDevice8A *i, LPDIACTIONFORMATA f, LPCSTR user, DWORD flags) { (void)i; (void)f; (void)user; (void)flags; return DIERR_UNSUPPORTED; }
DA_(da_image)(IDirectInputDevice8A *i, LPDIDEVICEIMAGEINFOHEADERA h) { (void)i; (void)h; return DIERR_UNSUPPORTED; }
static const IDirectInputDevice8AVtbl kDeviceVtblA = {
    da_query, da_add_ref, da_release, da_caps, da_enum_objects, da_get_property, da_set_property, da_acquire, da_unacquire, da_state, da_data,
    da_format, da_event, da_coop, da_object_info, da_device_info, da_panel, da_init, da_create_effect, da_enum_effects, da_effect_info,
    da_ff_state, da_ff_command, da_enum_created, da_escape, da_poll, da_send, da_effects_file, da_write_file, da_build_map, da_set_map, da_image};

/* The W table. */
static HRESULT WINAPI dw_query(IDirectInputDevice8W *i, REFIID riid, void **out) { return dev_query(dev_w(i), riid, out); }
static ULONG WINAPI dw_add_ref(IDirectInputDevice8W *i) { return dev_add_ref(dev_w(i)); }
static ULONG WINAPI dw_release(IDirectInputDevice8W *i) { return dev_release(dev_w(i)); }
DA_(dw_caps)(IDirectInputDevice8W *i, LPDIDEVCAPS c) { return dev_capabilities(dev_w(i), c); }
DA_(dw_enum_objects)(IDirectInputDevice8W *i, LPDIENUMDEVICEOBJECTSCALLBACKW cb, LPVOID ctx, DWORD flags) { return dev_enum_objects(dev_w(i), (void *)cb, ctx, flags, TRUE); }
DA_(dw_get_property)(IDirectInputDevice8W *i, REFGUID p, LPDIPROPHEADER h) { return dev_get_property(dev_w(i), p, h); }
DA_(dw_set_property)(IDirectInputDevice8W *i, REFGUID p, LPCDIPROPHEADER h) { return dev_set_property(dev_w(i), p, h); }
DA_(dw_acquire)(IDirectInputDevice8W *i) { return dev_acquire(dev_w(i)); }
DA_(dw_unacquire)(IDirectInputDevice8W *i) { return dev_unacquire(dev_w(i)); }
DA_(dw_state)(IDirectInputDevice8W *i, DWORD size, LPVOID data) { return dev_state(dev_w(i), size, data); }
DA_(dw_data)(IDirectInputDevice8W *i, DWORD size, LPDIDEVICEOBJECTDATA out, LPDWORD count, DWORD flags) { return dev_data(dev_w(i), size, out, count, flags); }
DA_(dw_format)(IDirectInputDevice8W *i, LPCDIDATAFORMAT f) { return dev_set_format(dev_w(i), f); }
DA_(dw_event)(IDirectInputDevice8W *i, HANDLE e) { return dev_event(dev_w(i), e); }
DA_(dw_coop)(IDirectInputDevice8W *i, HWND w, DWORD flags) { return dev_cooperative(dev_w(i), w, flags); }
DA_(dw_object_info)(IDirectInputDevice8W *i, LPDIDEVICEOBJECTINSTANCEW o, DWORD which, DWORD how) { return dev_object_info(dev_w(i), o, which, how, TRUE); }
DA_(dw_device_info)(IDirectInputDevice8W *i, LPDIDEVICEINSTANCEW o) { return dev_device_info(dev_w(i), o, TRUE); }
DA_(dw_panel)(IDirectInputDevice8W *i, HWND w, DWORD flags) { (void)i; (void)w; return flags ? DIERR_INVALIDPARAM : DI_OK; }
DA_(dw_init)(IDirectInputDevice8W *i, HINSTANCE inst, DWORD v, REFGUID g) { return dev_initialize(dev_w(i), inst, v, g); }
DA_(dw_create_effect)(IDirectInputDevice8W *i, REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer) {
  (void)i; (void)g; (void)e; (void)outer;
  if (out) *out = NULL;
  return DIERR_UNSUPPORTED;
}
DA_(dw_enum_effects)(IDirectInputDevice8W *i, LPDIENUMEFFECTSCALLBACKW cb, LPVOID ctx, DWORD type) { (void)i; (void)ctx; (void)type; return cb ? DI_OK : DIERR_INVALIDPARAM; }
DA_(dw_effect_info)(IDirectInputDevice8W *i, LPDIEFFECTINFOW info, REFGUID g) { (void)i; (void)info; (void)g; return DIERR_DEVICENOTREG; }
DA_(dw_ff_state)(IDirectInputDevice8W *i, LPDWORD out) { (void)i; (void)out; return DIERR_UNSUPPORTED; }
DA_(dw_ff_command)(IDirectInputDevice8W *i, DWORD flags) { (void)i; (void)flags; return DIERR_UNSUPPORTED; }
DA_(dw_enum_created)(IDirectInputDevice8W *i, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ctx, DWORD flags) { (void)i; (void)ctx; return cb && !flags ? DI_OK : DIERR_INVALIDPARAM; }
DA_(dw_escape)(IDirectInputDevice8W *i, LPDIEFFESCAPE e) { (void)i; (void)e; return DIERR_UNSUPPORTED; }
DA_(dw_poll)(IDirectInputDevice8W *i) { return dev_poll(dev_w(i)); }
DA_(dw_send)(IDirectInputDevice8W *i, DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) { (void)i; (void)size; (void)data; (void)count; (void)flags; return DIERR_UNSUPPORTED; }
DA_(dw_effects_file)(IDirectInputDevice8W *i, LPCWSTR file, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ctx, DWORD flags) { (void)i; (void)file; (void)cb; (void)ctx; (void)flags; return DIERR_UNSUPPORTED; }
DA_(dw_write_file)(IDirectInputDevice8W *i, LPCWSTR file, DWORD n, LPDIFILEEFFECT e, DWORD flags) { (void)i; (void)file; (void)n; (void)e; (void)flags; return DIERR_UNSUPPORTED; }
DA_(dw_build_map)(IDirectInputDevice8W *i, LPDIACTIONFORMATW f, LPCWSTR user, DWORD flags) { (void)i; (void)f; (void)user; (void)flags; return DIERR_UNSUPPORTED; }
DA_(dw_set_map)(IDirectInputDevice8W *i, LPDIACTIONFORMATW f, LPCWSTR user, DWORD flags) { (void)i; (void)f; (void)user; (void)flags; return DIERR_UNSUPPORTED; }
DA_(dw_image)(IDirectInputDevice8W *i, LPDIDEVICEIMAGEINFOHEADERW h) { (void)i; (void)h; return DIERR_UNSUPPORTED; }
static const IDirectInputDevice8WVtbl kDeviceVtblW = {
    dw_query, dw_add_ref, dw_release, dw_caps, dw_enum_objects, dw_get_property, dw_set_property, dw_acquire, dw_unacquire, dw_state, dw_data,
    dw_format, dw_event, dw_coop, dw_object_info, dw_device_info, dw_panel, dw_init, dw_create_effect, dw_enum_effects, dw_effect_info,
    dw_ff_state, dw_ff_command, dw_enum_created, dw_escape, dw_poll, dw_send, dw_effects_file, dw_write_file, dw_build_map, dw_set_map, dw_image};
#undef DA_

/* CreateDevice for a pad: by its instance GUID (a slot) or its product GUID (the first slot with that pad). */
static HRESULT create_pad(const GUID *guid, void **out, BOOL wide) {
  int slot = pad_slot_of(guid);
  if (slot < 0) slot = pad_slot_of_product(guid);
  if (slot < 0) return DIERR_DEVICENOTREG;
  pad_info info;
  if (!pad_query((DWORD)slot, &info)) return DIERR_DEVICENOTREG;
  PadDevice *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
  if (!d) return DIERR_OUTOFMEMORY;
  d->vtbl_a = &kDeviceVtblA;
  d->vtbl_w = &kDeviceVtblW;
  d->refs = 1;
  d->slot = (DWORD)slot;
  d->info = info;
  InitializeCriticalSection(&d->lock);
  dj_default_props(&d->props);
  *out = wide ? (void *)&d->vtbl_w : (void *)&d->vtbl_a;
  return DI_OK;
}

static BOOL pad_attached(const GUID *guid) {
  const int slot = pad_slot_of(guid);
  pad_info info;
  return slot >= 0 && pad_query((DWORD)slot, &info);
}
