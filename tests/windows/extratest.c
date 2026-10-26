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

/* More of kernel32 on TheoryCraftOS: fibers, the thread pool and timer queues, registered waits, volumes, search paths, file information, copying, initialization files,
 * resources and small things (mingw gcc; see tests/windows/build.sh). Prints "extratest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

/* ---- fibers ---- */
static LPVOID g_main_fiber, g_fiber_a, g_fiber_b;
static char g_trace[64];
static int g_trace_length;
static void note(char c) { g_trace[g_trace_length++] = c; g_trace[g_trace_length] = 0; }
static VOID CALLBACK fiber_a(LPVOID parameter) {
  note('a');
  check(GetFiberData() == parameter && (INT_PTR)parameter == 11, "GetFiberData in a fiber");
  SwitchToFiber(g_fiber_b);
  note('c');
  double x = 1.5;      /* (floating point registers survive a switch) */
  SwitchToFiber(g_fiber_b);
  note(x == 1.5 ? 'e' : 'X');
  SwitchToFiber(g_main_fiber);
}
static VOID CALLBACK fiber_b(LPVOID parameter) {
  (void)parameter;
  double y = 2.5;
  note('b');
  SwitchToFiber(g_fiber_a);
  note(y == 2.5 ? 'd' : 'X');
  SwitchToFiber(g_fiber_a);
}
static void test_fibers(void) {
  check(!IsThreadAFiber(), "a thread is not a fiber at first");
  g_main_fiber = ConvertThreadToFiber((LPVOID)7);
  check(g_main_fiber != NULL && IsThreadAFiber() && GetFiberData() == (LPVOID)7, "ConvertThreadToFiber");
  g_fiber_a = CreateFiber(0, fiber_a, (LPVOID)11);
  g_fiber_b = CreateFiber(64 * 1024, fiber_b, (LPVOID)22);
  check(g_fiber_a && g_fiber_b, "CreateFiber");
  note('m');
  SwitchToFiber(g_fiber_a);
  note('n');
  check(!strcmp(g_trace, "mabcdXn") || !strcmp(g_trace, "mabcden") || !strcmp(g_trace, "mabcdn"), "the fibers ran in the order they switched");
  check(!strcmp(g_trace, "mabcden"), "with their stacks and registers intact (trace mabcden)");
  DeleteFiber(g_fiber_a);
  DeleteFiber(g_fiber_b);
  check(ConvertFiberToThread() && !IsThreadAFiber(), "ConvertFiberToThread");
}

/* ---- the thread pool ---- */
static volatile LONG g_work_count, g_simple_count, g_timer_count, g_wait_count, g_wait_timeouts;
static VOID CALLBACK work_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work) { (void)instance; (void)context; (void)work; InterlockedIncrement(&g_work_count); }
static VOID CALLBACK simple_callback(PTP_CALLBACK_INSTANCE instance, PVOID context) { (void)instance; InterlockedAdd(&g_simple_count, (LONG)(INT_PTR)context); }
static DWORD WINAPI user_item(PVOID context) { InterlockedAdd(&g_simple_count, (LONG)(INT_PTR)context); return 0; }
static VOID CALLBACK timer_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer) { (void)instance; (void)context; (void)timer; InterlockedIncrement(&g_timer_count); }
static VOID CALLBACK queue_timer_callback(PVOID context, BOOLEAN fired) { (void)context; (void)fired; InterlockedIncrement(&g_timer_count); }
static VOID CALLBACK wait_callback(PVOID context, BOOLEAN timed_out) { (void)context; if (timed_out) InterlockedIncrement(&g_wait_timeouts); else InterlockedIncrement(&g_wait_count); }
static volatile LONG g_pool_wait;
static VOID CALLBACK pool_wait_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait, TP_WAIT_RESULT result) { (void)instance; (void)context; (void)wait; g_pool_wait = (LONG)result + 1; }

static void test_pool(void) {
  PTP_WORK work = CreateThreadpoolWork(work_callback, NULL, NULL);
  check(work != NULL, "CreateThreadpoolWork");
  for (int i = 0; i < 100; ++i) SubmitThreadpoolWork(work);
  WaitForThreadpoolWorkCallbacks(work, FALSE);
  check(g_work_count == 100, "100 work callbacks ran on pool threads");
  CloseThreadpoolWork(work);

  check(TrySubmitThreadpoolCallback(simple_callback, (PVOID)5, NULL), "TrySubmitThreadpoolCallback");
  check(QueueUserWorkItem(user_item, (PVOID)7, 0), "QueueUserWorkItem");
  for (int i = 0; i < 400 && g_simple_count != 12; ++i) Sleep(5);
  check(g_simple_count == 12, "both ran");

  /* a pool timer */
  PTP_TIMER timer = CreateThreadpoolTimer(timer_callback, NULL, NULL);
  FILETIME due;
  ULARGE_INTEGER u;
  u.QuadPart = (ULONGLONG)(-300000);      /* 30 ms from now */
  due.dwLowDateTime = u.LowPart;
  due.dwHighDateTime = u.HighPart;
  g_timer_count = 0;
  SetThreadpoolTimer(timer, &due, 40, 0);
  for (int i = 0; i < 400 && g_timer_count < 3; ++i) Sleep(5);
  check(g_timer_count >= 3, "a periodic pool timer fires again and again");
  SetThreadpoolTimer(timer, NULL, 0, 0);
  WaitForThreadpoolTimerCallbacks(timer, TRUE);
  const LONG stopped_at = g_timer_count;
  Sleep(150);
  check(g_timer_count == stopped_at, "and stops when it is cleared");
  CloseThreadpoolTimer(timer);

  /* a timer queue */
  HANDLE queue = CreateTimerQueue();
  HANDLE queue_timer = NULL;
  g_timer_count = 0;
  check(CreateTimerQueueTimer(&queue_timer, queue, queue_timer_callback, NULL, 20, 0, WT_EXECUTEDEFAULT), "CreateTimerQueueTimer");
  for (int i = 0; i < 400 && g_timer_count < 1; ++i) Sleep(5);
  Sleep(100);
  check(g_timer_count == 1, "a one-shot timer fires once");
  check(DeleteTimerQueueEx(queue, INVALID_HANDLE_VALUE), "DeleteTimerQueueEx");

  /* registered waits */
  HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
  HANDLE registered = NULL;
  g_wait_count = g_wait_timeouts = 0;
  check(RegisterWaitForSingleObject(&registered, event, wait_callback, NULL, 100, WT_EXECUTEDEFAULT), "RegisterWaitForSingleObject");
  SetEvent(event);
  for (int i = 0; i < 400 && g_wait_count < 1; ++i) Sleep(5);
  check(g_wait_count >= 1, "its callback runs when the handle is signalled");
  for (int i = 0; i < 400 && g_wait_timeouts < 2; ++i) Sleep(5);
  check(g_wait_timeouts >= 2, "and with timed_out set when the time passes (the wait goes on)");
  check(UnregisterWaitEx(registered, INVALID_HANDLE_VALUE), "UnregisterWaitEx");
  const LONG after = g_wait_count + g_wait_timeouts;
  Sleep(250);
  check(g_wait_count + g_wait_timeouts == after, "nothing runs after that");

  PTP_WAIT pool_wait = CreateThreadpoolWait(pool_wait_callback, NULL, NULL);
  g_pool_wait = 0;
  SetThreadpoolWait(pool_wait, event, NULL);
  SetEvent(event);
  for (int i = 0; i < 400 && !g_pool_wait; ++i) Sleep(5);
  check(g_pool_wait == (LONG)WAIT_OBJECT_0 + 1, "a pool wait fires for a signalled handle");
  CloseThreadpoolWait(pool_wait);
  CloseHandle(event);
}

/* ---- small things ---- */
static void test_small(void) {
  check(MulDiv(10, 3, 4) == 8 && MulDiv(-10, 3, 4) == -8 && MulDiv(1, 1, 0) == -1 && MulDiv(2000000000, 2, 1) == -1, "MulDiv rounds, and reports overflow and a zero divisor");
  FILETIME a = {1, 2}, b = {1, 3};
  check(CompareFileTime(&a, &b) == -1 && CompareFileTime(&b, &a) == 1 && CompareFileTime(&a, &a) == 0, "CompareFileTime");
  FILETIME ft;
  WORD date, time;
  check(DosDateTimeToFileTime((WORD)(((2024 - 1980) << 9) | (7 << 5) | 15), (WORD)((13 << 11) | (45 << 5) | 15), &ft) && FileTimeToDosDateTime(&ft, &date, &time) && (date >> 9) + 1980 == 2024 && ((time >> 11) == 13), "DOS date and time to FILETIME and back");
  void *p = (void *)0x123456789ABCull;
  check(DecodePointer(EncodePointer(p)) == p && EncodePointer(p) != p, "EncodePointer and DecodePointer");
  char buffer[16];
  check(!IsBadReadPtr(buffer, sizeof(buffer)) && !IsBadWritePtr(buffer, sizeof(buffer)) && IsBadReadPtr((void *)8, 4), "IsBadReadPtr and IsBadWritePtr");
  check(IsBadReadPtr(NULL, 1) && IsBadReadPtr((void *)0x10, 4) && !IsBadReadPtr(buffer, 0), "IsBadReadPtr on a null and an unmapped pointer");
  SetLastError(0);
  DWORD code = 0;
  check(!DeviceIoControl(GetStdHandle(STD_OUTPUT_HANDLE), 0x12345, NULL, 0, NULL, 0, &code, NULL) && GetLastError() == ERROR_INVALID_FUNCTION, "DeviceIoControl: ERROR_INVALID_FUNCTION");
  SetLastError(0);
  check(!CreateHardLinkA("C:\\Temp\\hl", "C:\\Temp\\hl2", NULL) && GetLastError() == ERROR_FILE_NOT_FOUND, "a hard link to a file that does not exist");
  {
    HANDLE hlf = CreateFileA("C:\\Temp\\hl2", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD put = 0;
    if (hlf != INVALID_HANDLE_VALUE) { WriteFile(hlf, "x", 1, &put, NULL); CloseHandle(hlf); }
    check(CreateHardLinkA("C:\\Temp\\hl", "C:\\Temp\\hl2", NULL) && DeleteFileA("C:\\Temp\\hl2") && GetFileAttributesA("C:\\Temp\\hl") != INVALID_FILE_ATTRIBUTES,
          "a hard link survives the name it was made from (see linktest.c for the rest)");
    DeleteFileA("C:\\Temp\\hl");
  }
  check(GetActiveProcessorCount(0) >= 2 && GetActiveProcessorGroupCount() == 1, "GetActiveProcessorCount");
  char name[32];
  DWORD size = sizeof(name);
  check(GetComputerNameExA(ComputerNameDnsHostname, name, &size) && size > 0, "GetComputerNameEx");
}

/* ---- volumes, search, file information, copying ---- */
static void test_files(void) {
  ULARGE_INTEGER available, total, free_bytes;
  check(GetDiskFreeSpaceExA("C:\\", &available, &total, &free_bytes) && total.QuadPart >= (1ull << 20) && available.QuadPart <= total.QuadPart && free_bytes.QuadPart >= available.QuadPart, "GetDiskFreeSpaceEx");
  DWORD sectors, bytes, free_clusters, total_clusters;
  check(GetDiskFreeSpaceA("C:\\", &sectors, &bytes, &free_clusters, &total_clusters) && sectors * bytes >= 512 && free_clusters <= total_clusters, "GetDiskFreeSpace");
  char label[64], fs_name[32];
  DWORD serial, max_component, flags;
  check(GetVolumeInformationA("C:\\", label, sizeof(label), &serial, &max_component, &flags, fs_name, sizeof(fs_name)) && max_component == 255 && fs_name[0] != 0, "GetVolumeInformation");
  char root[8];
  check(GetVolumePathNameA("C:\\Windows\\System32\\kernel32.dll", root, sizeof(root)) && !strcmp(root, "C:\\"), "GetVolumePathName");

  char found[MAX_PATH], *file_part = NULL;
  check(SearchPathA(NULL, "kernel32.dll", NULL, sizeof(found), found, &file_part) > 10 && file_part && !_stricmp(file_part, "kernel32.dll"), "SearchPath finds kernel32.dll");
  check(SearchPathA("C:\\Windows\\System32", "kernel32", ".dll", sizeof(found), found, NULL) > 10, "SearchPath in a folder, with an extension added");
  SetLastError(0);
  check(SearchPathA("C:\\Windows", "kernel32.dll", NULL, sizeof(found), found, NULL) == 0 && GetLastError() == ERROR_FILE_NOT_FOUND, "SearchPath does not find it where it is not");

  CreateDirectoryA("C:\\Temp", NULL);
  HANDLE f = CreateFileA("C:\\Temp\\ex.txt", GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD n = 0;
  WriteFile(f, "0123456789", 10, &n, NULL);
  FILE_STANDARD_INFO standard;
  check(GetFileInformationByHandleEx(f, FileStandardInfo, &standard, sizeof(standard)) && standard.EndOfFile.QuadPart == 10, "GetFileInformationByHandleEx(FileStandardInfo)");
  FILE_END_OF_FILE_INFO eof;
  eof.EndOfFile.QuadPart = 4;
  check(SetFileInformationByHandle(f, FileEndOfFileInfo, &eof, sizeof(eof)) && GetFileSize(f, NULL) == 4, "SetFileInformationByHandle(FileEndOfFileInfo)");
  CloseHandle(f);

  check(CopyFileExA("C:\\Temp\\ex.txt", "C:\\Temp\\ex2.txt", NULL, NULL, NULL, 0) && GetFileAttributesA("C:\\Temp\\ex2.txt") != INVALID_FILE_ATTRIBUTES, "CopyFileEx");
  check(ReplaceFileA("C:\\Temp\\ex.txt", "C:\\Temp\\ex2.txt", "C:\\Temp\\ex.bak", 0, NULL, NULL) && GetFileAttributesA("C:\\Temp\\ex.bak") != INVALID_FILE_ATTRIBUTES && GetFileAttributesA("C:\\Temp\\ex2.txt") == INVALID_FILE_ATTRIBUTES, "ReplaceFile (with a backup)");
  check(MoveFileWithProgressA("C:\\Temp\\ex.bak", "C:\\Temp\\ex3.txt", NULL, NULL, 0), "MoveFileWithProgress");
  DeleteFileA("C:\\Temp\\ex.txt");
  DeleteFileA("C:\\Temp\\ex3.txt");
}

/* ---- initialization files ---- */
static void test_ini(void) {
  const char *path = "C:\\Temp\\test.ini";
  DeleteFileA(path);
  check(WritePrivateProfileStringA("Window", "Width", "800", path) && WritePrivateProfileStringA("Window", "Title", "My Program", path) && WritePrivateProfileStringA("Colors", "Back", "255", path), "WritePrivateProfileString makes the file");
  char value[64];
  check(GetPrivateProfileStringA("Window", "Title", "none", value, sizeof(value), path) == 10 && !strcmp(value, "My Program"), "GetPrivateProfileString");
  check(GetPrivateProfileIntA("Window", "Width", 0, path) == 800 && GetPrivateProfileIntA("Window", "Height", 600, path) == 600, "GetPrivateProfileInt, with a default");
  check(GetPrivateProfileStringA("window", "TITLE", "none", value, sizeof(value), path) == 10, "names do not depend on case");
  GetPrivateProfileStringA("Window", "Missing", "fallback", value, sizeof(value), path);
  check(!strcmp(value, "fallback"), "the default for a missing key");
  WritePrivateProfileStringA("Window", "Width", "1024", path);
  check(GetPrivateProfileIntA("Window", "Width", 0, path) == 1024, "a key is changed in place");
  char names[128];
  DWORD n = GetPrivateProfileSectionNamesA(names, sizeof(names), path);
  check(n == strlen("Window") + 1 + strlen("Colors") + 1 && !strcmp(names, "Window") && !strcmp(names + 7, "Colors"), "GetPrivateProfileSectionNames");
  n = GetPrivateProfileStringA("Window", NULL, NULL, names, sizeof(names), path);
  check(n == strlen("Width") + 1 + strlen("Title") + 1 && !strcmp(names, "Width") && !strcmp(names + 6, "Title"), "the keys of a section");
  WritePrivateProfileStringA("Window", "Title", NULL, path);
  GetPrivateProfileStringA("Window", "Title", "gone", value, sizeof(value), path);
  check(!strcmp(value, "gone"), "a key is removed with a NULL value");
  WritePrivateProfileStringA("Colors", NULL, NULL, path);
  GetPrivateProfileStringA("Colors", "Back", "gone", value, sizeof(value), path);
  check(!strcmp(value, "gone"), "a section is removed with a NULL key");
  WCHAR wide[64];
  check(GetPrivateProfileIntW(L"Window", L"Width", 0, L"C:\\Temp\\test.ini") == 1024 && GetPrivateProfileStringW(L"Window", L"Width", L"", wide, 64, L"C:\\Temp\\test.ini") == 4 && !wcscmp(wide, L"1024"), "the wide versions");
  DeleteFileA(path);
}

int main(void) {
  test_fibers();
  test_pool();
  test_small();
  test_files();
  test_ini();
  printf("extratest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
