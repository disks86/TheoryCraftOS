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

/* OleTranslateColor, the DLL's class objects and registration. */

/* ---- OleTranslateColor ---------------------------------------------------------------------------------------------------------------------------------------- */
/* An OLE_COLOR is a COLORREF, or: 0x80 in the top byte and a system color index, 0x01 and a palette index, 0x02 and a palette-relative RGB. */
EXPORT HRESULT WINAPI OleTranslateColor(OLE_COLOR color, HPALETTE palette, COLORREF *out) {
  COLORREF result;
  switch ((DWORD)color >> 24) {
  case 0x80: {
    unsigned index = (DWORD)color & 0xFF;
    if (index > COLOR_MENUBAR) return E_INVALIDARG;
    result = so_GetSysColor((int)index);
    break;
  }
  case 0x01: {
    PALETTEENTRY entry;
    if (!palette) palette = (HPALETTE)so_GetStockObject(DEFAULT_PALETTE);
    if (palette && so_GetPaletteEntries(palette, (DWORD)color & 0xFFFF, 1, &entry) == 1) result = RGB(entry.peRed, entry.peGreen, entry.peBlue);
    else result = (DWORD)color & 0x00FFFFFF;
    break;
  }
  case 0x02:
    result = (DWORD)color & 0x00FFFFFF;
    if (palette) {
      PALETTEENTRY entry;
      UINT index = so_GetNearestPaletteIndex(palette, result);
      if (index != 0xFFFFFFFFu && so_GetPaletteEntries(palette, index, 1, &entry) == 1) result = RGB(entry.peRed, entry.peGreen, entry.peBlue);
    }
    break;
  default:
    result = (DWORD)color;
    break;
  }
  if (out) *out = result;
  return S_OK;
}

/* ---- class objects ---------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct so_factory {
  IClassFactory iface;
  BOOL is_font;
} so_factory;

static const IClassFactoryVtbl so_factory_vtbl;
static so_factory so_font_factory = {{(void *)&so_factory_vtbl}, TRUE};
static so_factory so_pic_factory = {{(void *)&so_factory_vtbl}, FALSE};

static HRESULT WINAPI so_factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *ppv = iface; return S_OK; }
  *ppv = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI so_factory_AddRef(IClassFactory *iface) { (void)iface; return 2; }
static ULONG WINAPI so_factory_Release(IClassFactory *iface) { (void)iface; return 1; }
static HRESULT WINAPI so_factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv) {
  so_factory *f = CONTAINING_RECORD(iface, so_factory, iface);
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  return f->is_font ? so_font_create(NULL, riid, ppv) : so_pic_create(NULL, TRUE, riid, ppv);
}
static HRESULT WINAPI so_factory_LockServer(IClassFactory *iface, BOOL lock) {
  (void)iface;
  if (lock) __sync_add_and_fetch(&so_server_locks, 1);
  else __sync_sub_and_fetch(&so_server_locks, 1);
  return S_OK;
}
static const IClassFactoryVtbl so_factory_vtbl = {so_factory_QueryInterface, so_factory_AddRef, so_factory_Release, so_factory_CreateInstance, so_factory_LockServer};

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppv) {
  if (!ppv) return E_POINTER;
  *ppv = NULL;
  if (IsEqualCLSID(clsid, &so_CLSID_StdFont)) return IClassFactory_QueryInterface(&so_font_factory.iface, riid, ppv);
  if (IsEqualCLSID(clsid, &so_CLSID_StdPicture)) return IClassFactory_QueryInterface(&so_pic_factory.iface, riid, ppv);
  return CLASS_E_CLASSNOTAVAILABLE;
}

EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return so_objects == 0 && so_server_locks == 0 ? S_OK : S_FALSE; }

/* ---- registration ---------------------------------------------------------------------------------------------------------------------------------------------- */
static void so_guid_string(const GUID *g, WCHAR out[39]) {
  static const char hex[] = "0123456789ABCDEF";
  const BYTE *b = (const BYTE *)g;
  /* the layout in text: 8-4-4-4-12; the first three fields are little endian numbers */
  static const BYTE order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  int o = 0;
  out[o++] = '{';
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = (WCHAR)hex[b[order[i]] >> 4];
    out[o++] = (WCHAR)hex[b[order[i]] & 15];
  }
  out[o++] = '}';
  out[o] = 0;
}

static LONG so_reg_set(const WCHAR *key_path, const WCHAR *name, const WCHAR *value) {
  HKEY key;
  LONG error = RegCreateKeyExW(HKEY_CLASSES_ROOT, key_path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
  if (error != ERROR_SUCCESS) return error;
  error = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((oa_wcslen(value) + 1) * sizeof(WCHAR)));
  RegCloseKey(key);
  return error;
}

/* "CLSID\{guid}\<tail>" */
static void so_clsid_path(WCHAR *out, const WCHAR *guid, const WCHAR *tail) {
  static const WCHAR prefix[] = {'C','L','S','I','D','\\',0};
  SIZE_T o = 0;
  for (const WCHAR *p = prefix; *p; p++) out[o++] = *p;
  for (const WCHAR *p = guid; *p; p++) out[o++] = *p;
  if (tail && *tail) { out[o++] = '\\'; for (const WCHAR *p = tail; *p; p++) out[o++] = *p; }
  out[o] = 0;
}

typedef struct { const CLSID *clsid; const WCHAR *title; const WCHAR *progid; } so_class_info;
static const WCHAR so_title_font[] = {'S','t','a','n','d','a','r','d',' ','F','o','n','t',0};
static const WCHAR so_title_pic[] = {'S','t','a','n','d','a','r','d',' ','P','i','c','t','u','r','e',0};
static const WCHAR so_progid_font[] = {'S','t','d','F','o','n','t',0};
static const WCHAR so_progid_pic[] = {'S','t','d','P','i','c','t','u','r','e',0};

EXPORT HRESULT WINAPI DllRegisterServer(void) {
  static const WCHAR server[] = {'o','l','e','a','u','t','3','2','.','d','l','l',0};
  static const WCHAR inproc[] = {'I','n','p','r','o','c','S','e','r','v','e','r','3','2',0};
  static const WCHAR threading[] = {'T','h','r','e','a','d','i','n','g','M','o','d','e','l',0};
  static const WCHAR both[] = {'B','o','t','h',0};
  static const WCHAR progid_key[] = {'P','r','o','g','I','D',0};
  static const WCHAR clsid_key[] = {'C','L','S','I','D',0};
  const so_class_info classes[2] = {{&so_CLSID_StdFont, so_title_font, so_progid_font}, {&so_CLSID_StdPicture, so_title_pic, so_progid_pic}};
  for (int i = 0; i < 2; i++) {
    WCHAR guid[39], path[96], progid_path[48];
    so_guid_string(classes[i].clsid, guid);
    so_clsid_path(path, guid, NULL);
    LONG error = so_reg_set(path, NULL, classes[i].title);
    if (error == ERROR_SUCCESS) { so_clsid_path(path, guid, inproc); error = so_reg_set(path, NULL, server); }
    if (error == ERROR_SUCCESS) error = so_reg_set(path, threading, both);
    if (error == ERROR_SUCCESS) { so_clsid_path(path, guid, progid_key); error = so_reg_set(path, NULL, classes[i].progid); }
    if (error == ERROR_SUCCESS) error = so_reg_set(classes[i].progid, NULL, classes[i].title);
    if (error == ERROR_SUCCESS) {
      SIZE_T o = 0;
      for (const WCHAR *p = classes[i].progid; *p; p++) progid_path[o++] = *p;
      progid_path[o++] = '\\';
      for (const WCHAR *p = clsid_key; *p; p++) progid_path[o++] = *p;
      progid_path[o] = 0;
      error = so_reg_set(progid_path, NULL, guid);
    }
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
  }
  return S_OK;
}

EXPORT HRESULT WINAPI DllUnregisterServer(void) {
  static const WCHAR inproc[] = {'I','n','p','r','o','c','S','e','r','v','e','r','3','2',0};
  static const WCHAR progid_key[] = {'P','r','o','g','I','D',0};
  const so_class_info classes[2] = {{&so_CLSID_StdFont, so_title_font, so_progid_font}, {&so_CLSID_StdPicture, so_title_pic, so_progid_pic}};
  for (int i = 0; i < 2; i++) {
    WCHAR guid[39], path[96], progid_path[48];
    so_guid_string(classes[i].clsid, guid);
    so_clsid_path(path, guid, inproc); RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
    so_clsid_path(path, guid, progid_key); RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
    so_clsid_path(path, guid, NULL); RegDeleteKeyW(HKEY_CLASSES_ROOT, path);
    SIZE_T o = 0;
    for (const WCHAR *p = classes[i].progid; *p; p++) progid_path[o++] = *p;
    progid_path[o++] = '\\'; progid_path[o++] = 'C'; progid_path[o++] = 'L'; progid_path[o++] = 'S'; progid_path[o++] = 'I'; progid_path[o++] = 'D'; progid_path[o] = 0;
    RegDeleteKeyW(HKEY_CLASSES_ROOT, progid_path);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, classes[i].progid);
  }
  return S_OK;
}
