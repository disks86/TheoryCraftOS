/* Prototypes of the kernel32.dll functions msvcrt.dll imports (the definitions are in kernel32/kernel32.c). */
#pragma once
#include "tcos_win.h"

IMPORT BOOL WINAPI CloseHandle(HANDLE h);
IMPORT BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
IMPORT HANDLE WINAPI CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file);
IMPORT HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD id);
IMPORT BOOL WINAPI DeleteFileA(LPCSTR path);
IMPORT BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE source, HANDLE tp, HANDLE *target, DWORD access, BOOL inherit, DWORD options);
IMPORT void WINAPI ExitProcess(UINT code);
IMPORT void WINAPI ExitThread(DWORD code);
IMPORT BOOL WINAPI FlushFileBuffers(HANDLE h);
IMPORT LPSTR WINAPI GetCommandLineA(void);
IMPORT LPWSTR WINAPI GetCommandLineW(void);
IMPORT DWORD WINAPI GetCurrentDirectoryA(DWORD capacity, LPSTR buffer);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT LPWSTR WINAPI GetEnvironmentStringsW(void);
IMPORT DWORD WINAPI GetFileAttributesA(LPCSTR path);
IMPORT BOOL WINAPI GetFileAttributesExW(LPCWSTR path, int level, LPVOID data);
IMPORT BOOL WINAPI GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
IMPORT BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
IMPORT DWORD WINAPI GetFileType(HANDLE h);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT HANDLE WINAPI GetProcessHeap(void);
IMPORT HANDLE WINAPI GetStdHandle(DWORD which);
IMPORT void WINAPI GetSystemTimeAsFileTime(LPFILETIME ft);
IMPORT UINT WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR result);
IMPORT DWORD WINAPI GetTempPathA(DWORD capacity, LPSTR buffer);
IMPORT DWORD WINAPI GetTickCount(void);
IMPORT ULONGLONG WINAPI GetTickCount64(void);
IMPORT LPVOID WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T bytes);
IMPORT BOOL WINAPI HeapFree(HANDLE h, DWORD flags, LPVOID memory);
IMPORT LPVOID WINAPI HeapReAlloc(HANDLE h, DWORD flags, LPVOID memory, SIZE_T bytes);
IMPORT SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, LPCVOID memory);
IMPORT BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int length, LPWSTR dest, int capacity);
IMPORT BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter);
IMPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped);
IMPORT BOOL WINAPI RemoveDirectoryA(LPCSTR path);
IMPORT BOOL WINAPI SetCurrentDirectoryA(LPCSTR path);
IMPORT BOOL WINAPI SetEndOfFile(HANDLE h);
IMPORT BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
IMPORT BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER result, DWORD method);
IMPORT void WINAPI Sleep(DWORD ms);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default);
IMPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped);
IMPORT int WINAPI lstrlenW(LPCWSTR s);
