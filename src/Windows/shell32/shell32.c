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

/* shell32.dll for TheoryCraftOS. Built freestanding over kernel32.dll (and advapi32.dll's registry, user32.dll) with the OS's mingw compiler, against mingw-w64's headers
 * (the COM interfaces of the shell are declared there). The parts, one file each:
 *   util.inc     memory, string and path helpers shared by the others; IMalloc; shared memory (SHAlloc, SHLockShared ...)
 *   cmdline.inc  CommandLineToArgvW, DoEnvironmentSubst, CheckEscapes, the She* functions of Windows 3.x, drive and path helpers
 *   folders.inc  the special folders: CSIDL values and known-folder ids, SHGetFolderPath, SHGetKnownFolderPath, SHSetKnownFolderPath ...
 *   pidl.inc     item id lists (IL*), SHParseDisplayName, SHGetPathFromIDList, IShellFolder for the desktop, My Computer and file system folders
 *   items.inc    IShellItem, IShellItemArray, IDataObject for files, SHCreateItem*
 *   fileop.inc   SHFileOperation, SHCreateDirectory*, the recycle bin, unique names, PathCleanupSpec ...
 *   exec.inc     ShellExecute[Ex], FindExecutable, file associations in the registry, the Rundll entry points
 *   info.inc     SHGetFileInfo, icons, drag and drop (HDROP), change notifications, notification icons, dialogs and the small rest
 *   link.inc     shortcuts (IShellLink, IPersistFile), the class factories of DllGetClassObject
 * The behaviour follows Windows (checked with Wine's tests, tests/windows/wine/shell32). The documented limits are in docs/WindowsUserMode.md. */
#define _SHELL32_
#define _SHFOLDER_
#define WINSHELLAPI
#define COBJMACROS
#define INITGUID
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000000
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <ntsecapi.h>
#include <sddl.h>

/* The generic names (SHGetFileInfo, ShellExecuteEx ...) are macros for the A or W version in the headers; this DLL exports them as the wide ones. */
#undef ILCreateFromPath
#undef SHGetPathFromIDList
#undef SHGetFileInfo
#undef SHFileOperation
#undef ShellExecuteEx
#undef ShellExecute
#undef FindExecutable
#undef ExtractIconEx
#undef ExtractIcon
#undef ExtractAssociatedIcon
#undef ExtractAssociatedIconEx
#undef DragQueryFile
#undef SHBrowseForFolder
#undef Shell_NotifyIcon
#undef SHGetNewLinkInfo
#undef SHCreateDirectoryEx
#undef SHEmptyRecycleBin
#undef SHQueryRecycleBin
#undef SHGetSpecialFolderPath
#undef SHGetFolderPath
#undef SHGetFolderPathAndSubDir
#undef ShellAbout
#undef ShellMessageBox
#undef SHSetFolderPath
#undef SHGetDiskFreeSpaceEx
#undef IsLFNDrive
#undef SHInvokePrinterCommand
#undef Shell_GetCachedImageIndex
#undef SHDefExtractIcon
#undef SHUpdateImage
#undef PathCleanupSpec
#undef PathIsExe
#undef SHSimpleIDListFromPath
#undef SHGetDataFromIDList
#undef InternalExtractIconList
#undef ExtractIconResInfo
#undef SHGetIconOverlayIndex
#undef SheChangeDir
#undef SheChangeDirEx
#undef SheGetDir
#undef SheFullPath
#undef SheShortenPath
#undef SheRemoveQuotes
#undef DoEnvironmentSubst
#undef CheckEscapes
#undef SHCreateDirectoryExA

#ifndef CSIDL_PROFILES
#define CSIDL_PROFILES 0x003e
#endif
typedef struct _DllVersionInfo { DWORD cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID; } DLLVERSIONINFO;
typedef struct _DllVersionInfo2 { DLLVERSIONINFO info1; DWORD dwFlags; ULONGLONG ullVersion; } DLLVERSIONINFO2;
#define DLLVER_PLATFORM_NT 2
#define MAKEDLLVERULL(major, minor, build, qfe) (((ULONGLONG)(major) << 48) | ((ULONGLONG)(minor) << 32) | ((ULONGLONG)(build) << 16) | (ULONGLONG)(qfe))
typedef struct { UINT uNumberOfMappings; LPSHNAMEMAPPINGW lpSM; } HANDLETOMAPPINGS;
#define PPCF_ADDQUOTES 0x01
#define PPCF_ADDARGUMENTS 0x03
#define PPCF_NODIRECTORIES 0x10
#define PPCF_FORCEQUALIFY 0x40
#define PPCF_LONGESTPOSSIBLE 0x80
static const GUID kGuidNull = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

/* the headers declare the entries of other libraries this one defines (or forwards) as imports: harmless here */
__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define EXPORT __declspec(dllexport)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_PATH_EX 1024      /* the buffers for paths that may be longer than MAX_PATH */

static HINSTANCE g_module;
static CRITICAL_SECTION g_lock;      /* guards every table of this file (change-notification listeners, shared blocks, the folder cache) */

#include "util.inc"
#include "cmdline.inc"
#include "folders.inc"
#include "pidl.inc"
#include "items.inc"
#include "fileop.inc"
#include "exec.inc"
#include "info.inc"
#include "link.inc"
#include "kfm.inc"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    InitializeCriticalSection(&g_lock);
  }
  return TRUE;
}
