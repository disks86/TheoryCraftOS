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

/* Item id lists and the shell folders that turn them into names and objects. A list is a series of items (a 16-bit size, then bytes the folder that made the item understands) ending
 * in a size of 0. The items made here:
 *   PT_GUID 0x1F    [size 2][type][sort][CLSID 16]                           a virtual folder: This PC, the Recycle Bin, Network ...
 *   PT_DRIVE 0x23   [size 2][type]["C:\" and 0 as ANSI][zeros to 25 bytes]
 *   PT_SHARE 0xC3   [size 2][type][pad][UTF-16 "\\server\share" and 0]
 *   PT_FOLDERW 0x31 / PT_VALUEW 0x32 (a folder / a file)
 *                   [size 2][type][pad][size 4][DOS date 2][DOS time 2][attributes 2][UTF-16 name and 0]
 * A list for a path is This PC, the drive, the folders ... except under the desktop folder, where the items start with the folder or file directly. Included by shell32.c. */

#define PT_GUID 0x1F
#define PT_DRIVE 0x23
#define PT_FOLDERW 0x31
#define PT_VALUEW 0x32
#define PT_SHARE 0xC3
#define ITEM_HEADER 4

typedef struct { USHORT cb; BYTE type; BYTE pad; } ItemHeader;
typedef struct { USHORT cb; BYTE type; BYTE sort; GUID clsid; } GuidItem;
typedef struct { USHORT cb; BYTE type; BYTE pad; DWORD size; WORD date, time, attributes; WCHAR name[1]; } FileItem;
#define FILE_ITEM_FIXED 14      /* the part of a file item before the name */

static const GUID CLSID_MyComputer_ = {0x20D04FE0, 0x3AEA, 0x1069, {0xA2, 0xD8, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D}};
static const GUID CLSID_RecycleBin_ = {0x645FF040, 0x5081, 0x101B, {0x9F, 0x08, 0x00, 0xAA, 0x00, 0x2F, 0x95, 0x4E}};
static const GUID CLSID_Network_ = {0xF02C1A0D, 0xBE21, 0x4350, {0x88, 0xB0, 0x73, 0x67, 0xFC, 0x96, 0xEF, 0x3C}};
static const GUID CLSID_ShellDesktop_ = {0x00021400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID CLSID_ShellFSFolder_ = {0xF3364BA0, 0x65B9, 0x11CE, {0xA9, 0xBA, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};
static const GUID CLSID_ControlPanel_ = {0x21EC2020, 0x3AEA, 0x1069, {0xA2, 0xDD, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D}};
static const GUID CLSID_Printers_ = {0x2227A280, 0x3AEA, 0x1069, {0xA2, 0xDE, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D}};

static const struct { const GUID *clsid; const WCHAR *name; BYTE sort; } g_virtual_folders[] = {
  {&CLSID_MyComputer_, L"This PC", 0x50}, {&CLSID_RecycleBin_, L"Recycle Bin", 0x78}, {&CLSID_Network_, L"Network", 0x58}, {&CLSID_ControlPanel_, L"Control Panel", 0x70},
  {&CLSID_Printers_, L"Printers", 0x80},
};

/* ---- the list functions --------------------------------------------------------------------------------------------------------------------------------- */

static LPITEMIDLIST il_new(size_t bytes) {
  LPITEMIDLIST p = SHAlloc(bytes);
  if (p) ZeroMemory(p, bytes);      /* (an empty list must end in its 0) */
  return p;
}
static USHORT item_size(LPCITEMIDLIST id) { return id->mkid.cb; }

EXPORT LPITEMIDLIST WINAPI ILGetNext(LPCITEMIDLIST pidl) {
  if (!pidl || !pidl->mkid.cb) return NULL;
  return (LPITEMIDLIST)((const BYTE *)pidl + pidl->mkid.cb);
}
EXPORT UINT WINAPI ILGetSize(LPCITEMIDLIST pidl) {
  if (!pidl) return 0;
  UINT total = sizeof(USHORT);
  for (const ITEMIDLIST *p = pidl; p->mkid.cb; p = (const ITEMIDLIST *)((const BYTE *)p + p->mkid.cb)) total += p->mkid.cb;
  return total;
}
EXPORT LPITEMIDLIST WINAPI ILFindLastID(LPCITEMIDLIST pidl) {
  if (!pidl) return NULL;
  const ITEMIDLIST *last = pidl;
  for (const ITEMIDLIST *p = pidl; p->mkid.cb; p = (const ITEMIDLIST *)((const BYTE *)p + p->mkid.cb)) last = p;
  return (LPITEMIDLIST)last;
}
EXPORT BOOL WINAPI ILRemoveLastID(LPITEMIDLIST pidl) {
  if (!pidl || !pidl->mkid.cb) return FALSE;
  ITEMIDLIST *last = ILFindLastID(pidl);
  last->mkid.cb = 0;
  return TRUE;
}
EXPORT LPITEMIDLIST WINAPI ILClone(LPCITEMIDLIST pidl) {
  if (!pidl) return NULL;
  const UINT n = ILGetSize(pidl);
  LPITEMIDLIST copy = il_new(n);
  if (copy) CopyMemory(copy, pidl, n);
  return copy;
}
EXPORT LPITEMIDLIST WINAPI ILCloneFirst(LPCITEMIDLIST pidl) {
  if (!pidl) return NULL;
  const UINT n = pidl->mkid.cb;
  LPITEMIDLIST copy = il_new(n + sizeof(USHORT));
  if (copy) { CopyMemory(copy, pidl, n); *(USHORT *)((BYTE *)copy + n) = 0; }
  return copy;
}
EXPORT LPITEMIDLIST WINAPI ILCombine(LPCITEMIDLIST a, LPCITEMIDLIST b) {
  if (!a && !b) return NULL;
  if (!a) return ILClone(b);
  if (!b) return ILClone(a);
  const UINT na = ILGetSize(a) - sizeof(USHORT), nb = ILGetSize(b);
  LPITEMIDLIST r = il_new(na + nb);
  if (r) { CopyMemory(r, a, na); CopyMemory((BYTE *)r + na, b, nb); }
  return r;
}
EXPORT void WINAPI ILFree(LPITEMIDLIST pidl) { SHFree(pidl); }
/* Appends (or, with `at_end` false, prepends) one item; the old list is freed. */
EXPORT LPITEMIDLIST WINAPI ILAppendID(LPITEMIDLIST pidl, LPCSHITEMID item, BOOL at_end) {
  if (!item) return NULL;
  if (!pidl) {
    LPITEMIDLIST r = il_new((size_t)item->cb + sizeof(USHORT));
    if (r) CopyMemory(r, item, item->cb);
    return r;
  }
  const UINT n = ILGetSize(pidl);
  LPITEMIDLIST r = il_new(n + item->cb);
  if (!r) return NULL;
  if (at_end) {
    CopyMemory(r, pidl, n - sizeof(USHORT));
    CopyMemory((BYTE *)r + n - sizeof(USHORT), item, item->cb);
    *(USHORT *)((BYTE *)r + n - sizeof(USHORT) + item->cb) = 0;
  } else {
    CopyMemory(r, item, item->cb);
    CopyMemory((BYTE *)r + item->cb, pidl, n);
  }
  SHFree(pidl);
  return r;
}
EXPORT BOOL WINAPI ILIsEqual(LPCITEMIDLIST a, LPCITEMIDLIST b) {
  if (a == b) return TRUE;
  if (!a || !b) return FALSE;
  const UINT n = ILGetSize(a);
  return n == ILGetSize(b) && !memcmp(a, b, n);
}
/* Is `parent` the start of `child`? With `immediate` it must be the direct parent. */
EXPORT BOOL WINAPI ILIsParent(LPCITEMIDLIST parent, LPCITEMIDLIST child, BOOL immediate) {
  if (!parent || !child) return FALSE;
  const ITEMIDLIST *p = parent, *c = child;
  while (p->mkid.cb) {
    if (!c->mkid.cb || p->mkid.cb != c->mkid.cb || memcmp(p, c, p->mkid.cb)) return FALSE;
    p = (const ITEMIDLIST *)((const BYTE *)p + p->mkid.cb);
    c = (const ITEMIDLIST *)((const BYTE *)c + c->mkid.cb);
  }
  if (!c->mkid.cb) return FALSE;
  if (immediate) return ((const ITEMIDLIST *)((const BYTE *)c + c->mkid.cb))->mkid.cb == 0;
  return TRUE;
}
/* The part of `child` after `parent`, or NULL if parent is not a start of it. */
EXPORT LPITEMIDLIST WINAPI ILFindChild(PIDLIST_ABSOLUTE parent, PCIDLIST_ABSOLUTE child) {
  if (!parent || !child) return NULL;
  const ITEMIDLIST *p = parent, *c = child;
  while (p->mkid.cb) {
    if (!c->mkid.cb || p->mkid.cb != c->mkid.cb || memcmp(p, c, p->mkid.cb)) return NULL;
    p = (const ITEMIDLIST *)((const BYTE *)p + p->mkid.cb);
    c = (const ITEMIDLIST *)((const BYTE *)c + c->mkid.cb);
  }
  return (LPITEMIDLIST)c;
}

/* Lists in a stream: the size (16 bits, with the end mark) then the list. */
EXPORT HRESULT WINAPI ILSaveToStream(IStream *stream, LPCITEMIDLIST pidl) {
  if (!stream) return E_INVALIDARG;
  const UINT n = ILGetSize(pidl);
  USHORT size = (USHORT)n;
  ULONG done = 0;
  HRESULT hr = IStream_Write(stream, &size, sizeof(size), &done);
  if (FAILED(hr)) return hr;
  if (n) hr = IStream_Write(stream, pidl, n, &done);
  return hr;
}
EXPORT HRESULT WINAPI ILLoadFromStream(IStream *stream, LPITEMIDLIST *pidl) {
  USHORT size = 0;
  ULONG got = 0;
  if (!stream || !pidl) return E_INVALIDARG;
  if (*pidl) { SHFree(*pidl); *pidl = NULL; }
  HRESULT hr = IStream_Read(stream, &size, sizeof(size), &got);
  if (FAILED(hr)) return hr;
  if (got != sizeof(size)) return E_FAIL;
  if (!size) return S_OK;
  LPITEMIDLIST p = il_new(size);
  if (!p) return E_OUTOFMEMORY;
  hr = IStream_Read(stream, p, size, &got);
  if (FAILED(hr) || got != size) { SHFree(p); return FAILED(hr) ? hr : E_FAIL; }
  *pidl = p;
  return S_OK;
}
EXPORT HRESULT WINAPI ILLoadFromStreamEx(IStream *stream, LPITEMIDLIST *pidl) { return ILLoadFromStream(stream, pidl); }

/* ---- making items ------------------------------------------------------------------------------------------------------------------------------------------ */

static LPITEMIDLIST il_end_of(LPITEMIDLIST pidl) {
  while (pidl->mkid.cb) pidl = (LPITEMIDLIST)((BYTE *)pidl + pidl->mkid.cb);
  return pidl;
}
/* A one-item list from the raw item bytes. */
static LPITEMIDLIST il_from_item(const void *item, size_t size) {
  LPITEMIDLIST r = il_new(size + sizeof(USHORT));
  if (r) CopyMemory(r, item, size);
  return r;
}
static LPITEMIDLIST il_guid(const GUID *clsid) {
  GuidItem item;
  ZeroMemory(&item, sizeof(item));
  item.cb = sizeof(item);
  item.type = IsEqualGUID(clsid, &CLSID_Printers_) ? 0x70 : PT_GUID;      /* (PT_YAGUID for the printers) */
  item.sort = 0x50;
  for (size_t i = 0; i < ARRAY_LEN(g_virtual_folders); ++i) if (IsEqualGUID(g_virtual_folders[i].clsid, clsid)) item.sort = g_virtual_folders[i].sort;
  item.clsid = *clsid;
  return il_from_item(&item, sizeof(item));
}
static LPITEMIDLIST il_drive(const WCHAR *root) {
  BYTE item[25];
  ZeroMemory(item, sizeof(item));
  *(USHORT *)item = sizeof(item);
  item[2] = PT_DRIVE;
  item[3] = (BYTE)wup(root[0]);
  item[4] = ':';
  item[5] = '\\';
  return il_from_item(item, sizeof(item));
}
static LPITEMIDLIST il_share(const WCHAR *unc) {
  const size_t n = wlen(unc);
  const size_t size = ITEM_HEADER + (n + 1) * sizeof(WCHAR);
  BYTE *item = mem_alloc(size);
  if (!item) return NULL;
  *(USHORT *)item = (USHORT)size;
  item[2] = PT_SHARE;
  CopyMemory(item + ITEM_HEADER, unc, (n + 1) * sizeof(WCHAR));
  LPITEMIDLIST r = il_from_item(item, size);
  mem_free(item);
  return r;
}
/* A file or folder item from the facts about it. */
static LPITEMIDLIST il_file(const WCHAR *name, DWORD attributes, ULONGLONG size, const FILETIME *modified) {
  const size_t n = wlen(name);
  const size_t total = FILE_ITEM_FIXED + (n + 1) * sizeof(WCHAR);
  if (total > 0xFFF0) return NULL;
  BYTE *raw = mem_alloc(total + sizeof(USHORT));
  if (!raw) return NULL;
  FileItem *item = (FileItem *)raw;
  item->cb = (USHORT)total;
  item->type = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? PT_FOLDERW : PT_VALUEW;
  item->size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (DWORD)size;
  item->attributes = (WORD)attributes;
  WORD date = 0, time = 0;
  if (modified && (modified->dwLowDateTime || modified->dwHighDateTime)) FileTimeToDosDateTime(modified, &date, &time);
  item->date = date;
  item->time = time;
  CopyMemory(raw + FILE_ITEM_FIXED, name, (n + 1) * sizeof(WCHAR));
  return (LPITEMIDLIST)raw;
}
static LPITEMIDLIST il_from_find(const WIN32_FIND_DATAW *fd) {
  return il_file(fd->cFileName, fd->dwFileAttributes, ((ULONGLONG)fd->nFileSizeHigh << 32) | fd->nFileSizeLow, &fd->ftLastWriteTime);
}

/* What kind an item is. */
static BOOL item_is_file_item(const ITEMIDLIST *id) { return id->mkid.cb >= FILE_ITEM_FIXED + 2 && (((const ItemHeader *)id)->type == PT_FOLDERW || ((const ItemHeader *)id)->type == PT_VALUEW); }
static BOOL item_is_folder_item(const ITEMIDLIST *id) { return item_is_file_item(id) && ((const ItemHeader *)id)->type == PT_FOLDERW; }
static BOOL item_is_drive(const ITEMIDLIST *id) { return id->mkid.cb >= 7 && ((const ItemHeader *)id)->type == PT_DRIVE; }
static BOOL item_is_share(const ITEMIDLIST *id) { return id->mkid.cb >= ITEM_HEADER + 2 && ((const ItemHeader *)id)->type == PT_SHARE; }
static const GUID *item_guid(const ITEMIDLIST *id) {
  if (id->mkid.cb == sizeof(GuidItem) && ((const ItemHeader *)id)->type == PT_GUID) return &((const GuidItem *)id)->clsid;
  return NULL;
}
/* The name stored in a file or share item, copied out (so that it is aligned and terminated). */
static BOOL item_name_w(const ITEMIDLIST *id, WCHAR *out, size_t capacity) {
  size_t offset = 0;
  if (item_is_file_item(id)) offset = FILE_ITEM_FIXED; else if (item_is_share(id)) offset = ITEM_HEADER; else return FALSE;
  const BYTE *p = (const BYTE *)id + offset;
  const size_t chars = (id->mkid.cb - offset) / sizeof(WCHAR);
  size_t i = 0;
  for (; i < chars; ++i) {
    WCHAR c;
    CopyMemory(&c, p + i * sizeof(WCHAR), sizeof(c));
    if (!c) break;
    if (i + 1 >= capacity) { out[0] = 0; return FALSE; }
    out[i] = c;
  }
  out[i] = 0;
  return TRUE;
}
static DWORD file_item_attributes(const ITEMIDLIST *id) { return ((const FileItem *)id)->attributes; }
static DWORD file_item_size(const ITEMIDLIST *id) { return ((const FileItem *)id)->size; }
static BOOL file_item_time(const ITEMIDLIST *id, FILETIME *out) {
  const FileItem *f = (const FileItem *)id;
  return f->date && DosDateTimeToFileTime(f->date, f->time, out);
}
/* The name of an item as the shell shows it. */
static BOOL item_display_name(const ITEMIDLIST *id, WCHAR *out, size_t capacity) {
  const GUID *g = item_guid(id);
  if (g) {
    for (size_t i = 0; i < ARRAY_LEN(g_virtual_folders); ++i) if (IsEqualGUID(g_virtual_folders[i].clsid, g)) return w_copy(out, capacity, g_virtual_folders[i].name);
    return w_copy(out, capacity, L"Shell folder");
  }
  if (item_is_drive(id)) {
    WCHAR root[4] = {(WCHAR)((const BYTE *)id)[3], ':', '\\', 0};
    WCHAR label[MAX_PATH];
    if (GetVolumeInformationW(root, label, ARRAY_LEN(label), NULL, NULL, NULL, NULL, 0) && label[0]) {
      WCHAR drive[4] = {root[0], ':', ')', 0};
      return w_copy(out, capacity, label) && w_cat(out, capacity, L" (") && w_cat(out, capacity, drive);
    }
    return w_copy(out, capacity, L"Local Disk (") && w_cat(out, capacity, (WCHAR[]){root[0], ':', ')', 0});
  }
  return item_name_w(id, out, capacity);
}

/* ---- paths <-> lists --------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL desktop_directory(WCHAR *out, size_t capacity) {
  return SUCCEEDED(csidl_path(CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_DONT_VERIFY, SHGFP_TYPE_CURRENT, out, capacity)) && out[0];
}
/* Is `path` below the desktop directory? (Then its list starts at the item under the desktop.) */
static const WCHAR *under_directory(const WCHAR *path, const WCHAR *directory) {
  const size_t n = wlen(directory);
  if (n && wnicmp(path, directory, n) == 0 && (path[n] == 0 || is_sep(path[n]) || is_sep(directory[n - 1]))) {
    const WCHAR *rest = path + n;
    while (is_sep(*rest)) ++rest;
    return rest;
  }
  return NULL;
}

static BOOL only_separators(const WCHAR *p) { while (is_sep(*p)) ++p; return *p == 0; }

/* The list for a path. With `must_exist` a name that is not there fails (ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND); `attributes` gets the file's. */
static HRESULT path_to_pidl(const WCHAR *input, BOOL must_exist, LPITEMIDLIST *out, DWORD *attributes_out) {
  WCHAR path[MAX_PATH_EX];
  *out = NULL;
  if (!input || !*input) return E_INVALIDARG;
  if (!w_copy(path, ARRAY_LEN(path), input)) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  path_unquote(path);
  if (has_wildcards(path)) return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
  WCHAR full[MAX_PATH_EX];
  if (!path_full(path, full, ARRAY_LEN(full))) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  size_t root = path_root_length(full);
  LPITEMIDLIST list = NULL;
  WCHAR walked[MAX_PATH_EX];
  WCHAR desktop[MAX_PATH_EX], common_desktop[MAX_PATH_EX];
  const WCHAR *rest = NULL;
  BOOL via_desktop = FALSE;
  if (desktop_directory(desktop, ARRAY_LEN(desktop)) && (rest = under_directory(full, desktop)) != NULL && *rest) {
    via_desktop = TRUE;
    w_copy_n(walked, ARRAY_LEN(walked), full, (size_t)(rest - full));
    path_strip_trailing_sep(walked);
  } else {
    rest = NULL;
    if (SUCCEEDED(csidl_path(CSIDL_COMMON_DESKTOPDIRECTORY | CSIDL_FLAG_DONT_VERIFY, SHGFP_TYPE_CURRENT, common_desktop, ARRAY_LEN(common_desktop))) && common_desktop[0] &&
        (rest = under_directory(full, common_desktop)) != NULL && *rest) {
      via_desktop = TRUE;
      w_copy_n(walked, ARRAY_LEN(walked), full, (size_t)(rest - full));
      path_strip_trailing_sep(walked);
    }
  }
  DWORD last_attributes = FILE_ATTRIBUTE_DIRECTORY;
  if (!via_desktop) {
    if (has_drive(full)) {
      WCHAR drive_root[4] = {full[0], ':', '\\', 0};
      if (must_exist && GetDriveTypeW(drive_root) <= DRIVE_NO_ROOT_DIR) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
      list = il_guid(&CLSID_MyComputer_);
      if (!list) return E_OUTOFMEMORY;
      LPITEMIDLIST drive = il_drive(full);
      LPITEMIDLIST combined = ILCombine(list, drive);
      SHFree(list); SHFree(drive);
      list = combined;
      w_copy_n(walked, ARRAY_LEN(walked), full, 3);
      rest = full + (root < 3 ? root : 3);
    } else if (is_unc(full)) {
      WCHAR share[MAX_PATH_EX];
      w_copy_n(share, ARRAY_LEN(share), full, root);
      path_strip_trailing_sep(share);
      list = il_share(share);
      if (!list) return E_OUTOFMEMORY;
      w_copy(walked, ARRAY_LEN(walked), share);
      rest = full + root;
    } else {
      return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    }
  } else {
    list = il_new(sizeof(USHORT));
    if (!list) return E_OUTOFMEMORY;
  }
  /* the folders and the file, one item each */
  while (rest && *rest) {
    const WCHAR *end = rest;
    while (*end && !is_sep(*end)) ++end;
    const size_t n = (size_t)(end - rest);
    WCHAR component[MAX_PATH_EX];
    if (!n) { ++rest; continue; }
    if (!w_copy_n(component, ARRAY_LEN(component), rest, n)) { SHFree(list); return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE); }
    if (!path_add_sep(walked, ARRAY_LEN(walked)) || !w_cat(walked, ARRAY_LEN(walked), component)) { SHFree(list); return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE); }
    const BOOL last = only_separators(end);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(walked, &fd);
    LPITEMIDLIST item;
    if (find != INVALID_HANDLE_VALUE) {
      FindClose(find);
      /* (the name in the list is the one on the disk) */
      item = il_from_find(&fd);
      last_attributes = fd.dwFileAttributes;
    } else {
      const DWORD error = GetLastError();
      if (must_exist) { SHFree(list); return HRESULT_FROM_WIN32(last ? ERROR_FILE_NOT_FOUND : (error == ERROR_FILE_NOT_FOUND ? ERROR_PATH_NOT_FOUND : error)); }
      last_attributes = last ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;
      item = il_file(component, last_attributes, 0, NULL);
    }
    if (!item) { SHFree(list); return E_OUTOFMEMORY; }
    LPITEMIDLIST combined = ILCombine(list, item);
    SHFree(list); SHFree(item);
    list = combined;
    if (!list) return E_OUTOFMEMORY;
    rest = end;
    while (is_sep(*rest)) ++rest;
  }
  if (!rest || !via_desktop) {
    if (has_drive(full) && !full[3]) { DWORD a; last_attributes = path_attributes(full, &a) ? a : FILE_ATTRIBUTE_DIRECTORY; }
  }
  if (attributes_out) *attributes_out = last_attributes;
  *out = list;
  return S_OK;
}

/* The path of a list; FALSE for one that is not a file (This PC, the Recycle Bin ...). */
static BOOL pidl_to_path(LPCITEMIDLIST pidl, WCHAR *out, size_t capacity) {
  if (!pidl) return FALSE;
  const ITEMIDLIST *id = pidl;
  out[0] = 0;
  BOOL started = FALSE;
  if (!id->mkid.cb) return desktop_directory(out, capacity);
  const GUID *g = item_guid(id);
  if (g) {
    if (!IsEqualGUID(g, &CLSID_MyComputer_)) return FALSE;
    id = (const ITEMIDLIST *)((const BYTE *)id + id->mkid.cb);
    if (!id->mkid.cb) return FALSE;
    if (!item_is_drive(id) && !item_is_share(id)) return FALSE;
  } else if (item_is_file_item(id)) {
    if (!desktop_directory(out, capacity)) return FALSE;
    started = TRUE;
  }
  for (; id->mkid.cb; id = (const ITEMIDLIST *)((const BYTE *)id + id->mkid.cb)) {
    WCHAR name[MAX_PATH_EX];
    if (item_is_drive(id)) {
      if (started) return FALSE;
      out[0] = (WCHAR)((const BYTE *)id)[3]; out[1] = ':'; out[2] = '\\'; out[3] = 0;
      started = TRUE;
    } else if (item_is_share(id)) {
      if (started || !item_name_w(id, name, ARRAY_LEN(name))) return FALSE;
      if (!w_copy(out, capacity, name)) return FALSE;
      started = TRUE;
    } else if (item_is_file_item(id)) {
      if (!started || !item_name_w(id, name, ARRAY_LEN(name))) return FALSE;
      if (!path_add_sep(out, capacity) || !w_cat(out, capacity, name)) return FALSE;
    } else {
      return FALSE;
    }
  }
  return started;
}

EXPORT LPITEMIDLIST WINAPI ILCreateFromPathW(LPCWSTR path) {
  LPITEMIDLIST pidl = NULL;
  if (FAILED(path_to_pidl(path, TRUE, &pidl, NULL))) return NULL;
  return pidl;
}
EXPORT LPITEMIDLIST WINAPI ILCreateFromPathA(LPCSTR path) {
  WCHAR *w = a_to_w(path);
  LPITEMIDLIST pidl = w ? ILCreateFromPathW(w) : NULL;
  mem_free(w);
  return pidl;
}
EXPORT LPITEMIDLIST WINAPI ILCreateFromPath(LPCWSTR path) { return ILCreateFromPathW(path); }
EXPORT HRESULT WINAPI SHILCreateFromPath(LPCWSTR path, LPITEMIDLIST *pidl, DWORD *attributes) {
  if (!pidl) return E_INVALIDARG;
  DWORD found = 0;
  const HRESULT hr = path_to_pidl(path, TRUE, pidl, &found);
  if (attributes) {
    const DWORD wanted = *attributes;
    *attributes = SUCCEEDED(hr) ? (wanted & SFGAO_FOLDER & (found & FILE_ATTRIBUTE_DIRECTORY ? ~0u : 0u)) : 0;
  }
  return hr;
}
/* A list for a path without looking at the disk. */
EXPORT LPITEMIDLIST WINAPI SHSimpleIDListFromPath(LPCWSTR path) {
  LPITEMIDLIST pidl = NULL;
  if (FAILED(path_to_pidl(path, FALSE, &pidl, NULL))) return NULL;
  return pidl;
}
EXPORT BOOL WINAPI SHGetPathFromIDListW(LPCITEMIDLIST pidl, LPWSTR path) {
  if (!path) return FALSE;
  path[0] = 0;
  WCHAR full[MAX_PATH_EX];
  if (!pidl_to_path(pidl, full, ARRAY_LEN(full)) || wlen(full) >= MAX_PATH) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  w_copy(path, MAX_PATH, full);
  return TRUE;
}
EXPORT BOOL WINAPI SHGetPathFromIDListA(LPCITEMIDLIST pidl, LPSTR path) {
  WCHAR w[MAX_PATH];
  if (!path) return FALSE;
  path[0] = 0;
  return SHGetPathFromIDListW(pidl, w) && w_to_a(w, path, MAX_PATH);
}
EXPORT BOOL WINAPI SHGetPathFromIDList(LPCITEMIDLIST pidl, LPWSTR path) { return SHGetPathFromIDListW(pidl, path); }
EXPORT BOOL WINAPI SHGetPathFromIDListEx(PCIDLIST_ABSOLUTE pidl, WCHAR *path, DWORD capacity, GPFIDL_FLAGS flags) {
  (void)flags;
  WCHAR full[MAX_PATH_EX];
  if (!path || !capacity) return FALSE;
  if (!pidl_to_path(pidl, full, ARRAY_LEN(full))) { path[0] = 0; return FALSE; }
  return w_copy(path, capacity, full);
}

/* ---- special folders as lists ------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT known_folder_pidl(const KnownFolder *kf, DWORD flags, LPITEMIDLIST *out) {
  *out = NULL;
  if (kf->kind & KF_VIRTUAL) {
    if (kf->id == &FOLDERID_Desktop) { *out = il_new(sizeof(USHORT)); return *out ? S_OK : E_OUTOFMEMORY; }
    if (!kf->clsid) return E_FAIL;
    /* "::{A}\::{B}" is a list of two (or more) GUID items */
    LPITEMIDLIST list = NULL;
    for (const WCHAR *s = kf->clsid; *s;) {
      if (s[0] == ':' && s[1] == ':' && s[2] == '{') {
        GUID g;
        WCHAR text[40];
        const WCHAR *close = w_find(s, '}');
        if (!close || (size_t)(close - s - 2) + 1 >= ARRAY_LEN(text)) break;
        w_copy_n(text, ARRAY_LEN(text), s + 2, (size_t)(close - s - 1));
        if (FAILED(CLSIDFromString(text, &g))) break;
        LPITEMIDLIST item = il_guid(&g);
        LPITEMIDLIST combined = ILCombine(list, item);
        SHFree(item); SHFree(list);
        list = combined;
        s = close + 1;
        if (*s == '\\') ++s;
      } else break;
    }
    *out = list;
    return list ? S_OK : E_FAIL;
  }
  WCHAR path[MAX_PATH_EX];
  const HRESULT hr = kf_resolve(kf, flags, path, ARRAY_LEN(path));
  if (FAILED(hr)) return hr;
  return path_to_pidl(path, !(flags & KF_FLAG_DONT_VERIFY), out, NULL);
}
EXPORT HRESULT WINAPI SHGetKnownFolderIDList(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PIDLIST_ABSOLUTE *out) {
  (void)token;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  const KnownFolder *kf = id ? kf_find(id) : NULL;
  if (!kf) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  return known_folder_pidl(kf, flags, out);
}
EXPORT HRESULT WINAPI SHGetFolderLocation(HWND owner, int csidl, HANDLE token, DWORD reserved, PIDLIST_ABSOLUTE *out) {
  (void)owner;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!token_valid(token)) return E_FAIL;
  if (reserved) return E_INVALIDARG;
  const KnownFolder *kf = kf_for_csidl(csidl & 0xFF);
  if (!kf) return E_INVALIDARG;
  DWORD flags = 0;
  if (csidl & CSIDL_FLAG_CREATE) flags |= KF_FLAG_CREATE;
  if (csidl & CSIDL_FLAG_DONT_VERIFY) flags |= KF_FLAG_DONT_VERIFY;
  if ((csidl & 0xFF) == CSIDL_DESKTOP) { *out = il_new(sizeof(USHORT)); return *out ? S_OK : E_OUTOFMEMORY; }
  return known_folder_pidl(kf, flags, out);
}
EXPORT HRESULT WINAPI SHGetFolderLocationWorker(HWND owner, int csidl, HANDLE token, DWORD reserved, PIDLIST_ABSOLUTE *out) { return SHGetFolderLocation(owner, csidl, token, reserved, out); }
EXPORT HRESULT WINAPI SHGetSpecialFolderLocation(HWND owner, int csidl, PIDLIST_ABSOLUTE *out) {
  const HRESULT hr = SHGetFolderLocation(owner, csidl | CSIDL_FLAG_CREATE, NULL, 0, out);
  return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ? hr : hr;
}
EXPORT LPITEMIDLIST WINAPI SHCloneSpecialIDList(HWND owner, int csidl, BOOL create) {
  LPITEMIDLIST pidl = NULL;
  if (FAILED(SHGetFolderLocation(owner, create ? csidl | CSIDL_FLAG_CREATE : csidl, NULL, 0, &pidl))) return NULL;
  return pidl;
}

/* ---- the shell folder objects ------------------------------------------------------------------------------------------------------------------------------ */

typedef enum { FOLDER_DESKTOP, FOLDER_COMPUTER, FOLDER_FILES, FOLDER_VIRTUAL } FolderKind;
typedef struct {
  const IShellFolder2Vtbl *folder;
  const IPersistFolder3Vtbl *persist;
  LONG ref;
  FolderKind kind;
  LPITEMIDLIST pidl;      /* the absolute list of this folder */
  WCHAR path[MAX_PATH_EX];      /* for a file system folder */
} FolderObject;
#define FOLDER_FROM_PERSIST(p) ((FolderObject *)((BYTE *)(p) - offsetof(FolderObject, persist)))

static HRESULT folder_create(FolderKind kind, LPCITEMIDLIST pidl, const WCHAR *path, REFIID riid, void **out);

static void strret_wide(STRRET *s, const WCHAR *text) {
  s->uType = STRRET_WSTR;
  s->pOleStr = w_task_dup(text);
}


/* A STRRET as a string (to be freed with SHFree) or in a buffer; the string may be in the item or an offset into it. */
static HRESULT strret_to_buffer(STRRET *s, LPCITEMIDLIST pidl, WCHAR *out, size_t capacity) {
  HRESULT hr = S_OK;
  switch (s->uType) {
    case STRRET_WSTR: hr = w_copy(out, capacity, s->pOleStr ? s->pOleStr : L"") ? S_OK : E_FAIL; SHFree(s->pOleStr); s->pOleStr = NULL; break;
    case STRRET_CSTR: { WCHAR *w = a_to_w(s->cStr); hr = w && w_copy(out, capacity, w) ? S_OK : E_FAIL; mem_free(w); break; }
    case STRRET_OFFSET: { WCHAR *w = pidl ? a_to_w((const char *)pidl + s->uOffset) : NULL; hr = w && w_copy(out, capacity, w) ? S_OK : E_FAIL; mem_free(w); break; }
    default: out[0] = 0; hr = E_FAIL; break;
  }
  return hr;
}
static HRESULT strret_to_task_string(STRRET *s, LPCITEMIDLIST pidl, WCHAR **out) {
  WCHAR buffer[MAX_PATH_EX];
  if (s->uType == STRRET_WSTR) { *out = s->pOleStr; s->pOleStr = NULL; return *out ? S_OK : E_FAIL; }
  const HRESULT hr = strret_to_buffer(s, pidl, buffer, ARRAY_LEN(buffer));
  if (FAILED(hr)) { *out = NULL; return hr; }
  *out = w_task_dup(buffer);
  return *out ? S_OK : E_OUTOFMEMORY;
}

/* The items of a folder, copied into a list of lists: the enumerator hands them out one by one. */
typedef struct {
  const IEnumIDListVtbl *vtbl;
  LONG ref;
  LPITEMIDLIST *items;
  size_t count, position;
} EnumIdList;

static HRESULT STDMETHODCALLTYPE enum_query(IEnumIDList *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumIDList)) { *out = self; InterlockedIncrement(&((EnumIdList *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE enum_addref(IEnumIDList *self) { return (ULONG)InterlockedIncrement(&((EnumIdList *)self)->ref); }
static ULONG STDMETHODCALLTYPE enum_release(IEnumIDList *self) {
  EnumIdList *e = (EnumIdList *)self;
  const LONG n = InterlockedDecrement(&e->ref);
  if (!n) {
    for (size_t i = 0; i < e->count; ++i) SHFree(e->items[i]);
    mem_free(e->items);
    mem_free(e);
    objects_remove();
  }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE enum_next(IEnumIDList *self, ULONG wanted, PITEMID_CHILD *out, ULONG *fetched) {
  EnumIdList *e = (EnumIdList *)self;
  ULONG got = 0;
  if (!out || (!fetched && wanted != 1)) return E_INVALIDARG;
  while (got < wanted && e->position < e->count) {
    out[got] = ILClone(e->items[e->position]);
    if (!out[got]) break;
    ++got;
    ++e->position;
  }
  if (fetched) *fetched = got;
  return got == wanted ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE enum_skip(IEnumIDList *self, ULONG n) {
  EnumIdList *e = (EnumIdList *)self;
  if (n > e->count - e->position) { e->position = e->count; return S_FALSE; }
  e->position += n;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE enum_reset(IEnumIDList *self) { ((EnumIdList *)self)->position = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE enum_clone(IEnumIDList *self, IEnumIDList **out);
static const IEnumIDListVtbl g_enum_vtbl = {enum_query, enum_addref, enum_release, enum_next, enum_skip, enum_reset, enum_clone};

/* `items` is taken over (the array and every list in it). */
static EnumIdList *enum_new(LPITEMIDLIST *items, size_t count) {
  EnumIdList *e = mem_alloc(sizeof(*e));
  if (!e) { for (size_t i = 0; i < count; ++i) SHFree(items[i]); mem_free(items); return NULL; }
  e->vtbl = &g_enum_vtbl;
  e->ref = 1;
  e->items = items;
  e->count = count;
  objects_add();
  return e;
}
static HRESULT STDMETHODCALLTYPE enum_clone(IEnumIDList *self, IEnumIDList **out) {
  EnumIdList *e = (EnumIdList *)self;
  LPITEMIDLIST *items = mem_alloc((e->count ? e->count : 1) * sizeof(LPITEMIDLIST));
  if (!items) return E_OUTOFMEMORY;
  for (size_t i = 0; i < e->count; ++i) items[i] = ILClone(e->items[i]);
  EnumIdList *c = enum_new(items, e->count);
  if (!c) return E_OUTOFMEMORY;
  c->position = e->position;
  *out = (IEnumIDList *)c;
  return S_OK;
}

typedef struct { LPITEMIDLIST *items; size_t count, capacity; } IdVector;
static BOOL idv_push(IdVector *v, LPITEMIDLIST item) {
  if (!item) return FALSE;
  if (v->count == v->capacity) {
    const size_t n = v->capacity ? v->capacity * 2 : 16;
    LPITEMIDLIST *grown = mem_realloc(v->items, n * sizeof(LPITEMIDLIST));
    if (!grown) { SHFree(item); return FALSE; }
    v->items = grown;
    v->capacity = n;
  }
  v->items[v->count++] = item;
  return TRUE;
}

/* The entries of a directory that the flags let through, as items. */
static void enumerate_directory(const WCHAR *directory, DWORD flags, IdVector *v) {
  WCHAR pattern[MAX_PATH_EX];
  if (!path_join(pattern, ARRAY_LEN(pattern), directory, L"*")) return;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcmp(fd.cFileName, L".") || !wcmp(fd.cFileName, L"..")) continue;
    const BOOL folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (folder ? !(flags & SHCONTF_FOLDERS) : !(flags & SHCONTF_NONFOLDERS)) continue;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && !(flags & SHCONTF_INCLUDEHIDDEN)) continue;
    if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) && !(flags & 0x10000)) continue;      /* SHCONTF_INCLUDESUPERHIDDEN */
    idv_push(v, il_from_find(&fd));
  } while (FindNextFileW(find, &fd));
  FindClose(find);
}

/* ---- IShellFolder ------------------------------------------------------------------------------------------------------------------------------------------ */

#define SELF(p) ((FolderObject *)(p))

static HRESULT STDMETHODCALLTYPE sf_query(IShellFolder2 *self, REFIID riid, void **out) {
  FolderObject *f = SELF(self);
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IShellFolder) || IsEqualIID(riid, &IID_IShellFolder2)) *out = &f->folder;
  else if (IsEqualIID(riid, &IID_IPersist) || IsEqualIID(riid, &IID_IPersistFolder) || IsEqualIID(riid, &IID_IPersistFolder2) || IsEqualIID(riid, &IID_IPersistFolder3)) *out = &f->persist;
  else return E_NOINTERFACE;
  InterlockedIncrement(&f->ref);
  return S_OK;
}
static ULONG STDMETHODCALLTYPE sf_addref(IShellFolder2 *self) { return (ULONG)InterlockedIncrement(&SELF(self)->ref); }
static ULONG STDMETHODCALLTYPE sf_release(IShellFolder2 *self) {
  FolderObject *f = SELF(self);
  const LONG n = InterlockedDecrement(&f->ref);
  if (!n) {
    SHFree(f->pidl);
    mem_free(f);
    objects_remove();
  }
  return (ULONG)n;
}

/* The path of a child item of a file system folder. */
static BOOL child_path(const FolderObject *f, LPCITEMIDLIST item, WCHAR *out, size_t capacity) {
  WCHAR name[MAX_PATH_EX];
  if (f->kind == FOLDER_COMPUTER) {
    if (!item_is_drive(item) && !item_is_share(item)) return FALSE;
    if (item_is_drive(item)) { out[0] = (WCHAR)((const BYTE *)item)[3]; out[1] = ':'; out[2] = '\\'; out[3] = 0; return TRUE; }
    return item_name_w(item, out, capacity);
  }
  if (!item_is_file_item(item) || !item_name_w(item, name, ARRAY_LEN(name))) return FALSE;
  WCHAR base[MAX_PATH_EX];
  if (f->kind == FOLDER_DESKTOP) { if (!desktop_directory(base, ARRAY_LEN(base))) return FALSE; }
  else if (!w_copy(base, ARRAY_LEN(base), f->path)) return FALSE;
  return path_join(out, capacity, base, name);
}

static DWORD item_attribute_bits(FolderObject *f, const ITEMIDLIST *item);
static HRESULT STDMETHODCALLTYPE sf_parse(IShellFolder2 *self, HWND hwnd, IBindCtx *bind, LPWSTR name, ULONG *eaten, PIDLIST_RELATIVE *pidl, ULONG *attributes) {
  FolderObject *f = SELF(self);
  (void)hwnd; (void)bind;
  if (eaten) *eaten = 0;
  if (!pidl || !name) return E_INVALIDARG;
  *pidl = NULL;
  if (!*name) { if (f->kind == FOLDER_DESKTOP) { *pidl = il_new(sizeof(USHORT)); return *pidl ? S_OK : E_OUTOFMEMORY; } return E_INVALIDARG; }
  HRESULT hr = E_FAIL;
  if (f->kind == FOLDER_DESKTOP) {
    if (name[0] == ':' && name[1] == ':' && name[2] == '{') {
      GUID g;
      WCHAR text[48];
      const WCHAR *close = w_find(name, '}');
      if (!close || (size_t)(close - name - 2) + 1 >= ARRAY_LEN(text)) return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
      w_copy_n(text, ARRAY_LEN(text), name + 2, (size_t)(close - name - 1));
      if (FAILED(CLSIDFromString(text, &g))) return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
      *pidl = il_guid(&g);
      hr = *pidl ? S_OK : E_OUTOFMEMORY;
    } else if (!wicmp(name, L"Desktop")) {
      *pidl = il_new(sizeof(USHORT));
      hr = *pidl ? S_OK : E_OUTOFMEMORY;
    } else {
      hr = path_to_pidl(name, TRUE, pidl, NULL);
      if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME) || FAILED(hr)) {
        /* a name relative to the desktop folder */
        WCHAR desktop[MAX_PATH_EX], full[MAX_PATH_EX];
        if (is_relative(name) && desktop_directory(desktop, ARRAY_LEN(desktop)) && path_join(full, ARRAY_LEN(full), desktop, name)) hr = path_to_pidl(full, TRUE, pidl, NULL);
      }
    }
  } else {
    WCHAR base[MAX_PATH_EX], full[MAX_PATH_EX];
    if (f->kind == FOLDER_COMPUTER) {
      hr = path_to_pidl(name, TRUE, pidl, NULL);
      if (SUCCEEDED(hr)) {
        /* only the part below This PC */
        LPITEMIDLIST rest = ILFindChild(f->pidl, *pidl);
        LPITEMIDLIST rel = rest ? ILClone(rest) : NULL;
        SHFree(*pidl);
        *pidl = rel;
        hr = rel ? S_OK : E_FAIL;
      }
    } else {
      if (!w_copy(base, ARRAY_LEN(base), f->path) || !path_join(full, ARRAY_LEN(full), base, name)) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
      LPITEMIDLIST whole = NULL;
      hr = path_to_pidl(full, TRUE, &whole, NULL);
      if (SUCCEEDED(hr)) {
        LPITEMIDLIST rest = ILFindChild(f->pidl, whole);
        if (!rest) {
          /* the folder is below the desktop through another path: build the item list from the names */
          WCHAR component[MAX_PATH_EX];
          LPITEMIDLIST list = il_new(sizeof(USHORT));
          const WCHAR *s = name;
          WCHAR walked[MAX_PATH_EX];
          w_copy(walked, ARRAY_LEN(walked), f->path);
          hr = S_OK;
          while (*s && list) {
            const WCHAR *end = s;
            while (*end && !is_sep(*end)) ++end;
            if (end == s) { ++s; continue; }
            w_copy_n(component, ARRAY_LEN(component), s, (size_t)(end - s));
            path_join(walked, ARRAY_LEN(walked), walked, component);
            WIN32_FIND_DATAW fd;
            HANDLE find = FindFirstFileW(walked, &fd);
            if (find == INVALID_HANDLE_VALUE) { hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); break; }
            FindClose(find);
            LPITEMIDLIST item = il_from_find(&fd), combined = ILCombine(list, item);
            SHFree(item); SHFree(list);
            list = combined;
            s = end;
          }
          *pidl = SUCCEEDED(hr) ? list : (SHFree(list), NULL);
        } else {
          *pidl = ILClone(rest);
          hr = *pidl ? S_OK : E_OUTOFMEMORY;
        }
        SHFree(whole);
      }
    }
  }
  if (FAILED(hr)) { SHFree(*pidl); *pidl = NULL; return hr; }
  if (eaten) *eaten = (ULONG)wlen(name);
  if (attributes && *attributes) {
    /* the facts about the last item of the result, as far as this folder can tell */
    const ITEMIDLIST *last = ILFindLastID(*pidl);
    *attributes &= last && last->mkid.cb ? item_attribute_bits(f, last) : 0;
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE sf_enum(IShellFolder2 *self, HWND hwnd, SHCONTF flags, IEnumIDList **out) {
  FolderObject *f = SELF(self);
  (void)hwnd;
  if (!out) return E_POINTER;
  *out = NULL;
  IdVector v = {0};
  if (f->kind == FOLDER_DESKTOP) {
    if (flags & SHCONTF_FOLDERS) {
      idv_push(&v, il_guid(&CLSID_MyComputer_));
      idv_push(&v, il_guid(&CLSID_RecycleBin_));
    }
    WCHAR desktop[MAX_PATH_EX], common[MAX_PATH_EX];
    if (desktop_directory(desktop, ARRAY_LEN(desktop))) enumerate_directory(desktop, flags, &v);
    if (SUCCEEDED(csidl_path(CSIDL_COMMON_DESKTOPDIRECTORY | CSIDL_FLAG_DONT_VERIFY, SHGFP_TYPE_CURRENT, common, ARRAY_LEN(common))) && common[0]) enumerate_directory(common, flags, &v);
  } else if (f->kind == FOLDER_COMPUTER) {
    if (flags & SHCONTF_FOLDERS) {
      WCHAR drives[4 * 27 + 1];
      const DWORD n = GetLogicalDriveStringsW(ARRAY_LEN(drives) - 1, drives);
      for (DWORD i = 0; i < n && drives[i];) {
        idv_push(&v, il_drive(drives + i));
        i += (DWORD)wlen(drives + i) + 1;
      }
    }
  } else if (f->kind == FOLDER_FILES) {
    enumerate_directory(f->path, flags, &v);
  }
  EnumIdList *e = enum_new(v.items ? v.items : mem_alloc(sizeof(LPITEMIDLIST)), v.count);
  if (!e) return E_OUTOFMEMORY;
  *out = (IEnumIDList *)e;
  return S_OK;
}

static HRESULT child_folder(FolderObject *parent, LPCITEMIDLIST item, REFIID riid, void **out) {
  LPITEMIDLIST first = ILCloneFirst(item);
  LPITEMIDLIST abs = ILCombine(parent->pidl, first);
  SHFree(first);
  if (!abs) return E_OUTOFMEMORY;
  HRESULT hr;
  const GUID *g = item_guid(item);
  if (g && IsEqualGUID(g, &CLSID_MyComputer_)) hr = folder_create(FOLDER_COMPUTER, abs, NULL, riid, out);
  else if (g) hr = folder_create(FOLDER_VIRTUAL, abs, NULL, riid, out);
  else if (item_is_drive(item) || item_is_share(item) || item_is_folder_item(item)) {
    WCHAR path[MAX_PATH_EX];
    if (!child_path(parent, item, path, ARRAY_LEN(path))) hr = E_FAIL;
    else hr = folder_create(FOLDER_FILES, abs, path, riid, out);
  } else hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
  SHFree(abs);
  return hr;
}
static HRESULT STDMETHODCALLTYPE sf_bind(IShellFolder2 *self, PCUIDLIST_RELATIVE pidl, IBindCtx *bind, REFIID riid, void **out) {
  FolderObject *f = SELF(self);
  (void)bind;
  if (!out || !pidl) return E_INVALIDARG;
  *out = NULL;
  if (!pidl->mkid.cb) return E_INVALIDARG;
  const ITEMIDLIST *next = (const ITEMIDLIST *)((const BYTE *)pidl + pidl->mkid.cb);
  if (!next->mkid.cb) return child_folder(f, pidl, riid, out);
  /* several items: bind the first, then the rest in it */
  void *sub = NULL;
  HRESULT hr = child_folder(f, pidl, &IID_IShellFolder, &sub);
  if (FAILED(hr)) return hr;
  hr = IShellFolder_BindToObject((IShellFolder *)sub, next, bind, riid, out);
  IShellFolder_Release((IShellFolder *)sub);
  return hr;
}
static HRESULT STDMETHODCALLTYPE sf_bind_storage(IShellFolder2 *self, PCUIDLIST_RELATIVE pidl, IBindCtx *bind, REFIID riid, void **out) {
  (void)self; (void)pidl; (void)bind; (void)riid;
  if (out) *out = NULL;
  return E_NOTIMPL;
}

/* Orders two items: folders before files, then by name (ignoring case); `column` 1 is the size, 2 the type, 3 the date. */
static int compare_items(const ITEMIDLIST *a, const ITEMIDLIST *b, int column) {
  const BOOL fa = !item_is_file_item(a) || item_is_folder_item(a), fb = !item_is_file_item(b) || item_is_folder_item(b);
  if (column == 1 && item_is_file_item(a) && item_is_file_item(b)) {
    const DWORD sa = file_item_size(a), sb = file_item_size(b);
    if (sa != sb) return sa < sb ? -1 : 1;
  } else if (column == 3 && item_is_file_item(a) && item_is_file_item(b)) {
    FILETIME ta = {0}, tb = {0};
    file_item_time(a, &ta); file_item_time(b, &tb);
    const int c = CompareFileTime(&ta, &tb);
    if (c) return c;
  } else if (fa != fb) return fa ? -1 : 1;
  WCHAR na[MAX_PATH_EX], nb[MAX_PATH_EX];
  const BOOL ga = item_guid(a) != NULL, gb = item_guid(b) != NULL;
  if (ga != gb) return ga ? -1 : 1;
  if (ga && gb) return memcmp(item_guid(a), item_guid(b), sizeof(GUID)) < 0 ? -1 : (memcmp(item_guid(a), item_guid(b), sizeof(GUID)) > 0 ? 1 : 0);
  if (!item_display_name(a, na, ARRAY_LEN(na))) na[0] = 0;
  if (!item_display_name(b, nb, ARRAY_LEN(nb))) nb[0] = 0;
  const int c = wicmp(na, nb);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}
static HRESULT STDMETHODCALLTYPE sf_compare(IShellFolder2 *self, LPARAM lparam, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) {
  (void)self;
  if (!a || !b) return E_INVALIDARG;
  const int column = (int)(lparam & SHCIDS_COLUMNMASK);
  if (!a->mkid.cb || !b->mkid.cb) {
    if (!a->mkid.cb && !b->mkid.cb) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(a->mkid.cb ? 1 : -1));
  }
  int r = compare_items(a, b, column);
  if (!r) {
    const ITEMIDLIST *na = (const ITEMIDLIST *)((const BYTE *)a + a->mkid.cb), *nb = (const ITEMIDLIST *)((const BYTE *)b + b->mkid.cb);
    if (na->mkid.cb || nb->mkid.cb) r = na->mkid.cb ? (nb->mkid.cb ? compare_items(na, nb, column) : 1) : -1;
  }
  return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (USHORT)(short)r);
}
static HRESULT STDMETHODCALLTYPE sf_view(IShellFolder2 *self, HWND hwnd, REFIID riid, void **out) {
  (void)self; (void)hwnd; (void)riid;
  if (out) *out = NULL;
  return E_NOINTERFACE;      /* (there is no window system: no view) */
}

/* The facts about an item as SFGAO bits. */
static DWORD item_attribute_bits(FolderObject *f, const ITEMIDLIST *item) {
  const GUID *g = item_guid(item);
  if (g) {
    DWORD bits = SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_CANLINK | SFGAO_DROPTARGET | SFGAO_HASPROPSHEET | SFGAO_BROWSABLE;
    if (IsEqualGUID(g, &CLSID_MyComputer_)) bits |= SFGAO_CANRENAME | SFGAO_STORAGEANCESTOR | SFGAO_FILESYSANCESTOR;
    if (IsEqualGUID(g, &CLSID_RecycleBin_)) bits |= SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_CANMOVE | SFGAO_CANCOPY;
    return bits;
  }
  if (item_is_drive(item)) return SFGAO_STORAGE | SFGAO_FILESYSTEM | SFGAO_FILESYSANCESTOR | SFGAO_STORAGEANCESTOR | SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_CANLINK | SFGAO_DROPTARGET | SFGAO_HASPROPSHEET | SFGAO_CANCOPY | SFGAO_BROWSABLE;
  if (item_is_share(item)) return SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_CANLINK | SFGAO_DROPTARGET | SFGAO_HASPROPSHEET | SFGAO_CANCOPY;
  DWORD bits = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANLINK | SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_DROPTARGET | SFGAO_HASPROPSHEET;
  const DWORD a = file_item_attributes(item);
  if (item_is_folder_item(item)) {
    bits |= SFGAO_FOLDER | SFGAO_STORAGE | SFGAO_STORAGEANCESTOR | SFGAO_FILESYSANCESTOR | SFGAO_BROWSABLE;
    WCHAR path[MAX_PATH_EX];
    if (child_path(f, item, path, ARRAY_LEN(path))) {
      IdVector v = {0};
      enumerate_directory(path, SHCONTF_FOLDERS | SHCONTF_INCLUDEHIDDEN, &v);
      if (v.count) bits |= SFGAO_HASSUBFOLDER;
      for (size_t i = 0; i < v.count; ++i) SHFree(v.items[i]);
      mem_free(v.items);
    }
  } else {
    bits |= SFGAO_STREAM;
  }
  if (a & FILE_ATTRIBUTE_READONLY) bits |= SFGAO_READONLY;
  if (a & FILE_ATTRIBUTE_HIDDEN) bits |= SFGAO_HIDDEN | SFGAO_GHOSTED;
  if (a & FILE_ATTRIBUTE_SYSTEM) bits |= SFGAO_SYSTEM;
  return bits;
}
static HRESULT STDMETHODCALLTYPE sf_attributes(IShellFolder2 *self, UINT count, PCUITEMID_CHILD_ARRAY items, SFGAOF *attributes) {
  FolderObject *f = SELF(self);
  if (!attributes) return E_INVALIDARG;
  if (!count || !items) {
    /* the folder itself */
    DWORD bits = SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_CANLINK | SFGAO_DROPTARGET | SFGAO_FILESYSANCESTOR | SFGAO_STORAGEANCESTOR;
    if (f->kind == FOLDER_FILES) bits |= SFGAO_FILESYSTEM | SFGAO_STORAGE | SFGAO_CANCOPY;
    *attributes &= bits;
    return S_OK;
  }
  DWORD result = 0xFFFFFFFFu;
  for (UINT i = 0; i < count; ++i) {
    if (!items[i]) return E_INVALIDARG;
    result &= item_attribute_bits(f, items[i]);
  }
  *attributes &= result;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE sf_ui_object(IShellFolder2 *self, HWND hwnd, UINT count, PCUITEMID_CHILD_ARRAY items, REFIID riid, UINT *reserved, void **out);

/* The name of an item for the flags (SHGDN_*). */
static HRESULT item_name_for(FolderObject *f, const ITEMIDLIST *item, SHGDNF flags, WCHAR *out, size_t capacity) {
  const BOOL parsing = (flags & SHGDN_FORPARSING) != 0, in_folder = (flags & SHGDN_INFOLDER) != 0;
  const GUID *g = item_guid(item);
  if (g) {
    if (parsing) {
      WCHAR text[48];
      if (!StringFromGUID2(g, text, ARRAY_LEN(text))) return E_FAIL;
      return w_copy(out, capacity, L"::") && w_cat(out, capacity, text) ? S_OK : E_FAIL;
    }
    return item_display_name(item, out, capacity) ? S_OK : E_FAIL;
  }
  if (item_is_drive(item)) {
    if (parsing) { WCHAR root[4] = {(WCHAR)((const BYTE *)item)[3], ':', '\\', 0}; return w_copy(out, capacity, root) ? S_OK : E_FAIL; }
    return item_display_name(item, out, capacity) ? S_OK : E_FAIL;
  }
  if (parsing && !in_folder) return child_path(f, item, out, capacity) ? S_OK : E_FAIL;
  return item_name_w(item, out, capacity) ? S_OK : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE sf_display_name(IShellFolder2 *self, PCUITEMID_CHILD pidl, SHGDNF flags, STRRET *name) {
  FolderObject *f = SELF(self);
  WCHAR text[MAX_PATH_EX];
  if (!pidl || !name) return E_INVALIDARG;
  if (!pidl->mkid.cb) {
    /* the folder itself */
    if (f->kind == FOLDER_DESKTOP) { strret_wide(name, (flags & SHGDN_FORPARSING) ? L"Desktop" : L"Desktop"); return name->pOleStr ? S_OK : E_OUTOFMEMORY; }
    return E_INVALIDARG;
  }
  const ITEMIDLIST *next = (const ITEMIDLIST *)((const BYTE *)pidl + pidl->mkid.cb);
  if (next->mkid.cb) {
    /* a path of several items: the name of the last in its parent, or the whole path */
    LPITEMIDLIST abs = ILCombine(f->pidl, pidl);
    WCHAR path[MAX_PATH_EX];
    const BOOL ok = abs && pidl_to_path(abs, path, ARRAY_LEN(path));
    SHFree(abs);
    if (!ok) return E_FAIL;
    if ((flags & SHGDN_INFOLDER) && (flags & SHGDN_FORPARSING)) {
      LPITEMIDLIST rest = ILFindLastID(pidl);
      (void)rest;
    }
    if (!(flags & SHGDN_FORPARSING) || (flags & SHGDN_INFOLDER)) strret_wide(name, path_file_name(path)); else strret_wide(name, path);
    return name->pOleStr ? S_OK : E_OUTOFMEMORY;
  }
  const HRESULT hr = item_name_for(f, pidl, flags, text, ARRAY_LEN(text));
  if (FAILED(hr)) return hr;
  strret_wide(name, text);
  return name->pOleStr ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE sf_set_name(IShellFolder2 *self, HWND hwnd, PCUITEMID_CHILD pidl, LPCWSTR name, SHGDNF flags, PITEMID_CHILD *new_pidl) {
  FolderObject *f = SELF(self);
  (void)hwnd; (void)flags;
  if (new_pidl) *new_pidl = NULL;
  if (!pidl || !name || !*name) return E_INVALIDARG;
  WCHAR from[MAX_PATH_EX], to[MAX_PATH_EX], base[MAX_PATH_EX];
  if (!child_path(f, pidl, from, ARRAY_LEN(from))) return E_FAIL;
  if (!w_copy(base, ARRAY_LEN(base), from)) return E_FAIL;
  WCHAR *cut = (WCHAR *)path_file_name(base);
  *cut = 0;
  for (const WCHAR *c = name; *c; ++c) if (is_sep(*c) || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
  if (!w_cat(base, ARRAY_LEN(base), name)) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  w_copy(to, ARRAY_LEN(to), base);
  if (!MoveFileW(from, to)) return hr_last_error();
  if (new_pidl) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(to, &fd);
    if (find != INVALID_HANDLE_VALUE) { FindClose(find); *new_pidl = il_from_find(&fd); }
  }
  return S_OK;
}

/* Details: the columns of a file system folder. */
static HRESULT STDMETHODCALLTYPE sf2_default_search(IShellFolder2 *self, GUID *guid) { (void)self; if (guid) *guid = kGuidNull; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE sf2_enum_searches(IShellFolder2 *self, IEnumExtraSearch **out) { (void)self; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE sf2_default_column(IShellFolder2 *self, DWORD reserved, ULONG *sort, ULONG *display) {
  (void)self; (void)reserved;
  if (sort) *sort = 0;
  if (display) *display = 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sf2_column_state(IShellFolder2 *self, UINT column, SHCOLSTATEF *state) {
  (void)self;
  if (!state) return E_INVALIDARG;
  if (column > 3) return E_INVALIDARG;
  *state = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT;
  if (column == 1) *state = SHCOLSTATE_TYPE_INT | SHCOLSTATE_ONBYDEFAULT;
  if (column == 3) *state = SHCOLSTATE_TYPE_DATE | SHCOLSTATE_ONBYDEFAULT;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sf2_details_ex(IShellFolder2 *self, PCUITEMID_CHILD pidl, const SHCOLUMNID *id, VARIANT *v) { (void)self; (void)pidl; (void)id; (void)v; return E_NOTIMPL; }
static void format_file_size(ULONGLONG size, WCHAR *out) {
  /* "12 KB": rounded up to whole kilobytes like the file list does */
  ULONGLONG kb = (size + 1023) / 1024;
  WCHAR digits[24];
  int n = 0;
  do { digits[n++] = (WCHAR)('0' + kb % 10); kb /= 10; } while (kb);
  int o = 0;
  while (n) { out[o++] = digits[--n]; }
  out[o++] = ' '; out[o++] = 'K'; out[o++] = 'B'; out[o] = 0;
}
static void format_item_date(const FILETIME *ft, WCHAR *out, size_t capacity) {
  SYSTEMTIME st, local;
  FILETIME lft;
  if (!FileTimeToLocalFileTime(ft, &lft) || !FileTimeToSystemTime(&lft, &st)) { out[0] = 0; return; }
  local = st;
  WCHAR date[40], time[40];
  GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, ARRAY_LEN(date));
  GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, time, ARRAY_LEN(time));
  w_copy(out, capacity, date);
  w_cat(out, capacity, L" ");
  w_cat(out, capacity, time);
}
static HRESULT STDMETHODCALLTYPE sf2_details_of(IShellFolder2 *self, PCUITEMID_CHILD pidl, UINT column, SHELLDETAILS *d) {
  FolderObject *f = SELF(self);
  static const WCHAR *const titles[] = {L"Name", L"Size", L"Type", L"Date modified"};
  if (!d) return E_INVALIDARG;
  d->fmt = column == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
  d->cxChar = column == 0 ? 30 : (column == 2 ? 20 : 15);
  if (column > 3) return E_INVALIDARG;
  if (!pidl) { strret_wide(&d->str, titles[column]); return d->str.pOleStr ? S_OK : E_OUTOFMEMORY; }
  WCHAR text[MAX_PATH_EX];
  text[0] = 0;
  if (column == 0) {
    if (FAILED(item_name_for(f, pidl, SHGDN_NORMAL, text, ARRAY_LEN(text)))) text[0] = 0;
  } else if (item_is_file_item(pidl)) {
    if (column == 1 && !item_is_folder_item(pidl)) format_file_size(file_item_size(pidl), text);
    else if (column == 2) {
      if (item_is_folder_item(pidl)) w_copy(text, ARRAY_LEN(text), L"File folder");
      else {
        WCHAR name[MAX_PATH_EX];
        item_name_w(pidl, name, ARRAY_LEN(name));
        const WCHAR *ext = path_extension(name);
        if (*ext) { w_copy(text, ARRAY_LEN(text), ext + 1); for (WCHAR *c = text; *c; ++c) *c = wup(*c); w_cat(text, ARRAY_LEN(text), L" File"); }
        else w_copy(text, ARRAY_LEN(text), L"File");
      }
    } else if (column == 3) {
      FILETIME ft;
      if (file_item_time(pidl, &ft)) format_item_date(&ft, text, ARRAY_LEN(text));
    }
  } else if (column == 2) {
    w_copy(text, ARRAY_LEN(text), item_is_drive(pidl) ? L"Local Disk" : L"System Folder");
  }
  strret_wide(&d->str, text);
  return d->str.pOleStr ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE sf2_map_column(IShellFolder2 *self, UINT column, SHCOLUMNID *id) { (void)self; (void)column; (void)id; return E_NOTIMPL; }

static const IShellFolder2Vtbl g_folder_vtbl = {
  sf_query, sf_addref, sf_release, sf_parse, sf_enum, sf_bind, sf_bind_storage, sf_compare, sf_view, sf_attributes, sf_ui_object, sf_display_name, sf_set_name,
  sf2_default_search, sf2_enum_searches, sf2_default_column, sf2_column_state, sf2_details_ex, sf2_details_of, sf2_map_column,
};

/* IPersistFolder3 on the same object */
static HRESULT STDMETHODCALLTYPE pf_query(IPersistFolder3 *self, REFIID riid, void **out) { return sf_query((IShellFolder2 *)FOLDER_FROM_PERSIST(self), riid, out); }
static ULONG STDMETHODCALLTYPE pf_addref(IPersistFolder3 *self) { return sf_addref((IShellFolder2 *)FOLDER_FROM_PERSIST(self)); }
static ULONG STDMETHODCALLTYPE pf_release(IPersistFolder3 *self) { return sf_release((IShellFolder2 *)FOLDER_FROM_PERSIST(self)); }
static HRESULT STDMETHODCALLTYPE pf_class_id(IPersistFolder3 *self, CLSID *id) {
  FolderObject *f = FOLDER_FROM_PERSIST(self);
  if (!id) return E_INVALIDARG;
  *id = f->kind == FOLDER_DESKTOP ? CLSID_ShellDesktop_ : (f->kind == FOLDER_COMPUTER ? CLSID_MyComputer_ : CLSID_ShellFSFolder_);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE pf_initialize(IPersistFolder3 *self, PCIDLIST_ABSOLUTE pidl) {
  FolderObject *f = FOLDER_FROM_PERSIST(self);
  LPITEMIDLIST copy = pidl ? ILClone(pidl) : NULL;
  if (!copy) return E_INVALIDARG;
  SHFree(f->pidl);
  f->pidl = copy;
  if (f->kind == FOLDER_FILES) {
    if (!pidl_to_path(copy, f->path, ARRAY_LEN(f->path))) return E_FAIL;
  }
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE pf_cur_folder(IPersistFolder3 *self, PIDLIST_ABSOLUTE *pidl) {
  FolderObject *f = FOLDER_FROM_PERSIST(self);
  if (!pidl) return E_POINTER;
  *pidl = ILClone(f->pidl);
  return *pidl ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE pf_initialize_ex(IPersistFolder3 *self, IBindCtx *bind, PCIDLIST_ABSOLUTE pidl, const PERSIST_FOLDER_TARGET_INFO *info) {
  (void)bind; (void)info;
  return pf_initialize(self, pidl);
}
static HRESULT STDMETHODCALLTYPE pf_target_info(IPersistFolder3 *self, PERSIST_FOLDER_TARGET_INFO *info) {
  FolderObject *f = FOLDER_FROM_PERSIST(self);
  if (!info) return E_POINTER;
  ZeroMemory(info, sizeof(*info));
  if (f->kind == FOLDER_FILES) w_copy(info->szTargetParsingName, ARRAY_LEN(info->szTargetParsingName), f->path);
  info->dwAttributes = FILE_ATTRIBUTE_DIRECTORY;
  info->csidl = -1;
  return S_OK;
}
static const IPersistFolder3Vtbl g_persist_vtbl = {pf_query, pf_addref, pf_release, pf_class_id, pf_initialize, pf_cur_folder, pf_initialize_ex, pf_target_info};

static HRESULT folder_create(FolderKind kind, LPCITEMIDLIST pidl, const WCHAR *path, REFIID riid, void **out) {
  FolderObject *f = mem_alloc(sizeof(*f));
  if (!f) return E_OUTOFMEMORY;
  f->folder = &g_folder_vtbl;
  f->persist = &g_persist_vtbl;
  f->ref = 1;
  f->kind = kind;
  f->pidl = pidl ? ILClone(pidl) : il_new(sizeof(USHORT));
  if (path) w_copy(f->path, ARRAY_LEN(f->path), path);
  objects_add();
  const HRESULT hr = sf_query((IShellFolder2 *)f, riid, out);
  sf_release((IShellFolder2 *)f);
  return hr;
}

EXPORT HRESULT WINAPI SHGetDesktopFolder(IShellFolder **out) {
  if (!out) return E_INVALIDARG;
  return folder_create(FOLDER_DESKTOP, NULL, NULL, &IID_IShellFolder, (void **)out);
}
EXPORT HRESULT WINAPI SHGetDesktopFolderWorker(IShellFolder **out) { return SHGetDesktopFolder(out); }

/* The folder of a list and its last item. */
EXPORT HRESULT WINAPI SHBindToFolderIDListParent(IShellFolder *root, PCUIDLIST_RELATIVE pidl, REFIID riid, void **out, PCUITEMID_CHILD *last) {
  IShellFolder *base = root;
  HRESULT hr;
  if (!pidl || !out) return E_INVALIDARG;
  *out = NULL;
  if (!pidl->mkid.cb) return E_INVALIDARG;
  if (!base) { hr = SHGetDesktopFolder(&base); if (FAILED(hr)) return hr; } else IShellFolder_AddRef(base);
  const ITEMIDLIST *final_item = ILFindLastID(pidl);
  if (final_item == pidl) {
    hr = IShellFolder_QueryInterface(base, riid, out);
  } else {
    const UINT parent_bytes = (UINT)((const BYTE *)final_item - (const BYTE *)pidl);
    LPITEMIDLIST parent = il_new(parent_bytes + sizeof(USHORT));
    if (!parent) { IShellFolder_Release(base); return E_OUTOFMEMORY; }
    CopyMemory(parent, pidl, parent_bytes);
    hr = IShellFolder_BindToObject(base, parent, NULL, riid, out);
    SHFree(parent);
  }
  IShellFolder_Release(base);
  if (SUCCEEDED(hr) && last) *last = final_item;
  return hr;
}
EXPORT HRESULT WINAPI SHBindToFolderIDListParentEx(IShellFolder *root, PCUIDLIST_RELATIVE pidl, IBindCtx *bind, REFIID riid, void **out, PCUITEMID_CHILD *last) {
  (void)bind;
  return SHBindToFolderIDListParent(root, pidl, riid, out, last);
}
EXPORT HRESULT WINAPI SHBindToParent(PCIDLIST_ABSOLUTE pidl, REFIID riid, void **out, PCUITEMID_CHILD *last) { return SHBindToFolderIDListParent(NULL, pidl, riid, out, last); }
EXPORT HRESULT WINAPI SHBindToObject(IShellFolder *folder, PCUIDLIST_RELATIVE pidl, IBindCtx *bind, REFIID riid, void **out) {
  IShellFolder *base = folder;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!base) { const HRESULT hr = SHGetDesktopFolder(&base); if (FAILED(hr)) return hr; } else IShellFolder_AddRef(base);
  HRESULT hr;
  if (!pidl || !pidl->mkid.cb) hr = IShellFolder_QueryInterface(base, riid, out);
  else hr = IShellFolder_BindToObject(base, pidl, bind, riid, out);
  IShellFolder_Release(base);
  return hr;
}

EXPORT HRESULT WINAPI SHGetNameFromIDList(PCIDLIST_ABSOLUTE pidl, SIGDN kind, PWSTR *name) {
  if (!name) return E_INVALIDARG;
  *name = NULL;
  if (!pidl) return E_INVALIDARG;
  IShellFolder *parent = NULL;
  PCUITEMID_CHILD last = NULL;
  if (!pidl->mkid.cb) { *name = w_task_dup(kind == SIGDN_DESKTOPABSOLUTEPARSING || kind == SIGDN_NORMALDISPLAY ? L"Desktop" : L"Desktop"); return *name ? S_OK : E_OUTOFMEMORY; }
  HRESULT hr = SHBindToParent(pidl, &IID_IShellFolder, (void **)&parent, &last);
  if (FAILED(hr)) return hr;
  SHGDNF flags = SHGDN_NORMAL;
  switch (kind) {
    case SIGDN_NORMALDISPLAY: flags = SHGDN_NORMAL; break;
    case SIGDN_PARENTRELATIVEPARSING: flags = SHGDN_INFOLDER | SHGDN_FORPARSING; break;
    case SIGDN_DESKTOPABSOLUTEPARSING: case SIGDN_FILESYSPATH: flags = SHGDN_FORPARSING; break;
    case SIGDN_PARENTRELATIVEEDITING: flags = SHGDN_INFOLDER | SHGDN_FOREDITING; break;
    case SIGDN_DESKTOPABSOLUTEEDITING: flags = SHGDN_FOREDITING | SHGDN_FORPARSING; break;
    case SIGDN_PARENTRELATIVEFORADDRESSBAR: flags = SHGDN_INFOLDER | SHGDN_FORADDRESSBAR; break;
    case SIGDN_PARENTRELATIVE: flags = SHGDN_INFOLDER; break;
    default: IShellFolder_Release(parent); return E_INVALIDARG;
  }
  if (kind == SIGDN_FILESYSPATH) {
    WCHAR path[MAX_PATH_EX];
    IShellFolder_Release(parent);
    if (!pidl_to_path(pidl, path, ARRAY_LEN(path))) return E_INVALIDARG;
    *name = w_task_dup(path);
    return *name ? S_OK : E_OUTOFMEMORY;
  }
  STRRET s;
  hr = IShellFolder_GetDisplayNameOf(parent, last, flags, &s);
  IShellFolder_Release(parent);
  if (FAILED(hr)) return hr;
  return strret_to_task_string(&s, last, name);
}
EXPORT HRESULT WINAPI SHParseDisplayName(LPCWSTR name, IBindCtx *bind, PIDLIST_ABSOLUTE *pidl, SFGAOF want, SFGAOF *have) {
  IShellFolder *desktop = NULL;
  if (pidl) *pidl = NULL;
  if (have) *have = 0;
  if (!name || !pidl) return E_INVALIDARG;
  HRESULT hr = SHGetDesktopFolder(&desktop);
  if (FAILED(hr)) return hr;
  WCHAR *copy = w_dup(name);
  ULONG attributes = want;
  hr = IShellFolder_ParseDisplayName(desktop, NULL, bind, copy, NULL, pidl, want ? &attributes : NULL);
  mem_free(copy);
  if (SUCCEEDED(hr) && have) {
    ULONG bits = want;
    IShellFolder *parent = NULL;
    PCUITEMID_CHILD last = NULL;
    if (want && (*pidl)->mkid.cb && SUCCEEDED(SHBindToParent(*pidl, &IID_IShellFolder, (void **)&parent, &last))) {
      if (SUCCEEDED(IShellFolder_GetAttributesOf(parent, 1, &last, &bits))) *have = bits; else *have = 0;
      IShellFolder_Release(parent);
    }
  }
  IShellFolder_Release(desktop);
  return hr;
}
EXPORT HRESULT WINAPI SHGetDataFromIDListW(IShellFolder *folder, PCUITEMID_CHILD pidl, int format, void *dest, int length) {
  if (!pidl || !dest) return E_INVALIDARG;
  if (format != SHGDFIL_FINDDATA) return E_INVALIDARG;
  if (length < (int)sizeof(WIN32_FIND_DATAW)) return E_INVALIDARG;
  if (!item_is_file_item(pidl)) return E_INVALIDARG;
  (void)folder;
  WIN32_FIND_DATAW *fd = dest;
  ZeroMemory(fd, sizeof(*fd));
  fd->dwFileAttributes = file_item_attributes(pidl);
  fd->nFileSizeLow = file_item_size(pidl);
  file_item_time(pidl, &fd->ftLastWriteTime);
  fd->ftCreationTime = fd->ftLastAccessTime = fd->ftLastWriteTime;
  item_name_w(pidl, fd->cFileName, ARRAY_LEN(fd->cFileName));
  return S_OK;
}
EXPORT HRESULT WINAPI SHGetDataFromIDListA(IShellFolder *folder, PCUITEMID_CHILD pidl, int format, void *dest, int length) {
  if (format != SHGDFIL_FINDDATA) return E_INVALIDARG;
  if (!dest || length < (int)sizeof(WIN32_FIND_DATAA)) return E_INVALIDARG;
  WIN32_FIND_DATAW w;
  const HRESULT hr = SHGetDataFromIDListW(folder, pidl, format, &w, sizeof(w));
  if (FAILED(hr)) return hr;
  WIN32_FIND_DATAA *a = dest;
  ZeroMemory(a, sizeof(*a));
  a->dwFileAttributes = w.dwFileAttributes;
  a->nFileSizeLow = w.nFileSizeLow;
  a->ftLastWriteTime = a->ftCreationTime = a->ftLastAccessTime = w.ftLastWriteTime;
  w_to_a(w.cFileName, a->cFileName, sizeof(a->cFileName));
  return S_OK;
}
/* The real list of a "simple" one: the one the file system makes for the same name. */
EXPORT HRESULT WINAPI SHGetRealIDL(IShellFolder *folder, PCUITEMID_CHILD simple, PITEMID_CHILD *real) {
  if (!real || !simple) return E_INVALIDARG;
  STRRET s;
  WCHAR path[MAX_PATH_EX];
  *real = NULL;
  if (folder && SUCCEEDED(IShellFolder_GetDisplayNameOf(folder, simple, SHGDN_FORPARSING, &s)) && SUCCEEDED(strret_to_buffer(&s, simple, path, ARRAY_LEN(path)))) {
    LPITEMIDLIST whole = NULL;
    if (SUCCEEDED(path_to_pidl(path, TRUE, &whole, NULL))) { *real = ILClone(ILFindLastID(whole)); SHFree(whole); }
  }
  if (!*real) *real = ILClone(simple);
  return *real ? S_OK : E_OUTOFMEMORY;
}
EXPORT HRESULT WINAPI SHCoCreateInstance(LPCWSTR path, const CLSID *clsid, IUnknown *outer, REFIID riid, void **out) {
  (void)path;
  return CoCreateInstance(clsid, outer, CLSCTX_INPROC_SERVER, riid, out);
}
EXPORT HRESULT WINAPI SHCoCreateInstanceWorker(LPCWSTR path, const CLSID *clsid, IUnknown *outer, REFIID riid, void **out) { return SHCoCreateInstance(path, clsid, outer, riid, out); }
EXPORT HRESULT WINAPI SHCLSIDFromString(LPCWSTR text, CLSID *clsid) { return CLSIDFromString(text, clsid); }
