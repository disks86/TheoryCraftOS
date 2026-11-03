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

/* The special folders. Every folder has a known-folder id (FOLDERID_*) and, for the old API, a CSIDL; a folder is either virtual (My Computer, the Recycle Bin: no path), a root
 * found from the system (Windows, Program Files, the profile ...) or a path relative to its parent. The per-user folders can be redirected in the registry
 * (HKCU\...\Explorer\User Shell Folders). Included by shell32.c. */

enum { KF_VIRTUAL = 1, KF_PERUSER = 2, KF_COMMON = 4 };
typedef enum {
  ROOT_NONE, ROOT_PROFILE, ROOT_USERS, ROOT_PUBLIC, ROOT_PROGRAMDATA, ROOT_PROGRAMFILES, ROOT_PROGRAMFILESX86, ROOT_COMMONFILES, ROOT_COMMONFILESX86, ROOT_WINDOWS, ROOT_SYSTEM,
  ROOT_SYSTEMX86
} FolderRoot;

typedef struct {
  const GUID *id;
  const WCHAR *name;      /* the canonical name; for a per-user folder also the value in User Shell Folders */
  DWORD kind;
  FolderRoot root;
  const GUID *parent;
  const WCHAR *relative;
  DWORD attributes;
  const WCHAR *clsid;      /* a virtual folder: its parsing name "::{...}" */
} KnownFolder;

#define KFD(id, name, kind, root, parent, relative, attributes, clsid) {&FOLDERID_##id, L##name, kind, root, parent, relative, attributes, clsid}
#define P(id) (&FOLDERID_##id)
#define RO FILE_ATTRIBUTE_READONLY

static const KnownFolder g_known_folders[] = {
  /* virtual folders */
  KFD(ComputerFolder, "MyComputerFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"),
  KFD(ControlPanelFolder, "ControlPanelFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{21EC2020-3AEA-1069-A2DD-08002B30309D}"),
  KFD(ConnectionsFolder, "ConnectionsFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{21EC2020-3AEA-1069-A2DD-08002B30309D}\\::{7007ACC7-3202-11D1-AAD2-00805FC1270E}"),
  KFD(ConflictFolder, "ConflictFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(Games, "Games", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{ED228FDF-9EA8-4870-83b1-96b02CFE0D52}"),
  KFD(HomeGroup, "HomeGroupFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{B4FB3F98-C1EA-428d-A78A-D1F5659CBA93}"),
  KFD(InternetFolder, "InternetFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{871C5380-42A0-1069-A2EA-08002B30309D}"),
  KFD(NetworkFolder, "NetworkFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"),
  KFD(PrintersFolder, "PrintersFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{21EC2020-3AEA-1069-A2DD-08002B30309D}\\::{2227A280-3AEA-1069-A2DE-08002B30309D}"),
  KFD(RecycleBinFolder, "RecycleBinFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, L"::{645FF040-5081-101B-9F08-00AA002F954E}"),
  KFD(AddNewPrograms, "AddNewProgramsFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(AppUpdates, "AppUpdatesFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(ChangeRemovePrograms, "ChangeRemoveProgramsFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(SyncManagerFolder, "SyncCenterFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(SyncSetupFolder, "SyncSetupFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(SyncResultsFolder, "SyncResultsFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(SearchHome, "SearchHomeFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(UsersFiles, "UsersFilesFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  KFD(UsersLibraries, "UsersLibrariesFolder", KF_VIRTUAL, ROOT_NONE, NULL, NULL, 0, NULL),
  /* fixed folders: found from the system */
  KFD(Windows, "Windows", 0, ROOT_WINDOWS, NULL, NULL, 0, NULL),
  KFD(System, "System", 0, ROOT_SYSTEM, NULL, NULL, 0, NULL),
  KFD(SystemX86, "SystemX86", 0, ROOT_SYSTEMX86, NULL, NULL, 0, NULL),
  KFD(Fonts, "Fonts", 0, ROOT_NONE, P(Windows), L"Fonts", 0, NULL),
  KFD(ResourceDir, "ResourceDir", 0, ROOT_NONE, P(Windows), L"Resources", 0, NULL),
  KFD(LocalizedResourcesDir, "LocalizedResourcesDir", 0, ROOT_NONE, P(ResourceDir), L"0409", 0, NULL),
  KFD(ProgramFiles, "ProgramFiles", 0, ROOT_PROGRAMFILES, NULL, NULL, 0, NULL),
  KFD(ProgramFilesX64, "ProgramFilesX64", 0, ROOT_PROGRAMFILES, NULL, NULL, 0, NULL),
  KFD(ProgramFilesX86, "ProgramFilesX86", 0, ROOT_PROGRAMFILESX86, NULL, NULL, 0, NULL),
  KFD(ProgramFilesCommon, "ProgramFilesCommon", 0, ROOT_COMMONFILES, NULL, NULL, 0, NULL),
  KFD(ProgramFilesCommonX64, "ProgramFilesCommonX64", 0, ROOT_COMMONFILES, NULL, NULL, 0, NULL),
  KFD(ProgramFilesCommonX86, "ProgramFilesCommonX86", 0, ROOT_COMMONFILESX86, NULL, NULL, 0, NULL),
  KFD(UserProfiles, "UserProfiles", 0, ROOT_USERS, NULL, NULL, RO, NULL),
  KFD(Public, "Public", KF_COMMON, ROOT_PUBLIC, NULL, NULL, RO, NULL),
  KFD(ProgramData, "Common AppData", KF_COMMON, ROOT_PROGRAMDATA, NULL, NULL, 0, NULL),
  /* common folders */
  KFD(CommonStartMenu, "Common Start Menu", KF_COMMON, ROOT_NONE, P(ProgramData), L"Microsoft\\Windows\\Start Menu", RO, NULL),
  KFD(CommonPrograms, "Common Programs", KF_COMMON, ROOT_NONE, P(CommonStartMenu), L"Programs", RO, NULL),
  KFD(CommonStartup, "Common Startup", KF_COMMON, ROOT_NONE, P(CommonPrograms), L"StartUp", RO, NULL),
  KFD(CommonAdminTools, "Common Administrative Tools", KF_COMMON, ROOT_NONE, P(CommonPrograms), L"Administrative Tools", RO, NULL),
  KFD(CommonTemplates, "Common Templates", KF_COMMON, ROOT_NONE, P(ProgramData), L"Microsoft\\Windows\\Templates", 0, NULL),
  KFD(CommonOEMLinks, "OEM Links", KF_COMMON, ROOT_NONE, P(ProgramData), L"OEM Links", 0, NULL),
  KFD(PublicDesktop, "Common Desktop", KF_COMMON, ROOT_NONE, P(Public), L"Desktop", RO, NULL),
  KFD(PublicDocuments, "Common Documents", KF_COMMON, ROOT_NONE, P(Public), L"Documents", RO, NULL),
  KFD(PublicDownloads, "CommonDownloads", KF_COMMON, ROOT_NONE, P(Public), L"Downloads", RO, NULL),
  KFD(PublicMusic, "CommonMusic", KF_COMMON, ROOT_NONE, P(Public), L"Music", RO, NULL),
  KFD(PublicPictures, "CommonPictures", KF_COMMON, ROOT_NONE, P(Public), L"Pictures", RO, NULL),
  KFD(PublicVideos, "CommonVideo", KF_COMMON, ROOT_NONE, P(Public), L"Videos", RO, NULL),
  KFD(PublicLibraries, "PublicLibraries", KF_COMMON, ROOT_NONE, P(Public), L"Libraries", RO, NULL),
  KFD(PublicGameTasks, "PublicGameTasks", KF_COMMON, ROOT_NONE, P(ProgramData), L"Microsoft\\Windows\\GameExplorer", 0, NULL),
  KFD(DeviceMetadataStore, "Device Metadata Store", KF_COMMON, ROOT_NONE, P(ProgramData), L"Microsoft\\Windows\\DeviceMetadataStore", 0, NULL),
  /* the user's folders */
  KFD(Profile, "Profile", KF_PERUSER, ROOT_PROFILE, NULL, NULL, RO, NULL),
  KFD(RoamingAppData, "AppData", KF_PERUSER, ROOT_NONE, P(Profile), L"AppData\\Roaming", 0, NULL),
  KFD(LocalAppData, "Local AppData", KF_PERUSER, ROOT_NONE, P(Profile), L"AppData\\Local", 0, NULL),
  KFD(LocalAppDataLow, "LocalAppDataLow", KF_PERUSER, ROOT_NONE, P(Profile), L"AppData\\LocalLow", 0, NULL),
  KFD(Desktop, "Desktop", KF_PERUSER, ROOT_NONE, P(Profile), L"Desktop", RO, NULL),
  KFD(Documents, "Personal", KF_PERUSER, ROOT_NONE, P(Profile), L"Documents", RO, NULL),
  KFD(Downloads, "{374DE290-123F-4565-9164-39C4925E467B}", KF_PERUSER, ROOT_NONE, P(Profile), L"Downloads", RO, NULL),
  KFD(Music, "My Music", KF_PERUSER, ROOT_NONE, P(Profile), L"Music", RO, NULL),
  KFD(Pictures, "My Pictures", KF_PERUSER, ROOT_NONE, P(Profile), L"Pictures", RO, NULL),
  KFD(Videos, "My Video", KF_PERUSER, ROOT_NONE, P(Profile), L"Videos", RO, NULL),
  KFD(Favorites, "Favorites", KF_PERUSER, ROOT_NONE, P(Profile), L"Favorites", RO, NULL),
  KFD(Links, "Links", KF_PERUSER, ROOT_NONE, P(Profile), L"Links", RO, NULL),
  KFD(Contacts, "Contacts", KF_PERUSER, ROOT_NONE, P(Profile), L"Contacts", RO, NULL),
  KFD(SavedGames, "SavedGames", KF_PERUSER, ROOT_NONE, P(Profile), L"Saved Games", RO, NULL),
  KFD(SavedSearches, "Searches", KF_PERUSER, ROOT_NONE, P(Profile), L"Searches", RO, NULL),
  KFD(Objects3D, "3D Objects", KF_PERUSER, ROOT_NONE, P(Profile), L"3D Objects", RO, NULL),
  KFD(Cookies, "Cookies", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\INetCookies", 0, NULL),
  KFD(History, "History", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\History", 0, NULL),
  KFD(InternetCache, "Cache", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\INetCache", 0, NULL),
  KFD(GameTasks, "GameTasks", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\GameExplorer", 0, NULL),
  KFD(CDBurning, "CD Burning", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\Burn\\Burn", 0, NULL),
  KFD(Recent, "Recent", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\Recent", 0, NULL),
  KFD(SendTo, "SendTo", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\SendTo", 0, NULL),
  KFD(StartMenu, "Start Menu", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\Start Menu", RO, NULL),
  KFD(Programs, "Programs", KF_PERUSER, ROOT_NONE, P(StartMenu), L"Programs", RO, NULL),
  KFD(Startup, "Startup", KF_PERUSER, ROOT_NONE, P(Programs), L"Startup", RO, NULL),
  KFD(AdminTools, "Administrative Tools", KF_PERUSER, ROOT_NONE, P(Programs), L"Administrative Tools", RO, NULL),
  KFD(Templates, "Templates", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\Templates", 0, NULL),
  KFD(NetHood, "NetHood", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\Network Shortcuts", 0, NULL),
  KFD(PrintHood, "PrintHood", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\Printer Shortcuts", 0, NULL),
  KFD(Libraries, "Libraries", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\Libraries", 0, NULL),
  KFD(QuickLaunch, "Quick Launch", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Internet Explorer\\Quick Launch", 0, NULL),
  KFD(UserPinned, "User Pinned", KF_PERUSER, ROOT_NONE, P(QuickLaunch), L"User Pinned", 0, NULL),
  KFD(ImplicitAppShortcuts, "ImplicitAppShortcuts", KF_PERUSER, ROOT_NONE, P(UserPinned), L"ImplicitAppShortcuts", 0, NULL),
  KFD(PhotoAlbums, "PhotoAlbums", KF_PERUSER, ROOT_NONE, P(Pictures), L"Slide Shows", 0, NULL),
  KFD(Screenshots, "Screenshots", KF_PERUSER, ROOT_NONE, P(Pictures), L"Screenshots", 0, NULL),
  KFD(CameraRoll, "Camera Roll", KF_PERUSER, ROOT_NONE, P(Pictures), L"Camera Roll", 0, NULL),
  KFD(SavedPictures, "Saved Pictures", KF_PERUSER, ROOT_NONE, P(Pictures), L"Saved Pictures", 0, NULL),
  KFD(Playlists, "Playlists", KF_PERUSER, ROOT_NONE, P(Music), L"Playlists", 0, NULL),
  KFD(AccountPictures, "AccountPictures", KF_PERUSER, ROOT_NONE, P(RoamingAppData), L"Microsoft\\Windows\\AccountPictures", 0, NULL),
  KFD(ApplicationShortcuts, "ApplicationShortcuts", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\Application Shortcuts", 0, NULL),
  KFD(RoamingTiles, "RoamingTiles", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Microsoft\\Windows\\RoamingTiles", 0, NULL),
  KFD(UserProgramFiles, "UserProgramFiles", KF_PERUSER, ROOT_NONE, P(LocalAppData), L"Programs", 0, NULL),
  KFD(UserProgramFilesCommon, "UserProgramFilesCommon", KF_PERUSER, ROOT_NONE, P(UserProgramFiles), L"Common", 0, NULL),
};
#undef KFD
#undef P
#undef RO

/* CSIDL -> known folder. The virtual and the user's ones have CSIDLs of their own; some folders have two. */
typedef struct { int csidl; const GUID *id; } CsidlEntry;
static const CsidlEntry g_csidls[] = {
  {CSIDL_DESKTOP, &FOLDERID_Desktop}, {CSIDL_INTERNET, &FOLDERID_InternetFolder}, {CSIDL_PROGRAMS, &FOLDERID_Programs}, {CSIDL_CONTROLS, &FOLDERID_ControlPanelFolder},
  {CSIDL_PRINTERS, &FOLDERID_PrintersFolder}, {CSIDL_PERSONAL, &FOLDERID_Documents}, {CSIDL_FAVORITES, &FOLDERID_Favorites}, {CSIDL_STARTUP, &FOLDERID_Startup},
  {CSIDL_RECENT, &FOLDERID_Recent}, {CSIDL_SENDTO, &FOLDERID_SendTo}, {CSIDL_BITBUCKET, &FOLDERID_RecycleBinFolder}, {CSIDL_STARTMENU, &FOLDERID_StartMenu},
  {0x000c, &FOLDERID_Documents}, {CSIDL_MYMUSIC, &FOLDERID_Music}, {CSIDL_MYVIDEO, &FOLDERID_Videos}, {CSIDL_DESKTOPDIRECTORY, &FOLDERID_Desktop},
  {CSIDL_DRIVES, &FOLDERID_ComputerFolder}, {CSIDL_NETWORK, &FOLDERID_NetworkFolder}, {CSIDL_NETHOOD, &FOLDERID_NetHood}, {CSIDL_FONTS, &FOLDERID_Fonts},
  {CSIDL_TEMPLATES, &FOLDERID_Templates}, {CSIDL_COMMON_STARTMENU, &FOLDERID_CommonStartMenu}, {CSIDL_COMMON_PROGRAMS, &FOLDERID_CommonPrograms},
  {CSIDL_COMMON_STARTUP, &FOLDERID_CommonStartup}, {CSIDL_COMMON_DESKTOPDIRECTORY, &FOLDERID_PublicDesktop}, {CSIDL_APPDATA, &FOLDERID_RoamingAppData},
  {CSIDL_PRINTHOOD, &FOLDERID_PrintHood}, {CSIDL_LOCAL_APPDATA, &FOLDERID_LocalAppData}, {CSIDL_INTERNET_CACHE, &FOLDERID_InternetCache}, {CSIDL_COOKIES, &FOLDERID_Cookies},
  {CSIDL_HISTORY, &FOLDERID_History}, {CSIDL_COMMON_APPDATA, &FOLDERID_ProgramData}, {CSIDL_WINDOWS, &FOLDERID_Windows}, {CSIDL_SYSTEM, &FOLDERID_System},
  {CSIDL_PROGRAM_FILES, &FOLDERID_ProgramFiles}, {CSIDL_MYPICTURES, &FOLDERID_Pictures}, {CSIDL_PROFILE, &FOLDERID_Profile}, {CSIDL_SYSTEMX86, &FOLDERID_SystemX86},
  {CSIDL_PROGRAM_FILESX86, &FOLDERID_ProgramFilesX86}, {CSIDL_PROGRAM_FILES_COMMON, &FOLDERID_ProgramFilesCommon}, {CSIDL_PROGRAM_FILES_COMMONX86, &FOLDERID_ProgramFilesCommonX86},
  {CSIDL_COMMON_TEMPLATES, &FOLDERID_CommonTemplates}, {CSIDL_COMMON_DOCUMENTS, &FOLDERID_PublicDocuments}, {CSIDL_COMMON_ADMINTOOLS, &FOLDERID_CommonAdminTools},
  {CSIDL_ADMINTOOLS, &FOLDERID_AdminTools}, {CSIDL_CONNECTIONS, &FOLDERID_ConnectionsFolder}, {CSIDL_COMMON_MUSIC, &FOLDERID_PublicMusic},
  {CSIDL_COMMON_PICTURES, &FOLDERID_PublicPictures}, {CSIDL_COMMON_VIDEO, &FOLDERID_PublicVideos}, {CSIDL_RESOURCES, &FOLDERID_ResourceDir},
  {CSIDL_RESOURCES_LOCALIZED, &FOLDERID_LocalizedResourcesDir}, {CSIDL_COMMON_OEM_LINKS, &FOLDERID_CommonOEMLinks}, {CSIDL_CDBURN_AREA, &FOLDERID_CDBurning},
  {CSIDL_PROFILES, &FOLDERID_UserProfiles},
};

static const KnownFolder *kf_find(const GUID *id) {
  for (size_t i = 0; i < ARRAY_LEN(g_known_folders); ++i) if (IsEqualGUID(g_known_folders[i].id, id)) return &g_known_folders[i];
  return NULL;
}
static const KnownFolder *kf_for_csidl(int csidl) {
  for (size_t i = 0; i < ARRAY_LEN(g_csidls); ++i) if (g_csidls[i].csidl == csidl) return kf_find(g_csidls[i].id);
  return NULL;
}
/* The CSIDL of a known folder, or -1. */
static int kf_csidl(const KnownFolder *kf) {
  for (size_t i = 0; i < ARRAY_LEN(g_csidls); ++i) if (g_csidls[i].id == kf->id) return g_csidls[i].csidl == 0x000c ? CSIDL_PERSONAL : g_csidls[i].csidl;
  return -1;
}

/* A path from the environment (the variable `name`) or, failing that, `fallback`. */
static void env_or(const WCHAR *name, const WCHAR *fallback, WCHAR *out, size_t capacity) {
  const DWORD n = GetEnvironmentVariableW(name, out, (DWORD)capacity);
  if (!n || n >= capacity) w_copy(out, capacity, fallback);
}

/* The path of a root folder. */
static BOOL root_path(FolderRoot root, WCHAR *out, size_t capacity) {
  switch (root) {
    case ROOT_PROFILE: env_or(L"USERPROFILE", L"C:\\Users\\Default", out, capacity); return TRUE;
    case ROOT_USERS: {
      env_or(L"USERPROFILE", L"C:\\Users\\Default", out, capacity);
      WCHAR *last = (WCHAR *)w_find_last(out, '\\');
      if (last && last != out + 2) *last = 0; else w_copy(out, capacity, L"C:\\Users");
      return TRUE;
    }
    case ROOT_PUBLIC: {
      root_path(ROOT_USERS, out, capacity);
      return w_cat(out, capacity, L"\\Public");
    }
    case ROOT_PROGRAMDATA: env_or(L"ProgramData", L"C:\\ProgramData", out, capacity); return TRUE;
    case ROOT_PROGRAMFILES: env_or(L"ProgramW6432", L"C:\\Program Files", out, capacity); return TRUE;
    case ROOT_PROGRAMFILESX86: env_or(L"ProgramFiles(x86)", L"C:\\Program Files (x86)", out, capacity); return TRUE;
    case ROOT_COMMONFILES: env_or(L"CommonProgramW6432", L"C:\\Program Files\\Common Files", out, capacity); return TRUE;
    case ROOT_COMMONFILESX86: env_or(L"CommonProgramFiles(x86)", L"C:\\Program Files (x86)\\Common Files", out, capacity); return TRUE;
    case ROOT_WINDOWS: { const UINT n = GetWindowsDirectoryW(out, (UINT)capacity); if (!n || n >= capacity) w_copy(out, capacity, L"C:\\Windows"); return TRUE; }
    case ROOT_SYSTEM: { const UINT n = GetSystemDirectoryW(out, (UINT)capacity); if (!n || n >= capacity) w_copy(out, capacity, L"C:\\Windows\\System32"); return TRUE; }
    case ROOT_SYSTEMX86: {
      const UINT n = GetSystemWow64DirectoryW(out, (UINT)capacity);
      if (!n || n >= capacity) w_copy(out, capacity, L"C:\\Windows\\SysWOW64");
      return TRUE;
    }
    default: return FALSE;
  }
}

#define SHELL_FOLDERS_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders"
#define SHELL_FOLDERS_CACHE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders"

/* The path of a known folder; `current` takes the registry's redirection into account, otherwise the default is returned. FALSE for a virtual folder. */
static BOOL kf_path(const KnownFolder *kf, BOOL current, WCHAR *out, size_t capacity, int depth) {
  if (kf->kind & KF_VIRTUAL) return FALSE;
  if (depth > 8) return FALSE;
  if (current && (kf->kind & KF_PERUSER) && kf->name[0] && kf->id != &FOLDERID_Profile) {
    if (reg_get_string(HKEY_CURRENT_USER, SHELL_FOLDERS_KEY, kf->name, out, (DWORD)capacity) && out[0]) {
      path_strip_trailing_sep(out);
      return TRUE;
    }
  }
  if (kf->root != ROOT_NONE) return root_path(kf->root, out, capacity);
  if (!kf->parent) return FALSE;
  const KnownFolder *parent = kf_find(kf->parent);
  if (!parent || !kf_path(parent, current, out, capacity, depth + 1)) return FALSE;
  if (!path_add_sep(out, capacity)) return FALSE;
  return w_cat(out, capacity, kf->relative);
}

/* Makes a directory and the ones above it. */
static BOOL make_directories(const WCHAR *path) {
  WCHAR p[MAX_PATH_EX];
  if (!w_copy(p, ARRAY_LEN(p), path)) return FALSE;
  path_strip_trailing_sep(p);
  if (path_is_dir(p)) return TRUE;
  const size_t root = path_root_length(p);
  for (size_t i = root; p[i]; ++i) {
    if (!is_sep(p[i])) continue;
    p[i] = 0;
    if (i > root && !path_is_dir(p) && !CreateDirectoryW(p, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { p[i] = '\\'; return FALSE; }
    p[i] = '\\';
  }
  if (!CreateDirectoryW(p, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
  return TRUE;
}

/* A token for the folders: none (this user), -1 (the default user) or a handle that exists. */
static BOOL token_valid(HANDLE token) {
  DWORD flags;
  return !token || token == (HANDLE)(LONG_PTR)-1 || GetHandleInformation(token, &flags);
}

/* The common core: the path of a known folder with the KF_FLAG_* bits. */
static HRESULT kf_resolve(const KnownFolder *kf, DWORD flags, WCHAR *out, size_t capacity) {
  out[0] = 0;
  if (kf->kind & KF_VIRTUAL) return E_FAIL;
  if (!kf_path(kf, !(flags & KF_FLAG_DEFAULT_PATH), out, capacity, 0)) { out[0] = 0; return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); }
  if (flags & KF_FLAG_DONT_VERIFY) return S_OK;
  if (flags & KF_FLAG_CREATE) {
    if (!path_is_dir(out) && !make_directories(out)) { const HRESULT hr = hr_last_error(); out[0] = 0; return hr; }
    if (kf->attributes && (kf->kind & KF_PERUSER) == 0) {}
    return S_OK;
  }
  if (!path_is_dir(out)) {
    /* the user's and the common folders are made when the profile is: make them when first asked for (the fixed ones, Windows ... are not ours to make) */
    if ((kf->kind & (KF_PERUSER | KF_COMMON)) && make_directories(out)) return S_OK;
    out[0] = 0;
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  return S_OK;
}

EXPORT HRESULT WINAPI SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR *path) {
  if (!path) return E_INVALIDARG;
  *path = NULL;
  if (!id) return E_INVALIDARG;
  if (!token_valid(token)) return E_FAIL;
  if ((flags & KF_FLAG_NOT_PARENT_RELATIVE) && !(flags & KF_FLAG_DEFAULT_PATH)) return E_INVALIDARG;
  const KnownFolder *kf = kf_find(id);
  if (!kf) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  WCHAR buffer[MAX_PATH_EX];
  const HRESULT hr = kf_resolve(kf, flags, buffer, ARRAY_LEN(buffer));
  if (FAILED(hr)) return hr;
  *path = w_task_dup(buffer);
  return *path ? S_OK : E_OUTOFMEMORY;
}
EXPORT HRESULT WINAPI SHGetKnownFolderPathWorker(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR *path) { return SHGetKnownFolderPath(id, flags, token, path); }

EXPORT HRESULT WINAPI SHGetFolderPathEx(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, LPWSTR buffer, DWORD capacity) {
  PWSTR path = NULL;
  if (!buffer || !capacity) return E_INVALIDARG;
  const HRESULT hr = SHGetKnownFolderPath(id, flags, token, &path);
  if (FAILED(hr)) return hr;
  const BOOL fits = w_copy(buffer, capacity, path);
  SHFree(path);
  return fits ? S_OK : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

/* Redirects a per-user folder (the registry value in User Shell Folders); with KF_FLAG_DEFAULT_PATH-less use the folder is made too. */
EXPORT HRESULT WINAPI SHSetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PCWSTR path) {
  (void)flags; (void)token;
  const KnownFolder *kf = id ? kf_find(id) : NULL;
  if (!kf) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  if (!path || !*path) return E_INVALIDARG;
  if ((kf->kind & KF_VIRTUAL) || !(kf->kind & KF_PERUSER) || !kf->name[0] || kf->id == &FOLDERID_Profile) return E_ACCESSDENIED;
  if (!path_is_dir(path) && !make_directories(path)) return hr_last_error();
  if (!reg_set_string(HKEY_CURRENT_USER, SHELL_FOLDERS_KEY, kf->name, path, REG_EXPAND_SZ)) return E_FAIL;
  reg_set_string(HKEY_CURRENT_USER, SHELL_FOLDERS_CACHE_KEY, kf->name, path, REG_SZ);
  return S_OK;
}
EXPORT HRESULT WINAPI SHSetKnownFolderPathWorker(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PCWSTR path) { return SHSetKnownFolderPath(id, flags, token, path); }

/* ---- the CSIDL functions ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT csidl_path(int csidl, DWORD type, WCHAR *out, size_t capacity) {
  out[0] = 0;
  const int folder = csidl & 0xFF;
  const DWORD csidl_flags = (DWORD)csidl & 0xFF00u;
  const KnownFolder *kf = kf_for_csidl(folder);
  if (!kf) return folder == CSIDL_ALTSTARTUP || folder == CSIDL_COMMON_ALTSTARTUP || folder == CSIDL_COMMON_FAVORITES ? S_FALSE : E_INVALIDARG;
  if (kf->kind & KF_VIRTUAL) return E_INVALIDARG;
  DWORD flags = 0;
  if (csidl_flags & CSIDL_FLAG_CREATE) flags |= KF_FLAG_CREATE;
  if (csidl_flags & CSIDL_FLAG_DONT_VERIFY) flags |= KF_FLAG_DONT_VERIFY;
  if (type == SHGFP_TYPE_DEFAULT) flags |= KF_FLAG_DEFAULT_PATH;
  return kf_resolve(kf, flags, out, capacity);
}
EXPORT HRESULT WINAPI SHGetFolderPathW(HWND owner, int csidl, HANDLE token, DWORD type, LPWSTR path) {
  (void)owner;
  if (!path) return E_INVALIDARG;
  if (!token_valid(token)) { path[0] = 0; return E_FAIL; }
  return csidl_path(csidl, type, path, MAX_PATH);
}
EXPORT HRESULT WINAPI SHGetFolderPathWWorker(HWND owner, int csidl, HANDLE token, DWORD type, LPWSTR path) { return SHGetFolderPathW(owner, csidl, token, type, path); }
EXPORT HRESULT WINAPI SHGetFolderPathA(HWND owner, int csidl, HANDLE token, DWORD type, LPSTR path) {
  WCHAR w[MAX_PATH];
  if (!path) return E_INVALIDARG;
  path[0] = 0;
  const HRESULT hr = SHGetFolderPathW(owner, csidl, token, type, w);
  if (hr == S_OK && !w_to_a(w, path, MAX_PATH)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  return hr;
}
EXPORT HRESULT WINAPI SHGetFolderPathAWorker(HWND owner, int csidl, HANDLE token, DWORD type, LPSTR path) { return SHGetFolderPathA(owner, csidl, token, type, path); }

EXPORT HRESULT WINAPI SHGetFolderPathAndSubDirW(HWND owner, int csidl, HANDLE token, DWORD type, LPCWSTR sub, LPWSTR path) {
  if (!path) return E_INVALIDARG;
  const HRESULT hr = SHGetFolderPathW(owner, csidl, token, type, path);
  if (FAILED(hr) || hr == S_FALSE) return hr;
  if (sub && *sub) {
    if (!path_add_sep(path, MAX_PATH)) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    if (!w_cat(path, MAX_PATH, sub[0] == '\\' ? sub + 1 : sub)) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    if ((csidl & CSIDL_FLAG_CREATE) && !path_is_dir(path) && !make_directories(path)) return hr_last_error();
  }
  return S_OK;
}
EXPORT HRESULT WINAPI SHGetFolderPathAndSubDirWWorker(HWND owner, int csidl, HANDLE token, DWORD type, LPCWSTR sub, LPWSTR path) {
  return SHGetFolderPathAndSubDirW(owner, csidl, token, type, sub, path);
}
EXPORT HRESULT WINAPI SHGetFolderPathAndSubDirA(HWND owner, int csidl, HANDLE token, DWORD type, LPCSTR sub, LPSTR path) {
  WCHAR w[MAX_PATH];
  WCHAR *wsub = a_to_w(sub);
  if (!path) { mem_free(wsub); return E_INVALIDARG; }
  const HRESULT hr = SHGetFolderPathAndSubDirW(owner, csidl, token, type, wsub, w);
  mem_free(wsub);
  if (hr == S_OK && !w_to_a(w, path, MAX_PATH)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  if (hr != S_OK) path[0] = 0;
  return hr;
}

EXPORT BOOL WINAPI SHGetSpecialFolderPathW(HWND owner, LPWSTR path, int csidl, BOOL create) {
  if (!path) return FALSE;
  return SHGetFolderPathW(owner, create ? csidl | CSIDL_FLAG_CREATE : csidl, NULL, SHGFP_TYPE_CURRENT, path) == S_OK;
}
EXPORT BOOL WINAPI SHGetSpecialFolderPathWWorker(HWND owner, LPWSTR path, int csidl, BOOL create) { return SHGetSpecialFolderPathW(owner, path, csidl, create); }
EXPORT BOOL WINAPI SHGetSpecialFolderPathA(HWND owner, LPSTR path, int csidl, BOOL create) {
  if (!path) return FALSE;
  return SHGetFolderPathA(owner, create ? csidl | CSIDL_FLAG_CREATE : csidl, NULL, SHGFP_TYPE_CURRENT, path) == S_OK;
}
EXPORT BOOL WINAPI SHGetSpecialFolderPathAWorker(HWND owner, LPSTR path, int csidl, BOOL create) { return SHGetSpecialFolderPathA(owner, path, csidl, create); }

/* Redirects a folder given by CSIDL. */
EXPORT HRESULT WINAPI SHSetFolderPathW(int csidl, HANDLE token, DWORD flags, LPCWSTR path) {
  (void)flags;
  const KnownFolder *kf = kf_for_csidl(csidl & 0xFF);
  if (!kf) return E_INVALIDARG;
  return SHSetKnownFolderPath(kf->id, 0, token, path);
}
EXPORT HRESULT WINAPI SHSetFolderPathA(int csidl, HANDLE token, DWORD flags, LPCSTR path) {
  WCHAR *w = a_to_w(path);
  const HRESULT hr = w ? SHSetFolderPathW(csidl, token, flags, w) : E_INVALIDARG;
  mem_free(w);
  return hr;
}
