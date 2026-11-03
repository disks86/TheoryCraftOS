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

/* Facts about files (SHGetFileInfo), icons, dropped files, change notifications, notification icons, dialogs that cannot be shown without a window system, and the rest of
 * the exports. Included by shell32.c. */

/* ---- change notifications ---------------------------------------------------------------------------------------------------------------------------------- */

/* Listeners are kept in the process: each has a window and a message to post, the events it wants and (optionally) a folder to watch. A change made through this DLL's
 * functions (file operations, directories) is told to them; a change made elsewhere is told by whoever made it, with SHChangeNotify. */
typedef struct {
  ULONG id;
  HWND window;
  UINT message;
  LONG events;
  int sources;
  LPITEMIDLIST watch;      /* NULL: everything */
  BOOL recursive;
  DWORD owner_thread;
} Listener;
static Listener g_listeners[16];
static ULONG g_next_listener = 1;

EXPORT ULONG WINAPI SHChangeNotifyRegister(HWND window, int sources, LONG events, UINT message, int count, const SHChangeNotifyEntry *entries) {
  ULONG id = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < (int)ARRAY_LEN(g_listeners); ++i) {
    if (g_listeners[i].id) continue;
    g_listeners[i].id = id = g_next_listener++;
    g_listeners[i].window = window;
    g_listeners[i].message = message;
    g_listeners[i].events = events;
    g_listeners[i].sources = sources;
    g_listeners[i].watch = count > 0 && entries && entries[0].pidl ? ILClone(entries[0].pidl) : NULL;
    g_listeners[i].recursive = count > 0 && entries && entries[0].fRecursive;
    g_listeners[i].owner_thread = GetCurrentThreadId();
    break;
  }
  LeaveCriticalSection(&g_lock);
  if (!id) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  return id;
}
EXPORT BOOL WINAPI SHChangeNotifyDeregister(ULONG id) {
  BOOL found = FALSE;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < (int)ARRAY_LEN(g_listeners); ++i) {
    if (g_listeners[i].id != id || !id) continue;
    SHFree(g_listeners[i].watch);
    ZeroMemory(&g_listeners[i], sizeof(g_listeners[i]));
    found = TRUE;
  }
  LeaveCriticalSection(&g_lock);
  return found;
}
EXPORT void WINAPI SHChangeNotifyRegisterThread(SCNRT_STATUS status) { (void)status; }
EXPORT BOOL WINAPI SHChangeNotifySuspendResume(BOOL suspend, PIDLIST_ABSOLUTE folder, BOOL recursive, DWORD reserved) { (void)suspend; (void)folder; (void)recursive; (void)reserved; return TRUE; }

/* The events of one notification are kept in a shared block for the receiver (SHChangeNotification_Lock): the event, then two lists. */
typedef struct { LONG event; DWORD first_offset, second_offset; } NotifyBlock;
EXPORT HANDLE WINAPI SHChangeNotification_Lock(HANDLE block, DWORD process_id, PIDLIST_ABSOLUTE **pidls, LONG *event) {
  NotifyBlock *b = SHLockShared(block, process_id);
  static PIDLIST_ABSOLUTE slots[2];
  if (!b) return NULL;
  slots[0] = b->first_offset ? (PIDLIST_ABSOLUTE)((BYTE *)b + b->first_offset) : NULL;
  slots[1] = b->second_offset ? (PIDLIST_ABSOLUTE)((BYTE *)b + b->second_offset) : NULL;
  if (pidls) *pidls = (PIDLIST_ABSOLUTE *)slots;
  if (event) *event = b->event;
  return (HANDLE)b;
}
EXPORT BOOL WINAPI SHChangeNotification_Unlock(HANDLE lock) { return SHUnlockShared(lock); }

static void notify_listeners(LONG event, LPCITEMIDLIST first, LPCITEMIDLIST second) {
  Listener copy[ARRAY_LEN(g_listeners)];
  EnterCriticalSection(&g_lock);
  CopyMemory(copy, g_listeners, sizeof(copy));
  LeaveCriticalSection(&g_lock);
  for (size_t i = 0; i < ARRAY_LEN(copy); ++i) {
    const Listener *l = &copy[i];
    if (!l->id || !(l->events & event) || !l->window) continue;
    if (l->watch && first && !ILIsEqual(l->watch, first) && !ILIsParent(l->watch, first, !l->recursive)) continue;
    const UINT n1 = first ? ILGetSize(first) : 0, n2 = second ? ILGetSize(second) : 0;
    HANDLE block = SHAllocShared(NULL, (DWORD)(sizeof(NotifyBlock) + n1 + n2), GetCurrentProcessId());
    if (!block) continue;
    NotifyBlock *b = SHLockShared(block, GetCurrentProcessId());
    if (b) {
      b->event = event;
      if (n1) { b->first_offset = sizeof(NotifyBlock); CopyMemory((BYTE *)b + b->first_offset, first, n1); }
      if (n2) { b->second_offset = (DWORD)(sizeof(NotifyBlock) + n1); CopyMemory((BYTE *)b + b->second_offset, second, n2); }
      SHUnlockShared(b);
    }
    if (!PostMessageW(l->window, l->message, (WPARAM)block, (LPARAM)GetCurrentProcessId())) SHFreeShared(block, GetCurrentProcessId());
  }
}
static void change_notify(LONG event, const WCHAR *first, const WCHAR *second) {
  BOOL any = FALSE;
  for (size_t i = 0; i < ARRAY_LEN(g_listeners); ++i) if (g_listeners[i].id) any = TRUE;
  if (!any) return;
  LPITEMIDLIST a = first ? SHSimpleIDListFromPath(first) : NULL, b = second ? SHSimpleIDListFromPath(second) : NULL;
  notify_listeners(event, a, b);
  SHFree(a); SHFree(b);
}
EXPORT void WINAPI SHChangeNotify(LONG event, UINT flags, LPCVOID item1, LPCVOID item2) {
  LPITEMIDLIST a = NULL, b = NULL;
  BOOL own_a = FALSE, own_b = FALSE;
  switch (flags & SHCNF_TYPE) {
    case SHCNF_IDLIST: a = (LPITEMIDLIST)item1; b = (LPITEMIDLIST)item2; break;
    case SHCNF_PATHW: a = item1 ? SHSimpleIDListFromPath(item1) : NULL; b = item2 ? SHSimpleIDListFromPath(item2) : NULL; own_a = own_b = TRUE; break;
    case SHCNF_PATHA: {
      WCHAR *p = a_to_w(item1), *q = a_to_w(item2);
      a = p ? SHSimpleIDListFromPath(p) : NULL; b = q ? SHSimpleIDListFromPath(q) : NULL;
      mem_free(p); mem_free(q);
      own_a = own_b = TRUE;
      break;
    }
    default: break;
  }
  notify_listeners(event, a, b);
  if (own_a) SHFree(a);
  if (own_b) SHFree(b);
}
EXPORT void WINAPI SHFlushSFCache(void) {}

/* ---- dropped files (HDROP) ------------------------------------------------------------------------------------------------------------------------------------ */

/* The Nth name of the DROPFILES block: the pointer to its characters and whether they are wide. */
static const void *drop_name(const DROPFILES *drop, UINT index, UINT *count_out) {
  const BYTE *p = (const BYTE *)drop + drop->pFiles;
  UINT n = 0;
  if (drop->fWide) {
    const WCHAR *w = (const WCHAR *)p;
    while (*w) { if (n == index) { if (count_out) *count_out = 0; return w; } w += wlen(w) + 1; ++n; }
  } else {
    const char *a = (const char *)p;
    while (*a) { if (n == index) { if (count_out) *count_out = 0; return a; } while (*a) ++a; ++a; ++n; }
  }
  if (count_out) *count_out = n;
  return NULL;
}
EXPORT UINT WINAPI DragQueryFileW(HDROP drop_handle, UINT index, LPWSTR buffer, UINT capacity) {
  DROPFILES *drop = GlobalLock(drop_handle);
  if (!drop) return 0;
  UINT result = 0;
  if (index == 0xFFFFFFFFu) { drop_name(drop, 0xFFFFFFFEu, &result); GlobalUnlock(drop_handle); return result; }
  const void *name = drop_name(drop, index, NULL);
  if (name) {
    if (drop->fWide) {
      const WCHAR *w = name;
      const UINT n = (UINT)wlen(w);
      if (buffer && capacity) { result = n < capacity ? n : capacity - 1; CopyMemory(buffer, w, result * sizeof(WCHAR)); buffer[result] = 0; } else result = n;
    } else {
      const char *a = name;
      const int n = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
      if (buffer && capacity) {
        WCHAR *tmp = mem_alloc(((size_t)n + 1) * sizeof(WCHAR));
        if (tmp) { MultiByteToWideChar(CP_ACP, 0, a, -1, tmp, n); const UINT len = (UINT)wlen(tmp); result = len < capacity ? len : capacity - 1; CopyMemory(buffer, tmp, result * sizeof(WCHAR)); buffer[result] = 0; mem_free(tmp); }
      } else result = (UINT)(n > 0 ? n - 1 : 0);
    }
  }
  GlobalUnlock(drop_handle);
  return result;
}
EXPORT UINT WINAPI DragQueryFileA(HDROP drop_handle, UINT index, LPSTR buffer, UINT capacity) {
  if (index == 0xFFFFFFFFu) return DragQueryFileW(drop_handle, index, NULL, 0);
  const UINT wide_len = DragQueryFileW(drop_handle, index, NULL, 0);
  WCHAR *w = mem_alloc(((size_t)wide_len + 1) * sizeof(WCHAR));
  if (!w) return 0;
  DragQueryFileW(drop_handle, index, w, wide_len + 1);
  UINT result = 0;
  const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
  if (buffer && capacity) {
    char *tmp = mem_alloc((size_t)n + 1);
    if (tmp) { WideCharToMultiByte(CP_ACP, 0, w, -1, tmp, n, NULL, NULL); size_t len = 0; while (tmp[len]) ++len; result = (UINT)(len < capacity ? len : capacity - 1); CopyMemory(buffer, tmp, result); buffer[result] = 0; mem_free(tmp); }
  } else result = (UINT)(n > 0 ? n - 1 : 0);
  mem_free(w);
  return result;
}
EXPORT UINT WINAPI DragQueryFile(HDROP drop, UINT index, LPWSTR buffer, UINT capacity) { return DragQueryFileW(drop, index, buffer, capacity); }
EXPORT UINT WINAPI DragQueryFileAorW(HDROP drop, UINT index, void *buffer, UINT capacity, BOOL wide, BOOL unused) {
  (void)unused;
  return wide ? DragQueryFileW(drop, index, buffer, capacity) : DragQueryFileA(drop, index, buffer, capacity);
}
EXPORT BOOL WINAPI DragQueryPoint(HDROP drop_handle, POINT *point) {
  DROPFILES *drop = GlobalLock(drop_handle);
  if (!drop || !point) { if (drop) GlobalUnlock(drop_handle); return FALSE; }
  *point = drop->pt;
  const BOOL client = !drop->fNC;
  GlobalUnlock(drop_handle);
  return client;
}
EXPORT void WINAPI DragFinish(HDROP drop) { if (drop) GlobalFree(drop); }
EXPORT void WINAPI DragAcceptFiles(HWND window, BOOL accept) {
  if (!window) return;
  LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if (accept) style |= WS_EX_ACCEPTFILES; else style &= ~(LONG_PTR)WS_EX_ACCEPTFILES;
  SetWindowLongPtrW(window, GWL_EXSTYLE, style);
}
/* The drag image functions (the image list drags of the shell's list views) need a window system: they report that nothing was done. */
EXPORT BOOL WINAPI DAD_SetDragImage(HIMAGELIST list, POINT *offset) { (void)list; (void)offset; return FALSE; }
EXPORT BOOL WINAPI DAD_DragEnterEx(HWND window, POINT at) { (void)window; (void)at; return FALSE; }
EXPORT BOOL WINAPI DAD_DragEnterEx2(HWND window, POINT at, IDataObject *data) { (void)window; (void)at; (void)data; return FALSE; }
EXPORT BOOL WINAPI DAD_DragLeave(void) { return FALSE; }
EXPORT BOOL WINAPI DAD_DragMove(POINT at) { (void)at; return FALSE; }
EXPORT BOOL WINAPI DAD_ShowDragImage(BOOL show) { (void)show; return FALSE; }
EXPORT BOOL WINAPI DAD_AutoScroll(HWND window, AUTO_SCROLL_DATA *state, const POINT *at) { (void)window; (void)state; (void)at; return FALSE; }
EXPORT HRESULT WINAPI SHDoDragDrop(HWND window, IDataObject *data, IDropSource *source, DWORD effects, DWORD *effect) { (void)window; (void)data; (void)source; (void)effects; if (effect) *effect = 0; return E_NOTIMPL; }

/* ---- facts about a file (SHGetFileInfo) ------------------------------------------------------------------------------------------------------------------------ */

/* The kind of program a file is: 'PE' with the subsystem version for a GUI program, 'PE' and 0 for a console one, 'NE', 'MZ'; 0 for anything else. */
static DWORD executable_type(const WCHAR *path) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return 0;
  BYTE header[64];
  DWORD got = 0, result = 0;
  if (ReadFile(f, header, sizeof(header), &got, NULL) && got == sizeof(header) && header[0] == 'M' && header[1] == 'Z') {
    const DWORD pe_offset = *(DWORD *)(header + 0x3C);
    BYTE pe[0x60];
    result = MAKELONG('M' | ('Z' << 8), 0);
    if (pe_offset < 0x1000000 && SetFilePointer(f, (LONG)pe_offset, NULL, FILE_BEGIN) == pe_offset && ReadFile(f, pe, sizeof(pe), &got, NULL) && got == sizeof(pe)) {
      if (pe[0] == 'P' && pe[1] == 'E' && !pe[2] && !pe[3]) {
        /* optional header at +24; subsystem at +24+68; subsystem version at +24+48 */
        const WORD subsystem = *(WORD *)(pe + 24 + 68);
        const WORD major = *(WORD *)(pe + 24 + 48), minor = *(WORD *)(pe + 24 + 50);
        result = MAKELONG('P' | ('E' << 8), subsystem == 3 ? 0 : (WORD)((major << 8) | minor));
      } else if (pe[0] == 'N' && pe[1] == 'E') {
        result = MAKELONG('N' | ('E' << 8), 0x0300);
      }
    }
  }
  CloseHandle(f);
  return result;
}

/* "TXT File", the class's description, "File folder", "Local Disk" ... */
static void type_name(const WCHAR *path, DWORD attributes, BOOL is_drive, WCHAR *out, size_t capacity) {
  if (is_drive) {
    WCHAR root[4] = {path[0], ':', '\\', 0};
    const UINT t = GetDriveTypeW(root);
    w_copy(out, capacity, t == DRIVE_REMOVABLE ? L"Removable Disk" : (t == DRIVE_CDROM ? L"CD Drive" : (t == DRIVE_REMOTE ? L"Network Drive" : L"Local Disk")));
    return;
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) { w_copy(out, capacity, L"File folder"); return; }
  const WCHAR *ext = path_extension(path);
  if (!*ext || !ext[1]) { w_copy(out, capacity, L"File"); return; }
  WCHAR class_name[MAX_PATH_EX], description[MAX_PATH_EX];
  if (file_class(path, class_name, ARRAY_LEN(class_name)) && classes_string(class_name, NULL, description, ARRAY_LEN(description)) && wicmp(class_name, ext)) { w_copy(out, capacity, description); return; }
  if (is_program_extension(ext) && !wicmp(ext, L".exe")) { w_copy(out, capacity, L"Application"); return; }
  w_copy(out, capacity, ext + 1);
  for (WCHAR *c = out; *c; ++c) *c = wup(*c);
  w_cat(out, capacity, L" File");
}
/* The index of an icon in the system image list the way a classic shell numbered it (there are no icons to show here). */
static int system_icon_index(const WCHAR *path, DWORD attributes, BOOL is_drive) {
  if (is_drive) return 8;
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) return 3;
  if (is_program_extension(path_extension(path))) return 2;
  return 0;
}

/* The common core of SHGetFileInfoW. `pidl` is the absolute list when SHGFI_PIDL is set. */
static DWORD_PTR get_file_info(const WCHAR *path_or_pidl, DWORD file_attributes, SHFILEINFOW *info, UINT size, UINT flags) {
  DWORD_PTR result = 1;
  WCHAR path[MAX_PATH_EX];
  DWORD attributes = file_attributes;
  BOOL have_path = FALSE, is_drive = FALSE, virtual_item = FALSE;
  LPITEMIDLIST pidl = NULL;
  if (!info || size < sizeof(SHFILEINFOW)) return 0;
  path[0] = 0;
  if (flags & SHGFI_PIDL) {
    pidl = (LPITEMIDLIST)path_or_pidl;
    if (pidl) {
      if (!pidl->mkid.cb) virtual_item = TRUE;
      else if (pidl_to_path(pidl, path, ARRAY_LEN(path))) have_path = TRUE;
      else virtual_item = TRUE;
    }
  } else if (path_or_pidl) {
    if (!w_copy(path, ARRAY_LEN(path), path_or_pidl)) return 0;
    have_path = TRUE;
  } else return 0;
  if (have_path && !(flags & SHGFI_USEFILEATTRIBUTES)) {
    DWORD a;
    if (!path_attributes(path, &a)) {
      /* a drive root that is not there, or a name that is not */
      WCHAR full[MAX_PATH_EX];
      if (!path_full(path, full, ARRAY_LEN(full)) || !path_attributes(full, &a)) return 0;
    }
    attributes = a;
  }
  if (have_path && has_drive(path) && (path[2] == 0 || (is_sep(path[2]) && path[3] == 0))) is_drive = TRUE;
  if (flags & SHGFI_ATTRIBUTES) {
    /* the shell attributes; with SHGFI_ATTR_SPECIFIED only those asked for in dwAttributes */
    DWORD wanted = (flags & SHGFI_ATTR_SPECIFIED) ? info->dwAttributes : 0xFFFFFFFFu;
    DWORD bits = 0;
    if (virtual_item && !have_path) bits = SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_CANLINK;
    else {
      bits = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANLINK | SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_DROPTARGET | SFGAO_HASPROPSHEET;
      if (attributes & FILE_ATTRIBUTE_DIRECTORY) bits |= SFGAO_FOLDER | SFGAO_STORAGE | SFGAO_BROWSABLE | SFGAO_FILESYSANCESTOR | SFGAO_STORAGEANCESTOR;
      else bits |= SFGAO_STREAM;
      if (attributes & FILE_ATTRIBUTE_READONLY) bits |= SFGAO_READONLY;
      if (attributes & FILE_ATTRIBUTE_HIDDEN) bits |= SFGAO_HIDDEN | SFGAO_GHOSTED;
      if (attributes & FILE_ATTRIBUTE_SYSTEM) bits |= SFGAO_SYSTEM;
      if (is_drive) bits &= ~(SFGAO_CANMOVE | SFGAO_CANRENAME | SFGAO_CANDELETE);
    }
    info->dwAttributes = bits & wanted;
  }
  if (flags & SHGFI_EXETYPE) {
    if (!have_path) return 0;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return executable_type(path);
  }
  if (flags & SHGFI_DISPLAYNAME) {
    if (virtual_item && pidl) { if (!pidl->mkid.cb) w_copy(info->szDisplayName, ARRAY_LEN(info->szDisplayName), L"Desktop"); else item_display_name(pidl, info->szDisplayName, ARRAY_LEN(info->szDisplayName)); }
    else if (is_drive && !(flags & SHGFI_USEFILEATTRIBUTES)) {
      LPITEMIDLIST drive = il_drive(path);
      if (drive) { item_display_name(drive, info->szDisplayName, ARRAY_LEN(info->szDisplayName)); SHFree(drive); }
    } else w_copy(info->szDisplayName, ARRAY_LEN(info->szDisplayName), path_file_name(path));
  }
  if (flags & SHGFI_TYPENAME) {
    if (virtual_item) w_copy(info->szTypeName, ARRAY_LEN(info->szTypeName), L"System Folder");
    else type_name(path, attributes, is_drive, info->szTypeName, ARRAY_LEN(info->szTypeName));
  }
  if (flags & SHGFI_ICONLOCATION) {
    w_copy(info->szDisplayName, ARRAY_LEN(info->szDisplayName), L"");
    info->iIcon = system_icon_index(path, attributes, is_drive);
    if (is_program_extension(path_extension(path)) && have_path) { w_copy(info->szDisplayName, ARRAY_LEN(info->szDisplayName), path); info->iIcon = 0; }
  }
  if (flags & (SHGFI_ICON | SHGFI_SYSICONINDEX)) {
    info->iIcon = virtual_item ? (pidl && pidl->mkid.cb ? 15 : 34) : system_icon_index(path, attributes, is_drive);
    info->hIcon = NULL;
    /* there is no icon to make without a window system: the call "works" as far as it gives the index */
    result = (flags & SHGFI_SYSICONINDEX) ? 1 : 1;
  }
  return result;
}
EXPORT DWORD_PTR WINAPI SHGetFileInfoW(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags) {
  if (!info && (flags & ~(SHGFI_EXETYPE | SHGFI_PIDL | SHGFI_USEFILEATTRIBUTES))) return 0;
  if (flags & SHGFI_PIDL) return get_file_info(path, attributes, info, size, flags);
  if (!path || !*path) return 0;
  if (!info) { return (flags & SHGFI_EXETYPE) ? 0 : 0; }
  return get_file_info(path, attributes, info, size, flags);
}
EXPORT DWORD_PTR WINAPI SHGetFileInfoWWorker(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags) { return SHGetFileInfoW(path, attributes, info, size, flags); }
EXPORT DWORD_PTR WINAPI SHGetFileInfoA(LPCSTR path, DWORD attributes, SHFILEINFOA *info, UINT size, UINT flags) {
  SHFILEINFOW w;
  if (!info || size < sizeof(SHFILEINFOA)) return 0;
  ZeroMemory(&w, sizeof(w));
  w.dwAttributes = info->dwAttributes;
  w.iIcon = info->iIcon;
  w.hIcon = info->hIcon;
  DWORD_PTR r;
  if (flags & SHGFI_PIDL) r = SHGetFileInfoW((LPCWSTR)path, attributes, &w, sizeof(w), flags);
  else {
    WCHAR *wide = a_to_w(path);
    r = wide ? SHGetFileInfoW(wide, attributes, &w, sizeof(w), flags) : 0;
    mem_free(wide);
  }
  if (r || (flags & SHGFI_EXETYPE)) {
    info->hIcon = w.hIcon;
    info->iIcon = w.iIcon;
    info->dwAttributes = w.dwAttributes;
    if (flags & (SHGFI_DISPLAYNAME | SHGFI_ICONLOCATION)) w_to_a(w.szDisplayName, info->szDisplayName, sizeof(info->szDisplayName));
    if (flags & SHGFI_TYPENAME) w_to_a(w.szTypeName, info->szTypeName, sizeof(info->szTypeName));
  }
  return r;
}
EXPORT DWORD_PTR WINAPI SHGetFileInfo(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags) { return SHGetFileInfoW(path, attributes, info, size, flags); }

EXPORT HRESULT WINAPI SHGetStockIconInfo(SHSTOCKICONID id, UINT flags, SHSTOCKICONINFO *info) {
  if (!info || info->cbSize != sizeof(*info)) return E_INVALIDARG;
  if ((int)id < 0 || id >= SIID_MAX_ICONS) return E_INVALIDARG;
  info->hIcon = NULL;
  info->iSysImageIndex = (int)id;
  info->iIcon = (int)id;
  info->szPath[0] = 0;
  if (flags & SHGSI_ICONLOCATION) {
    WCHAR system[MAX_PATH];
    if (GetSystemDirectoryW(system, ARRAY_LEN(system)) && path_add_sep(system, ARRAY_LEN(system))) { w_cat(system, ARRAY_LEN(system), L"shell32.dll"); w_copy(info->szPath, ARRAY_LEN(info->szPath), system); }
  }
  return S_OK;
}
EXPORT HRESULT WINAPI SHGetImageList(int which, REFIID riid, void **out) { (void)which; (void)riid; if (out) *out = NULL; return E_NOINTERFACE; }
EXPORT BOOL WINAPI Shell_GetImageLists(HIMAGELIST *large, HIMAGELIST *small) { if (large) *large = NULL; if (small) *small = NULL; return FALSE; }
EXPORT int WINAPI Shell_GetCachedImageIndexW(LPCWSTR path, int index, UINT flags) { (void)path; (void)index; (void)flags; return -1; }
EXPORT int WINAPI Shell_GetCachedImageIndexA(LPCSTR path, int index, UINT flags) { (void)path; (void)index; (void)flags; return -1; }
EXPORT int WINAPI Shell_GetCachedImageIndex(LPCWSTR path, int index, UINT flags) { return Shell_GetCachedImageIndexW(path, index, flags); }
EXPORT int WINAPI SHGetIconOverlayIndexW(LPCWSTR path, int index) { (void)path; (void)index; return -1; }
EXPORT int WINAPI SHGetIconOverlayIndexA(LPCSTR path, int index) { (void)path; (void)index; return -1; }
EXPORT HRESULT WINAPI SHLoadNonloadedIconOverlayIdentifiers(void) { return S_OK; }
EXPORT int WINAPI SHMapPIDLToSystemImageListIndex(IShellFolder *folder, PCUITEMID_CHILD pidl, int *open_index) {
  (void)folder;
  if (open_index) *open_index = -1;
  if (!pidl) return -1;
  return item_is_folder_item(pidl) ? 3 : (item_is_drive(pidl) ? 8 : 0);
}
EXPORT HRESULT WINAPI SHMapIDListToImageListIndexAsync(void *pool, IShellFolder *folder, PCIDLIST_RELATIVE pidl, UINT flags, void *callback, void *data, void *extra, int *index, int *open_index) {
  (void)pool; (void)folder; (void)pidl; (void)flags; (void)callback; (void)data; (void)extra; (void)index; (void)open_index;
  return E_NOTIMPL;
}
EXPORT void WINAPI SHUpdateImageW(LPCWSTR file, int index, UINT flags, int image) { (void)file; (void)index; (void)flags; (void)image; }
EXPORT void WINAPI SHUpdateImageA(LPCSTR file, int index, UINT flags, int image) { (void)file; (void)index; (void)flags; (void)image; }
EXPORT int WINAPI SHHandleUpdateImage(PCIDLIST_ABSOLUTE pidl) { (void)pidl; return -1; }

/* ---- icons in files --------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { int count; } IconCount;
static BOOL CALLBACK count_icon_group(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) { (void)module; (void)type; (void)name; ++((IconCount *)param)->count; return TRUE; }

/* The number of icons in a file (a program or a library: its icon groups; an icon file: one). */
static UINT icon_count(const WCHAR *file) {
  if (!path_exists(file)) return 0;
  if (!wicmp(path_extension(file), L".ico")) return 1;
  HMODULE module = LoadLibraryExW(file, NULL, LOAD_LIBRARY_AS_DATAFILE);
  if (!module) return 0;
  IconCount c = {0};
  EnumResourceNamesW(module, RT_GROUP_ICON, count_icon_group, (LONG_PTR)&c);
  FreeLibrary(module);
  return (UINT)c.count;
}
/* The icons need user32's CreateIconFromResourceEx, which only exists with a window system: until then a query for the count is all that works. */
EXPORT UINT WINAPI ExtractIconExW(LPCWSTR file, int index, HICON *large, HICON *small, UINT count) {
  if (!file) return 0;
  if (index == -1 && !large && !small) return icon_count(file);
  if (large) for (UINT i = 0; i < count; ++i) large[i] = NULL;
  if (small) for (UINT i = 0; i < count; ++i) small[i] = NULL;
  return 0;
}
EXPORT UINT WINAPI ExtractIconExA(LPCSTR file, int index, HICON *large, HICON *small, UINT count) {
  WCHAR *w = a_to_w(file);
  const UINT r = ExtractIconExW(w, index, large, small, count);
  mem_free(w);
  return r;
}
EXPORT UINT WINAPI ExtractIconEx(LPCWSTR file, int index, HICON *large, HICON *small, UINT count) { return ExtractIconExW(file, index, large, small, count); }
EXPORT HICON WINAPI ExtractIconW(HINSTANCE instance, LPCWSTR file, UINT index) {
  (void)instance;
  if (index == 0xFFFFFFFFu) return (HICON)(ULONG_PTR)icon_count(file);
  return NULL;
}
EXPORT HICON WINAPI ExtractIconA(HINSTANCE instance, LPCSTR file, UINT index) {
  WCHAR *w = a_to_w(file);
  HICON r = ExtractIconW(instance, w, index);
  mem_free(w);
  return r;
}
EXPORT HICON WINAPI ExtractAssociatedIconW(HINSTANCE instance, LPWSTR path, WORD *index) { (void)instance; (void)path; if (index) *index = 0; return NULL; }
EXPORT HICON WINAPI ExtractAssociatedIconA(HINSTANCE instance, LPSTR path, WORD *index) { (void)instance; (void)path; if (index) *index = 0; return NULL; }
EXPORT HICON WINAPI ExtractAssociatedIconExW(HINSTANCE instance, LPWSTR path, WORD *index, WORD *id) { (void)instance; (void)path; if (index) *index = 0; if (id) *id = 0; return NULL; }
EXPORT HICON WINAPI ExtractAssociatedIconExA(HINSTANCE instance, LPSTR path, WORD *index, WORD *id) { (void)instance; (void)path; if (index) *index = 0; if (id) *id = 0; return NULL; }
EXPORT HICON WINAPI DuplicateIcon(HINSTANCE instance, HICON icon) { (void)instance; return CopyIcon(icon); }
EXPORT UINT WINAPI SHExtractIconsW(LPCWSTR file, int index, int cx, int cy, HICON *icons, UINT *ids, UINT count, UINT flags) {
  (void)cx; (void)cy; (void)ids; (void)count; (void)flags;
  if (!file) return 0;
  if (!icons) return icon_count(file);
  return 0;
}
EXPORT HICON WINAPI ExtractIconResInfoW(HANDLE file, LPWSTR name, WORD index, WORD *size, WORD *count) { (void)file; (void)name; (void)index; if (size) *size = 0; if (count) *count = 0; return NULL; }
EXPORT HICON WINAPI ExtractIconResInfoA(HANDLE file, LPSTR name, WORD index, WORD *size, WORD *count) { (void)file; (void)name; (void)index; if (size) *size = 0; if (count) *count = 0; return NULL; }
EXPORT UINT WINAPI InternalExtractIconListW(HANDLE file, LPWSTR name, LPINT count) { (void)file; (void)name; if (count) *count = 0; return 0; }
EXPORT UINT WINAPI InternalExtractIconListA(HANDLE file, LPSTR name, LPINT count) { (void)file; (void)name; if (count) *count = 0; return 0; }
EXPORT void WINAPI FreeIconList(HICON *icons, DWORD count) { (void)icons; (void)count; }
EXPORT HRESULT WINAPI SHDefExtractIconW(LPCWSTR file, int index, UINT flags, HICON *large, HICON *small, UINT size) { (void)file; (void)index; (void)flags; (void)size; if (large) *large = NULL; if (small) *small = NULL; return S_FALSE; }
EXPORT HRESULT WINAPI SHDefExtractIconA(LPCSTR file, int index, UINT flags, HICON *large, HICON *small, UINT size) { (void)file; (void)index; (void)flags; (void)size; if (large) *large = NULL; if (small) *small = NULL; return S_FALSE; }
EXPORT HRESULT WINAPI SHCreateFileExtractIconW(LPCWSTR file, DWORD attributes, REFIID riid, void **out) { (void)file; (void)attributes; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT int WINAPI PickIconDlg(HWND owner, LPWSTR path, UINT capacity, int *index) { (void)owner; (void)path; (void)capacity; (void)index; return 0; }

/* ---- notification icons and application bars ---------------------------------------------------------------------------------------------------------------- */

typedef struct { HWND window; UINT id; UINT flags, callback_message; HICON icon; WCHAR tip[128]; DWORD state, version; } TrayIcon;
static TrayIcon g_tray[64];

static BOOL tray_operation(DWORD message, const NOTIFYICONDATAW *d) {
  BOOL ok = FALSE;
  if (!d || d->cbSize < NOTIFYICONDATAW_V1_SIZE) return FALSE;
  EnterCriticalSection(&g_lock);
  TrayIcon *slot = NULL, *free_slot = NULL;
  for (size_t i = 0; i < ARRAY_LEN(g_tray); ++i) {
    if (g_tray[i].window == d->hWnd && g_tray[i].id == d->uID && g_tray[i].window) slot = &g_tray[i];
    else if (!g_tray[i].window && !free_slot) free_slot = &g_tray[i];
  }
  switch (message) {
    case NIM_ADD:
      if (!slot && free_slot && d->hWnd) {
        slot = free_slot;
        ZeroMemory(slot, sizeof(*slot));
        slot->window = d->hWnd;
        slot->id = d->uID;
        slot->flags = d->uFlags;
        slot->callback_message = d->uCallbackMessage;
        slot->icon = d->hIcon;
        if (d->uFlags & NIF_TIP) w_copy(slot->tip, ARRAY_LEN(slot->tip), d->szTip);
        ok = TRUE;
      }
      break;
    case NIM_MODIFY:
      if (slot) {
        if (d->uFlags & NIF_MESSAGE) slot->callback_message = d->uCallbackMessage;
        if (d->uFlags & NIF_ICON) slot->icon = d->hIcon;
        if (d->uFlags & NIF_TIP) w_copy(slot->tip, ARRAY_LEN(slot->tip), d->szTip);
        ok = TRUE;
      }
      break;
    case NIM_DELETE: if (slot) { ZeroMemory(slot, sizeof(*slot)); ok = TRUE; } break;
    case NIM_SETFOCUS: ok = slot != NULL; break;
    case NIM_SETVERSION: if (slot) { slot->version = d->uVersion; ok = TRUE; } break;
    default: break;
  }
  LeaveCriticalSection(&g_lock);
  return ok;
}
EXPORT BOOL WINAPI Shell_NotifyIconW(DWORD message, NOTIFYICONDATAW *data) { return tray_operation(message, data); }
EXPORT BOOL WINAPI Shell_NotifyIcon(DWORD message, NOTIFYICONDATAW *data) { return Shell_NotifyIconW(message, data); }
EXPORT BOOL WINAPI Shell_NotifyIconA(DWORD message, NOTIFYICONDATAA *data) {
  NOTIFYICONDATAW w;
  if (!data || data->cbSize < NOTIFYICONDATAA_V1_SIZE) return FALSE;
  ZeroMemory(&w, sizeof(w));
  w.cbSize = sizeof(w);
  w.hWnd = data->hWnd; w.uID = data->uID; w.uFlags = data->uFlags; w.uCallbackMessage = data->uCallbackMessage; w.hIcon = data->hIcon;
  MultiByteToWideChar(CP_ACP, 0, data->szTip, -1, w.szTip, ARRAY_LEN(w.szTip));
  w.uVersion = data->uVersion;
  return tray_operation(message, &w);
}
EXPORT HRESULT WINAPI Shell_NotifyIconGetRect(const NOTIFYICONIDENTIFIER *id, RECT *rect) { (void)id; if (rect) ZeroMemory(rect, sizeof(*rect)); return E_FAIL; }
EXPORT UINT_PTR WINAPI SHAppBarMessage(DWORD message, APPBARDATA *data) { (void)message; (void)data; return 0; }
EXPORT LRESULT WINAPI ShellHookProc(int code, WPARAM wparam, LPARAM lparam) { (void)code; (void)wparam; (void)lparam; return 0; }

/* ---- dialogs ----------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT PIDLIST_ABSOLUTE WINAPI SHBrowseForFolderW(LPBROWSEINFOW info) { (void)info; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT PIDLIST_ABSOLUTE WINAPI SHBrowseForFolderA(LPBROWSEINFOA info) { (void)info; SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT PIDLIST_ABSOLUTE WINAPI SHBrowseForFolder(LPBROWSEINFOW info) { return SHBrowseForFolderW(info); }
EXPORT BOOL WINAPI GetFileNameFromBrowse(HWND owner, LPWSTR file, UINT capacity, LPCWSTR dir, LPCWSTR ext, LPCWSTR filter, LPCWSTR title) { (void)owner; (void)file; (void)capacity; (void)dir; (void)ext; (void)filter; (void)title; return FALSE; }
EXPORT int WINAPI RestartDialog(HWND owner, LPCWSTR reason, DWORD flags) { (void)owner; (void)reason; (void)flags; return IDNO; }
EXPORT int WINAPI RestartDialogEx(HWND owner, LPCWSTR reason, DWORD flags, DWORD why) { (void)owner; (void)reason; (void)flags; (void)why; return IDNO; }
EXPORT DWORD WINAPI SHFormatDrive(HWND owner, UINT drive, UINT format, UINT options) { (void)owner; (void)drive; (void)format; (void)options; return SHFMT_NOFORMAT; }
EXPORT BOOL WINAPI SHFindFiles(PCIDLIST_ABSOLUTE folder, PCIDLIST_ABSOLUTE save) { (void)folder; (void)save; return FALSE; }
EXPORT IContextMenu *WINAPI SHFind_InitMenuPopup(HMENU menu, HWND owner, UINT first, UINT last) { (void)menu; (void)owner; (void)first; (void)last; return NULL; }
EXPORT BOOL WINAPI SHInvokePrinterCommandW(HWND owner, UINT command, LPCWSTR printer, LPCWSTR server, BOOL modal) { (void)owner; (void)command; (void)printer; (void)server; (void)modal; return FALSE; }
EXPORT BOOL WINAPI SHInvokePrinterCommandA(HWND owner, UINT command, LPCSTR printer, LPCSTR server, BOOL modal) { (void)owner; (void)command; (void)printer; (void)server; (void)modal; return FALSE; }
EXPORT BOOL WINAPI SHObjectProperties(HWND owner, DWORD type, LPCWSTR object, LPCWSTR page) { (void)owner; (void)type; (void)object; (void)page; return FALSE; }
EXPORT BOOL WINAPI SHOpenPropSheetW(LPCWSTR caption, HKEY *keys, UINT key_count, const CLSID *handler, IDataObject *data, IShellBrowser *browser, LPCWSTR start_page) {
  (void)caption; (void)keys; (void)key_count; (void)handler; (void)data; (void)browser; (void)start_page;
  return FALSE;
}
EXPORT HRESULT WINAPI SHMultiFileProperties(IDataObject *data, DWORD flags) { (void)data; (void)flags; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHOpenFolderAndSelectItems(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY items, DWORD flags) {
  WCHAR path[MAX_PATH_EX];
  (void)count; (void)items; (void)flags;
  if (!folder) return E_INVALIDARG;
  if (!pidl_to_path(folder, path, ARRAY_LEN(path))) return E_FAIL;
  return (INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) > 32 ? S_OK : E_FAIL;
}
EXPORT HRESULT WINAPI SHLimitInputEdit(HWND edit, IShellFolder *folder) { (void)edit; (void)folder; return E_NOTIMPL; }
EXPORT BOOL WINAPI InitNetworkAddressControl(void) { return FALSE; }
EXPORT HRESULT WINAPI SHStartNetConnectionDialogW(HWND owner, LPCWSTR remote, DWORD type) { (void)owner; (void)remote; (void)type; return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); }

/* A message box with a text that may be a resource id and a format (the %1 ... of FormatMessage). */
EXPORT int WINAPIV ShellMessageBoxW(HINSTANCE instance, HWND owner, LPCWSTR text, LPCWSTR title, UINT style, ...) {
  WCHAR title_buffer[256], text_buffer[1024], *formatted = NULL;
  if (IS_INTRESOURCE(title)) {
    if (!LoadStringW(instance, (UINT)(ULONG_PTR)title, title_buffer, ARRAY_LEN(title_buffer))) title_buffer[0] = 0;
    title = title_buffer;
  }
  if (IS_INTRESOURCE(text)) {
    if (!LoadStringW(instance, (UINT)(ULONG_PTR)text, text_buffer, ARRAY_LEN(text_buffer))) text_buffer[0] = 0;
    text = text_buffer;
  }
  __builtin_va_list args;
  __builtin_va_start(args, style);
  FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER, text, 0, 0, (LPWSTR)&formatted, 0, &args);
  __builtin_va_end(args);
  const int r = MessageBoxW(owner, formatted ? formatted : text, title, style);
  if (formatted) LocalFree(formatted);
  return r;
}
EXPORT int WINAPIV ShellMessageBoxA(HINSTANCE instance, HWND owner, LPCSTR text, LPCSTR title, UINT style, ...) {
  char title_buffer[256], text_buffer[1024], *formatted = NULL;
  if (IS_INTRESOURCE(title)) { if (!LoadStringA(instance, (UINT)(ULONG_PTR)title, title_buffer, sizeof(title_buffer))) title_buffer[0] = 0; title = title_buffer; }
  if (IS_INTRESOURCE(text)) { if (!LoadStringA(instance, (UINT)(ULONG_PTR)text, text_buffer, sizeof(text_buffer))) text_buffer[0] = 0; text = text_buffer; }
  __builtin_va_list args;
  __builtin_va_start(args, style);
  FormatMessageA(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ALLOCATE_BUFFER, text, 0, 0, (LPSTR)&formatted, 0, &args);
  __builtin_va_end(args);
  const int r = MessageBoxA(owner, formatted ? formatted : text, title, style);
  if (formatted) LocalFree(formatted);
  return r;
}
EXPORT BOOL WINAPI ShellAboutW(HWND owner, LPCWSTR app, LPCWSTR other, HICON icon) {
  WCHAR title[300], text[1024];
  (void)icon;
  w_copy(title, ARRAY_LEN(title), L"About ");
  const WCHAR *name = app ? app : L"";
  const WCHAR *hash = w_find(name, '#');
  WCHAR shown[200];
  w_copy_n(shown, ARRAY_LEN(shown), name, hash ? (size_t)(hash - name) : wlen(name));
  w_cat(title, ARRAY_LEN(title), shown);
  w_copy(text, ARRAY_LEN(text), hash ? hash + 1 : shown);
  if (other) { w_cat(text, ARRAY_LEN(text), L"\n"); w_cat(text, ARRAY_LEN(text), other); }
  MessageBoxW(owner, text, title, MB_OK);
  return TRUE;
}
EXPORT BOOL WINAPI ShellAboutA(HWND owner, LPCSTR app, LPCSTR other, HICON icon) {
  WCHAR *a = a_to_w(app), *o = a_to_w(other);
  const BOOL r = ShellAboutW(owner, a, o, icon);
  mem_free(a); mem_free(o);
  return r;
}

/* ---- menus ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* Merges the items of one menu into another, the way the shell does for the menus of its extensions. Returns the next free id. */
EXPORT UINT WINAPI Shell_MergeMenus(HMENU dest, HMENU source, UINT position, UINT id_min, UINT id_max, ULONG flags) {
  if (!dest || !source) return id_min;
  UINT id_next = id_min;
  const int count = GetMenuItemCount(source);
  if (flags & MM_SUBMENUSHAVEIDS) {}
  for (int i = 0; i < count; ++i) {
    MENUITEMINFOW mi;
    WCHAR text[256];
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_CHECKMARKS | MIIM_TYPE | MIIM_DATA;
    mi.dwTypeData = text;
    mi.cch = ARRAY_LEN(text);
    if (!GetMenuItemInfoW(source, (UINT)i, TRUE, &mi)) continue;
    if (mi.fType & MFT_SEPARATOR) {}
    else if (mi.hSubMenu) {
      HMENU sub = CreatePopupMenu();
      const UINT used = Shell_MergeMenus(sub, mi.hSubMenu, 0, id_next, id_max, flags & ~MM_ADDSEPARATOR);
      id_next = used;
      mi.hSubMenu = sub;
    } else {
      mi.wID += id_min;
      if (mi.wID > id_max) break;
      if (mi.wID >= id_next) id_next = mi.wID + 1;
    }
    InsertMenuItemW(dest, position++, TRUE, &mi);
  }
  return id_next;
}

/* ---- settings --------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT DWORD WINAPI SHRestricted(RESTRICTIONS restriction) {
  WCHAR value[16];
  DWORD data = 0, size = sizeof(data);
  static const WCHAR kPolicies[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
  (void)value;
  HKEY key;
  /* the policy values are named by the number of the restriction in the registry's "Policies\\Explorer" key as REG_DWORD values of the same number */
  WCHAR name[16];
  DWORD n = restriction, len = 0;
  WCHAR tmp[12];
  do { tmp[len++] = (WCHAR)('0' + n % 10); n /= 10; } while (n);
  for (DWORD i = 0; i < len; ++i) name[i] = tmp[len - 1 - i];
  name[len] = 0;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kPolicies, 0, KEY_READ, &key) == ERROR_SUCCESS) {
    DWORD type;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&data, &size) != ERROR_SUCCESS || type != REG_DWORD) data = 0;
    RegCloseKey(key);
  }
  return data;
}
static SHELLSTATEW g_shell_state;
static BOOL g_shell_state_set;
static void default_shell_state(SHELLSTATEW *s) {
  ZeroMemory(s, sizeof(*s));
  s->fShowExtensions = FALSE;
  s->fShowInfoTip = TRUE;
  s->fNoConfirmRecycle = FALSE;
  s->fShowCompColor = TRUE;
  s->fWebView = TRUE;
  s->fDontPrettyPath = FALSE;
  s->fShowAttribCol = FALSE;
  s->fMapNetDrvBtn = FALSE;
  s->fShowSuperHidden = FALSE;
  s->version = 10;
  s->lParamSort = 0;
  s->iSortDirection = 1;
  s->fSepProcess = FALSE;
}
EXPORT void WINAPI SHGetSettings(SHELLFLAGSTATE *flags, DWORD mask) {
  (void)mask;
  if (!flags) return;
  ZeroMemory(flags, sizeof(*flags));
  flags->fShowExtensions = 0;
  flags->fWin95Classic = 0;
  flags->fDoubleClickInWebView = 1;
}
EXPORT void WINAPI SHGetSetSettings(SHELLSTATEW *state, DWORD mask, BOOL set) {
  if (!state) return;
  EnterCriticalSection(&g_lock);
  if (!g_shell_state_set) { default_shell_state(&g_shell_state); g_shell_state_set = TRUE; }
  if (set) { if (mask & SSF_SHOWEXTENSIONS) g_shell_state.fShowExtensions = state->fShowExtensions; if (mask & SSF_SHOWALLOBJECTS) g_shell_state.fShowAllObjects = state->fShowAllObjects; if (mask & SSF_SHOWSYSFILES) g_shell_state.fShowSysFiles = state->fShowSysFiles; }
  else *state = g_shell_state;
  LeaveCriticalSection(&g_lock);
}
EXPORT BOOL WINAPI ReadCabinetState(CABINETSTATE *state, int length) {
  if (!state || length < (int)sizeof(CABINETSTATE)) return FALSE;
  ZeroMemory(state, sizeof(*state));
  state->cLength = sizeof(CABINETSTATE);
  state->nVersion = 2;
  state->fMenuEnumFilter = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
  return TRUE;
}
EXPORT BOOL WINAPI WriteCabinetState(CABINETSTATE *state) { return state != NULL; }
EXPORT HRESULT WINAPI SHGetSetFolderCustomSettingsW(LPSHFOLDERCUSTOMSETTINGS settings, LPCWSTR path, DWORD flags) { (void)settings; (void)path; (void)flags; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHGetSetFolderCustomSettings(LPSHFOLDERCUSTOMSETTINGS settings, LPCWSTR path, DWORD flags) { return SHGetSetFolderCustomSettingsW(settings, path, flags); }
EXPORT HINSTANCE WINAPI SHGetShellStyleHInstance(void) { return g_module; }

/* ---- the folder's display name in desktop.ini ---------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI SHGetLocalizedName(LPCWSTR path, LPWSTR module, UINT module_capacity, int *id) {
  WCHAR ini[MAX_PATH_EX], value[MAX_PATH_EX];
  if (!path || !module || !id) return E_INVALIDARG;
  if (!path_join(ini, ARRAY_LEN(ini), path, L"desktop.ini")) return E_FAIL;
  if (!GetPrivateProfileStringW(L".ShellClassInfo", L"LocalizedResourceName", L"", value, ARRAY_LEN(value), ini) || value[0] != '@') return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  const WCHAR *comma = w_find(value, ',');
  if (!comma) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  WCHAR dll[MAX_PATH_EX];
  w_copy_n(dll, ARRAY_LEN(dll), value + 1, (size_t)(comma - value - 1));
  *id = 0;
  for (const WCHAR *c = comma + 1; *c; ++c) if (*c >= '0' && *c <= '9') *id = *id * 10 + (*c - '0');
  ExpandEnvironmentStringsW(dll, module, module_capacity);
  return S_OK;
}
EXPORT HRESULT WINAPI SHSetLocalizedName(PCWSTR path, PCWSTR module, int id) {
  WCHAR ini[MAX_PATH_EX], value[MAX_PATH_EX], digits[16];
  if (!path || !module) return E_INVALIDARG;
  if (!path_join(ini, ARRAY_LEN(ini), path, L"desktop.ini")) return E_FAIL;
  int v = id < 0 ? -id : id, n = 0;
  WCHAR tmp[16];
  do { tmp[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  int o = 0;
  if (id < 0) digits[o++] = '-';
  while (n) digits[o++] = tmp[--n];
  digits[o] = 0;
  w_copy(value, ARRAY_LEN(value), L"@");
  w_cat(value, ARRAY_LEN(value), module);
  w_cat(value, ARRAY_LEN(value), L",");
  w_cat(value, ARRAY_LEN(value), digits);
  if (!WritePrivateProfileStringW(L".ShellClassInfo", L"LocalizedResourceName", value, ini)) return hr_last_error();
  SetFileAttributesW(ini, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
  SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY | (GetFileAttributesW(path) & ~FILE_ATTRIBUTE_READONLY));
  return S_OK;
}
EXPORT HRESULT WINAPI SHRemoveLocalizedName(LPCWSTR path) {
  WCHAR ini[MAX_PATH_EX];
  if (!path || !path_join(ini, ARRAY_LEN(ini), path, L"desktop.ini")) return E_INVALIDARG;
  WritePrivateProfileStringW(L".ShellClassInfo", L"LocalizedResourceName", NULL, ini);
  return S_OK;
}

/* ---- what has no window system to work with, and the odds and ends ------------------------------------------------------------------------------------------ */

EXPORT void WINAPI SHAddToRecentDocs(UINT flags, LPCVOID item) { (void)flags; (void)item; }
EXPORT HRESULT WINAPI SHQueryUserNotificationState(QUERY_USER_NOTIFICATION_STATE *state) { if (!state) return E_INVALIDARG; *state = QUNS_ACCEPTS_NOTIFICATIONS; return S_OK; }
EXPORT BOOL WINAPI SignalFileOpen(PCIDLIST_ABSOLUTE pidl) { (void)pidl; return FALSE; }
EXPORT BOOL WINAPI WaitForExplorerRestartW(HANDLE event, DWORD timeout) { (void)event; (void)timeout; return FALSE; }
EXPORT HRESULT WINAPI SHFlushClipboard(void) { return S_OK; }
EXPORT HRESULT WINAPI SHIsFileAvailableOffline(LPCWSTR path, LPDWORD status) { (void)path; if (status) *status = 0; return E_FAIL; }
EXPORT HRESULT WINAPI SHGetUnreadMailCountW(HKEY key, LPCWSTR address, DWORD *count, FILETIME *time, LPWSTR application, int capacity) {
  (void)key; (void)address; (void)time; (void)application; (void)capacity;
  if (count) *count = 0;
  return E_FAIL;
}
EXPORT HRESULT WINAPI SHSetUnreadMailCountW(LPCWSTR address, DWORD count, LPCWSTR command) { (void)address; (void)count; (void)command; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHEnumerateUnreadMailAccountsW(HKEY key, DWORD index, LPWSTR address, int capacity) { (void)key; (void)index; (void)address; (void)capacity; return E_FAIL; }
EXPORT BOOL WINAPI RegenerateUserEnvironment(void **previous, BOOL set_current) { (void)set_current; if (previous) *previous = NULL; return TRUE; }
EXPORT HRESULT WINAPI SHLoadInProc(REFCLSID clsid) {
  IUnknown *object = NULL;
  const HRESULT hr = SHCoCreateInstance(NULL, clsid, NULL, &IID_IUnknown, (void **)&object);
  if (object) IUnknown_Release(object);
  return hr;
}
EXPORT HRESULT WINAPI SHEnableServiceObject(REFCLSID clsid, BOOL enable) { (void)clsid; (void)enable; return S_OK; }
static IUnknown *g_instance_explorer;
EXPORT HRESULT WINAPI SHGetInstanceExplorer(IUnknown **out) {
  if (!out) return E_POINTER;
  *out = g_instance_explorer;
  if (*out) { IUnknown_AddRef(*out); return S_OK; }
  return E_FAIL;
}
EXPORT HRESULT WINAPI SHGetInstanceExplorerWorker(IUnknown **out) { return SHGetInstanceExplorer(out); }
EXPORT void WINAPI SHSetInstanceExplorer(IUnknown *object) { g_instance_explorer = object; }
EXPORT LRESULT WINAPI SHShellFolderView_Message(HWND window, UINT message, LPARAM lparam) { (void)window; (void)message; (void)lparam; return 0; }
EXPORT HRESULT WINAPI SHCreateShellFolderView(const SFV_CREATE *create, IShellView **out) { (void)create; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHCreateShellFolderViewEx(CSFV *create, IShellView **out) { (void)create; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CDefFolderMenu_Create(PCIDLIST_ABSOLUTE folder, HWND owner, UINT count, PCUITEMID_CHILD_ARRAY items, IShellFolder *parent, LPFNDFMCALLBACK callback, UINT keys, const HKEY *key_list, IContextMenu **out) {
  (void)folder; (void)owner; (void)count; (void)items; (void)parent; (void)callback; (void)keys; (void)key_list;
  if (out) *out = NULL;
  return E_NOTIMPL;
}
EXPORT HRESULT WINAPI CDefFolderMenu_Create2(PCIDLIST_ABSOLUTE folder, HWND owner, UINT count, PCUITEMID_CHILD_ARRAY items, IShellFolder *parent, LPFNDFMCALLBACK callback, UINT keys, const HKEY *key_list, IContextMenu **out) {
  return CDefFolderMenu_Create(folder, owner, count, items, parent, callback, keys, key_list, out);
}
EXPORT HRESULT WINAPI SHCreateDefaultContextMenu(const DEFCONTEXTMENU *info, REFIID riid, void **out) { (void)info; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHCreateQueryCancelAutoPlayMoniker(IMoniker **out) { if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHCreateLocalServerRunDll(REFCLSID clsid, REFIID riid, void **out) { (void)clsid; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHCreateDefaultPropertiesOp(IShellItem *item, IFileOperation **out) { (void)item; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHSetDefaultProperties(HWND owner, IShellItem *item, DWORD flags, IFileOperationProgressSink *sink) { (void)owner; (void)item; (void)flags; (void)sink; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHAddDefaultPropertiesByExt(PCWSTR ext, IPropertyStore *store) { (void)ext; (void)store; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHGetPropertyStoreForWindow(HWND window, REFIID riid, void **out) { (void)window; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHGetPropertyStoreFromIDList(PCIDLIST_ABSOLUTE pidl, GETPROPERTYSTOREFLAGS flags, REFIID riid, void **out) { (void)pidl; (void)flags; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHGetPropertyStoreFromParsingName(PCWSTR name, IBindCtx *bind, GETPROPERTYSTOREFLAGS flags, REFIID riid, void **out) { (void)name; (void)bind; (void)flags; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHGetTemporaryPropertyForItem(IShellItem *item, REFPROPERTYKEY key, PROPVARIANT *value) { (void)item; (void)key; (void)value; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHSetTemporaryPropertyForItem(IShellItem *item, REFPROPERTYKEY key, REFPROPVARIANT value) { (void)item; (void)key; (void)value; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHPropStgCreate(IPropertySetStorage *a, REFFMTID b, const CLSID *c, DWORD d, DWORD e, DWORD f, IPropertyStorage **g, UINT *h) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; if (g) *g = NULL; if (h) *h = 0; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHPropStgReadMultiple(IPropertyStorage *a, UINT b, ULONG c, PROPSPEC const d[], PROPVARIANT e[]) { (void)a; (void)b; (void)c; (void)d; (void)e; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHPropStgWriteMultiple(IPropertyStorage *a, UINT *b, ULONG c, PROPSPEC const d[], PROPVARIANT e[], PROPID f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHResolveLibrary(IShellItem *item) { (void)item; return E_NOTIMPL; }
EXPORT HRESULT WINAPI SHShowManageLibraryUI(IShellItem *library, HWND owner, LPCWSTR title, LPCWSTR instruction, LIBRARYMANAGEDIALOGOPTIONS options) { (void)library; (void)owner; (void)title; (void)instruction; (void)options; return E_NOTIMPL; }
EXPORT HRESULT WINAPI StgMakeUniqueName(IStorage *parent, PCWSTR templ, DWORD flags, REFIID riid, void **out) { (void)parent; (void)templ; (void)flags; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI GetSystemPersistedStorageItemList(void *a, void *b) { (void)a; (void)b; return E_NOTIMPL; }
EXPORT HPSXA WINAPI SHCreatePropSheetExtArray(HKEY key, PCWSTR subkey, UINT max) { (void)key; (void)subkey; (void)max; return NULL; }
EXPORT void WINAPI SHDestroyPropSheetExtArray(HPSXA array) { (void)array; }
EXPORT UINT WINAPI SHAddFromPropSheetExtArray(HPSXA array, LPFNADDPROPSHEETPAGE add, LPARAM lparam) { (void)array; (void)add; (void)lparam; return 0; }
EXPORT UINT WINAPI SHReplaceFromPropSheetExtArray(HPSXA array, UINT id, LPFNADDPROPSHEETPAGE add, LPARAM lparam) { (void)array; (void)id; (void)add; (void)lparam; return 0; }
EXPORT HANDLE WINAPI PifMgr_CloseProperties(HANDLE h, UINT flags) { (void)h; (void)flags; return NULL; }
EXPORT int WINAPI PifMgr_GetProperties(HANDLE h, PCSTR group, void *buffer, int size, UINT flags) { (void)h; (void)group; (void)buffer; (void)size; (void)flags; return 0; }
EXPORT HANDLE WINAPI PifMgr_OpenProperties(PCWSTR name, PCWSTR file, UINT hotkey, UINT flags) { (void)name; (void)file; (void)hotkey; (void)flags; return NULL; }
EXPORT int WINAPI PifMgr_SetProperties(HANDLE h, PCSTR group, const void *buffer, int size, UINT flags) { (void)h; (void)group; (void)buffer; (void)size; (void)flags; return 0; }
EXPORT int WINAPI CallCPLEntry16(HMODULE module, FARPROC proc, HWND owner, UINT message, LPARAM a, LPARAM b) { (void)module; (void)proc; (void)owner; (void)message; (void)a; (void)b; return 0; }
EXPORT BOOL WINAPI ExtractVersionResource16W(LPWSTR path, HANDLE out) { (void)path; (void)out; return FALSE; }
EXPORT LRESULT WINAPI AppCompat_RunDLLW(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; return 0; }
EXPORT void WINAPI RunAsNewUser_RunDLLW(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; }
EXPORT void WINAPI LaunchMSHelp_RunDLLW(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; }
EXPORT void WINAPI PrepareDiscForBurnRunDllW(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; }
EXPORT void WINAPI Control_RunDLLAsUserW(HWND owner, HINSTANCE instance, LPCWSTR command_line, int show) { (void)owner; (void)instance; (void)command_line; (void)show; }
EXPORT INT_PTR WINAPI FindExeDlgProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) { (void)dialog; (void)message; (void)wparam; (void)lparam; return 0; }
EXPORT HRESULT WINAPI CreateStorageItemFromShellItem_FullTrustCaller(IShellItem *item, REFIID riid, void **out) { (void)item; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CreateStorageItemFromShellItem_FullTrustCaller_ForPackage(IShellItem *item, PCWSTR package, REFIID riid, void **out) { (void)item; (void)package; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CreateStorageItemFromShellItem_FullTrustCaller_UseImplicitFlagsAndPackage(IShellItem *item, PCWSTR package, REFIID riid, void **out) { (void)item; (void)package; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CreateStorageItemFromShellItem_FullTrustCaller_ForPackage_WithProcessHandle(IShellItem *item, PCWSTR package, HANDLE process, REFIID riid, void **out) { (void)item; (void)package; (void)process; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI AssocGetDetailsOfPropKey(IShellFolder *folder, PCUITEMID_CHILD pidl, const PROPERTYKEY *key, VARIANT *value, BOOL *present) { (void)folder; (void)pidl; (void)key; (void)value; if (present) *present = FALSE; return E_NOTIMPL; }

/* A stream in memory over the bytes of a registry value: OpenRegStream reads the value into it; it is read, written and sought like a file and written back by Commit. */
EXPORT IStream *WINAPI OpenRegStream(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD mode) {
  (void)mode;
  HKEY k;
  if (RegOpenKeyExW(key, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return NULL;
  DWORD size = 0, type;
  if (RegQueryValueExW(k, value, NULL, &type, NULL, &size) != ERROR_SUCCESS) { RegCloseKey(k); return NULL; }
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
  if (!h) { RegCloseKey(k); return NULL; }
  void *p = GlobalLock(h);
  RegQueryValueExW(k, value, NULL, &type, p, &size);
  GlobalUnlock(h);
  RegCloseKey(k);
  IStream *stream = NULL;
  if (FAILED(CreateStreamOnHGlobal(h, TRUE, &stream))) { GlobalFree(h); return NULL; }
  return stream;
}
