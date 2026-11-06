/* Tests of the user32.dll functions TheoryCraftOS wired to win32k for game engines and toolkits (Godot, SDL): keyboard layouts and key translation, DPI awareness, the
 * display configuration queries and the double-click time.
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/test.h"

static void test_keyboard_layout(void)
{
    HKL hkl = GetKeyboardLayout(0), list[8];
    WCHAR wname[KL_NAMELENGTH];
    char name[KL_NAMELENGTH];
    int n;

    ok(hkl == (HKL)(ULONG_PTR)0x04090409, "GetKeyboardLayout %p\n", hkl);
    ok(GetKeyboardLayout(GetCurrentThreadId()) == hkl, "layout of the thread by id\n");
    ok(GetKeyboardLayoutNameW(wname) && !lstrcmpW(wname, L"00000409"), "GetKeyboardLayoutNameW %s\n", wine_dbgstr_w(wname));
    ok(GetKeyboardLayoutNameA(name) && !strcmp(name, "00000409"), "GetKeyboardLayoutNameA %s\n", name);
    n = GetKeyboardLayoutList(0, NULL);
    ok(n >= 1, "GetKeyboardLayoutList count %d\n", n);
    n = GetKeyboardLayoutList(8, list);
    ok(n >= 1 && list[0] == hkl, "GetKeyboardLayoutList %d %p\n", n, n ? list[0] : NULL);
    ok(GetKeyboardType(0) == 4, "GetKeyboardType(0) %d\n", GetKeyboardType(0));
    ok(GetKeyboardType(2) == 12, "GetKeyboardType(2) %d\n", GetKeyboardType(2));
    ok(LoadKeyboardLayoutW(L"00000409", 0) == hkl, "LoadKeyboardLayoutW US\n");
    ok(LoadKeyboardLayoutA("00000409", 0) == hkl, "LoadKeyboardLayoutA US\n");
    ok(LoadKeyboardLayoutW(L"00000407", 0) == NULL, "LoadKeyboardLayoutW of a layout with no tables\n");
    ok(ActivateKeyboardLayout(hkl, 0) == hkl, "ActivateKeyboardLayout answers the previous layout\n");
    ok(!UnloadKeyboardLayout(hkl), "the last layout stays loaded\n");
}

static void test_map_virtual_key(void)
{
    ok(MapVirtualKeyW('A', MAPVK_VK_TO_VSC) == 0x1E, "A -> %#x\n", MapVirtualKeyW('A', MAPVK_VK_TO_VSC));
    ok(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC) == 0x1C, "Enter -> %#x\n", MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    ok(MapVirtualKeyW(0x1E, MAPVK_VSC_TO_VK) == 'A', "0x1E -> %#x\n", MapVirtualKeyW(0x1E, MAPVK_VSC_TO_VK));
    ok(MapVirtualKeyW(0x2A, MAPVK_VSC_TO_VK) == VK_SHIFT, "left shift -> %#x\n", MapVirtualKeyW(0x2A, MAPVK_VSC_TO_VK));
    ok(MapVirtualKeyW(0x2A, MAPVK_VSC_TO_VK_EX) == VK_LSHIFT, "left shift (exact) -> %#x\n", MapVirtualKeyW(0x2A, MAPVK_VSC_TO_VK_EX));
    ok(MapVirtualKeyW(0x36, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT, "right shift (exact) -> %#x\n", MapVirtualKeyW(0x36, MAPVK_VSC_TO_VK_EX));
    ok(MapVirtualKeyW(0x48, MAPVK_VSC_TO_VK) == VK_UP, "0x48 -> %#x\n", MapVirtualKeyW(0x48, MAPVK_VSC_TO_VK));      /* (Godot reads arrow keys this way) */
    ok(MapVirtualKeyW(0xE01D, MAPVK_VSC_TO_VK_EX) == VK_RCONTROL, "E0 1D -> %#x\n", MapVirtualKeyW(0xE01D, MAPVK_VSC_TO_VK_EX));
    ok(MapVirtualKeyW('A', MAPVK_VK_TO_CHAR) == 'A', "VK_TO_CHAR A %#x\n", MapVirtualKeyW('A', MAPVK_VK_TO_CHAR));
    ok(MapVirtualKeyW(VK_OEM_1, MAPVK_VK_TO_CHAR) == ';', "VK_TO_CHAR OEM_1 %#x\n", MapVirtualKeyW(VK_OEM_1, MAPVK_VK_TO_CHAR));
    ok(MapVirtualKeyA(VK_OEM_2, MAPVK_VK_TO_CHAR) == '/', "MapVirtualKeyA OEM_2 %#x\n", MapVirtualKeyA(VK_OEM_2, MAPVK_VK_TO_CHAR));
    ok(MapVirtualKeyExW(VK_SPACE, MAPVK_VK_TO_VSC, GetKeyboardLayout(0)) == 0x39, "space %#x\n", MapVirtualKeyExW(VK_SPACE, MAPVK_VK_TO_VSC, GetKeyboardLayout(0)));
    ok(MapVirtualKeyW(0x7F, MAPVK_VSC_TO_VK) == 0, "a scan code with no key\n");
}

static void test_to_unicode(void)
{
    BYTE state[256] = {0};
    WCHAR buffer[4];
    WORD ascii = 0;
    int n;

    memset(buffer, 0, sizeof(buffer));
    n = ToUnicode('A', 0x1E, state, buffer, 4, 0);
    ok(n == 1 && buffer[0] == 'a', "A: %d %#x\n", n, buffer[0]);
    state[VK_SHIFT] = 0x80;
    n = ToUnicodeEx('A', 0x1E, state, buffer, 4, 0, GetKeyboardLayout(0));
    ok(n == 1 && buffer[0] == 'A', "shift A: %d %#x\n", n, buffer[0]);
    n = ToUnicode('1', 0x02, state, buffer, 4, 0);
    ok(n == 1 && buffer[0] == '!', "shift 1: %d %#x\n", n, buffer[0]);
    state[VK_SHIFT] = 0;
    n = ToUnicode('A', 0x1E | 0x8000, state, buffer, 4, 0);
    ok(n == 0, "a key release makes no character: %d\n", n);
    n = ToUnicode(VK_F1, 0x3B, state, buffer, 4, 0);
    ok(n == 0, "F1 makes no character: %d\n", n);
    n = ToAscii(VK_OEM_PERIOD, 0x34, state, &ascii, 0);
    ok(n == 1 && ascii == '.', "ToAscii period: %d %#x\n", n, ascii);
    ok(ToUnicode('A', 0x1E, NULL, buffer, 4, 0) == 0, "no key state\n");
}

static void test_key_scan_and_names(void)
{
    WCHAR wname[32];
    char name[32];
    int n;

    ok(VkKeyScanW('a') == 'A', "VkKeyScanW a %#x\n", VkKeyScanW('a'));
    ok(VkKeyScanW('A') == 0x141, "VkKeyScanW A %#x\n", VkKeyScanW('A'));
    ok(VkKeyScanA('!') == 0x131, "VkKeyScanA ! %#x\n", VkKeyScanA('!'));
    ok(VkKeyScanExW(';', GetKeyboardLayout(0)) == VK_OEM_1, "VkKeyScanExW ; %#x\n", VkKeyScanExW(';', GetKeyboardLayout(0)));
    ok(OemKeyScan('a') == 0x1E, "OemKeyScan a %#lx\n", OemKeyScan('a'));
    ok(OemKeyScan('A') == (0x10000 | 0x1E), "OemKeyScan A %#lx\n", OemKeyScan('A'));

    n = GetKeyNameTextW(0x1E << 16, wname, 32);
    ok(n == 1 && !lstrcmpW(wname, L"A"), "GetKeyNameTextW A: %d %s\n", n, wine_dbgstr_w(wname));
    n = GetKeyNameTextA(0x01 << 16, name, 32);
    ok(n == 3 && !strcmp(name, "Esc"), "GetKeyNameTextA Esc: %d %s\n", n, name);
    n = GetKeyNameTextW((0x48 << 16) | (1 << 24), wname, 32);
    ok(!lstrcmpW(wname, L"Up"), "extended up: %d %s\n", n, wine_dbgstr_w(wname));
    n = GetKeyNameTextA(0x39 << 16, name, 3);
    ok(n == 2 && !strcmp(name, "Sp"), "a short buffer: %d %s\n", n, name);
    memset(name, 'x', sizeof(name));
    n = GetKeyNameTextA(0x39 << 16, name, 1);
    ok(n == 0 && name[0] == 0, "room for the terminator only: %d\n", n);
}

static void test_dpi_contexts(void)
{
    DPI_AWARENESS_CONTEXT previous, context;

    ok(IsValidDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE), "unaware is valid\n");
    ok(IsValidDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "per monitor v2 is valid\n");
    ok(!IsValidDpiAwarenessContext(NULL), "NULL is not valid\n");
    ok(!IsValidDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-6), "-6 is not valid\n");
    ok(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE) == DPI_AWARENESS_UNAWARE, "unaware\n");
    ok(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) == DPI_AWARENESS_SYSTEM_AWARE, "system\n");
    ok(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) == DPI_AWARENESS_PER_MONITOR_AWARE, "per monitor\n");
    ok(GetAwarenessFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == DPI_AWARENESS_PER_MONITOR_AWARE, "per monitor v2\n");
    ok(GetAwarenessFromDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-6) == DPI_AWARENESS_INVALID, "invalid\n");
    ok(AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE), "equal\n");
    ok(!AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "v1 and v2 differ\n");
    ok(!AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT_UNAWARE, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE), "unaware and system differ\n");
    ok(GetDpiFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE) == 96, "unaware dpi %u\n", GetDpiFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE));
    ok(GetDpiFromDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) == 0, "per monitor has no dpi of its own\n");

    /* the thread follows the (still unaware) process until it sets a context of its own */
    ok(!IsProcessDPIAware(), "the process starts unaware\n");
    context = GetThreadDpiAwarenessContext();
    ok(GetAwarenessFromDpiAwarenessContext(context) == DPI_AWARENESS_UNAWARE, "thread context %p\n", context);
    ok(GetDpiForSystem() == 96, "GetDpiForSystem unaware %u\n", GetDpiForSystem());
    previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ok(previous != NULL && GetAwarenessFromDpiAwarenessContext(previous) == DPI_AWARENESS_UNAWARE, "previous %p\n", previous);
    context = GetThreadDpiAwarenessContext();
    ok(AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "thread now %p\n", context);
    context = GetThreadDpiAwarenessContext();
    ok(AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "reading it does not change it: %p\n", context);
    SetThreadDpiAwarenessContext(previous);
    ok(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) == DPI_AWARENESS_UNAWARE, "back to the process's\n");
    SetLastError(0xdeadbeef);
    ok(SetThreadDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-7) == NULL, "an invalid context is refused\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
}

static void test_dpi_window(void)
{
    HWND hwnd = CreateWindowExW(0, L"static", L"dpi", WS_POPUP, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    POINT pt = {10, 20};
    UINT dpi;

    ok(hwnd != NULL, "CreateWindowEx %lu\n", GetLastError());
    ok(GetDpiForWindow(hwnd) == 96, "unaware window dpi %u\n", GetDpiForWindow(hwnd));
    ok(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd)) == DPI_AWARENESS_UNAWARE, "window context\n");
    SetLastError(0xdeadbeef);
    ok(GetDpiForWindow(NULL) == 0 && GetLastError() == ERROR_INVALID_WINDOW_HANDLE, "no window: %lu\n", GetLastError());
    ok(GetWindowDpiAwarenessContext(NULL) == NULL, "no window context\n");
    ok(EnableNonClientDpiScaling(hwnd), "EnableNonClientDpiScaling\n");
    ok(LogicalToPhysicalPointForPerMonitorDPI(hwnd, &pt) && pt.x == 10 && pt.y == 20, "point %ld,%ld\n", pt.x, pt.y);
    ok(PhysicalToLogicalPoint(hwnd, &pt) && pt.x == 10 && pt.y == 20, "point %ld,%ld\n", pt.x, pt.y);
    ok(GetSystemMetricsForDpi(SM_CXVSCROLL, 96) == GetSystemMetrics(SM_CXVSCROLL), "metrics at 96 dpi\n");
    ok(GetSystemMetricsForDpi(SM_CXVSCROLL, 192) > GetSystemMetricsForDpi(SM_CXVSCROLL, 96), "metrics grow with the dpi\n");

    /* the process becomes per-monitor aware, once */
    ok(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "SetProcessDpiAwarenessContext %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE), "the awareness is set once\n");
    ok(GetLastError() == ERROR_ACCESS_DENIED, "error %lu\n", GetLastError());
    ok(IsProcessDPIAware(), "the process is aware now\n");
    ok(SetProcessDPIAware(), "SetProcessDPIAware still answers TRUE\n");
    dpi = GetDpiForWindow(hwnd);
    ok(dpi >= 96, "aware window dpi %u\n", dpi);
    ok(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd)) == DPI_AWARENESS_PER_MONITOR_AWARE, "window context aware\n");
    ok(GetDpiForSystem() == dpi, "GetDpiForSystem %u, window %u\n", GetDpiForSystem(), dpi);
    DestroyWindow(hwnd);
}

static void test_display_config(void)
{
    DISPLAYCONFIG_PATH_INFO paths[4];
    DISPLAYCONFIG_MODE_INFO modes[8];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source;
    MONITORINFOEXW info;
    UINT32 path_count = 0, mode_count = 0;
    LONG ret;

    ret = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    ok(ret == ERROR_SUCCESS && path_count >= 1 && mode_count >= 1, "GetDisplayConfigBufferSizes %ld %u %u\n", ret, path_count, mode_count);
    ok(GetDisplayConfigBufferSizes(0, &path_count, &mode_count) == ERROR_INVALID_PARAMETER, "no flags\n");
    ok(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, NULL, &mode_count) == ERROR_INVALID_PARAMETER, "no count\n");

    path_count = 4; mode_count = 8;
    ret = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL);
    ok(ret == ERROR_SUCCESS && path_count == 1, "QueryDisplayConfig %ld %u\n", ret, path_count);
    if (ret != ERROR_SUCCESS) return;
    ok(paths[0].flags & DISPLAYCONFIG_PATH_ACTIVE, "path flags %#x\n", paths[0].flags);
    ok(modes[0].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE, "mode type %u\n", modes[0].infoType);
    ok(modes[0].sourceMode.width == (UINT)GetSystemMetrics(SM_CXSCREEN), "source width %u\n", modes[0].sourceMode.width);
    path_count = 0;
    ok(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL) == ERROR_INVALID_PARAMETER, "a count of 0\n");
    path_count = 1; mode_count = 1;
    ok(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL) == ERROR_INSUFFICIENT_BUFFER && mode_count == 1, "no room\n");

    memset(&source, 0, sizeof(source));
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[0].sourceInfo.adapterId;
    source.header.id = paths[0].sourceInfo.id;
    ret = DisplayConfigGetDeviceInfo(&source.header);
    ok(ret == ERROR_SUCCESS, "DisplayConfigGetDeviceInfo %ld\n", ret);
    info.cbSize = sizeof(info);
    ok(GetMonitorInfoW(MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&info), "GetMonitorInfoW\n");
    ok(!lstrcmpW(source.viewGdiDeviceName, info.szDevice), "source %s monitor %s\n", wine_dbgstr_w(source.viewGdiDeviceName), wine_dbgstr_w(info.szDevice));
    source.header.size = sizeof(source) - 1;
    ok(DisplayConfigGetDeviceInfo(&source.header) == ERROR_INVALID_PARAMETER, "a short packet\n");
    source.header.size = 8;
    ok(DisplayConfigGetDeviceInfo(&source.header) == ERROR_GEN_FAILURE, "a header that does not hold itself\n");
    ok(DisplayConfigGetDeviceInfo(NULL) == ERROR_GEN_FAILURE, "no packet\n");
}

static void test_mouse_settings(void)
{
    UINT original = GetDoubleClickTime();

    ok(original > 0 && original <= 5000, "GetDoubleClickTime %u\n", original);
    ok(SetDoubleClickTime(400) && GetDoubleClickTime() == 400, "SetDoubleClickTime 400: %u\n", GetDoubleClickTime());
    ok(SetDoubleClickTime(0) && GetDoubleClickTime() == 500, "0 is the default: %u\n", GetDoubleClickTime());
    SetDoubleClickTime(original);
    ok(MessageBeep(MB_OK), "MessageBeep\n");
}

/* Raw input: SendInput's events come back as WM_INPUT records (with no device handle, as on Windows), and RIDEV_NOLEGACY keeps WM_MOUSEMOVE away. */
static UINT raw_count, legacy_moves;
static RAWINPUT raw_last_mouse, raw_last_key;
static LRESULT CALLBACK raw_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INPUT)
    {
        RAWINPUT input;
        UINT size = sizeof(input);
        ok(wparam == RIM_INPUT, "WM_INPUT wparam %Iu\n", wparam);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1)
        {
            ++raw_count;
            if (input.header.dwType == RIM_TYPEMOUSE) raw_last_mouse = input;
            else if (input.header.dwType == RIM_TYPEKEYBOARD) raw_last_key = input;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (message == WM_MOUSEMOVE) ++legacy_moves;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
static void pump(void)
{
    MSG msg;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 100)
    {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    }
}
static void send_mouse(LONG dx, LONG dy, DWORD flags)
{
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = flags;
    ok(SendInput(1, &in, sizeof(in)) == 1, "SendInput mouse %lu\n", GetLastError());
}
static void send_key(WORD vk, DWORD flags)
{
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = flags;
    ok(SendInput(1, &in, sizeof(in)) == 1, "SendInput key %lu\n", GetLastError());
}

static void test_raw_input(void)
{
    WNDCLASSW cls = {0};
    RAWINPUTDEVICE devices[2], registered[4];
    RAWINPUTDEVICELIST list[4];
    UINT count;
    HWND hwnd;
    char name[300];

    cls.lpfnWndProc = raw_proc;
    cls.lpszClassName = L"tcos_raw";
    RegisterClassW(&cls);
    hwnd = CreateWindowExW(0, L"tcos_raw", L"raw", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowEx %lu\n", GetLastError());
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    pump();

    count = ARRAY_SIZE(list);
    ok(GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST)) == 2, "a mouse and a keyboard\n");
    count = sizeof(name);
    ok(GetRawInputDeviceInfoA(list[0].hDevice, RIDI_DEVICENAME, name, &count) > 4 && !strncmp(name, "\\\\?\\", 4), "device name %s\n", name);

    devices[0].usUsagePage = 1; devices[0].usUsage = 2; devices[0].dwFlags = 0; devices[0].hwndTarget = hwnd;      /* as Godot registers the mouse */
    devices[1].usUsagePage = 1; devices[1].usUsage = 6; devices[1].dwFlags = 0; devices[1].hwndTarget = NULL;
    ok(RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)), "RegisterRawInputDevices %lu\n", GetLastError());
    count = ARRAY_SIZE(registered);
    ok(GetRegisteredRawInputDevices(registered, &count, sizeof(RAWINPUTDEVICE)) == 2, "registered\n");

    raw_count = legacy_moves = 0;
    send_mouse(5, -3, MOUSEEVENTF_MOVE);
    pump();
    ok(raw_count == 1, "WM_INPUT for a relative move: %u\n", raw_count);
    ok(raw_last_mouse.header.hDevice == NULL, "SendInput's records name no device\n");
    ok(raw_last_mouse.data.mouse.usFlags == MOUSE_MOVE_RELATIVE && raw_last_mouse.data.mouse.lLastX == 5 && raw_last_mouse.data.mouse.lLastY == -3,
       "motion %ld,%ld flags %#x\n", raw_last_mouse.data.mouse.lLastX, raw_last_mouse.data.mouse.lLastY, raw_last_mouse.data.mouse.usFlags);
    ok(legacy_moves >= 1, "the legacy WM_MOUSEMOVE too\n");
    raw_count = 0;
    SetCursorPos(200, 210);
    pump();
    ok(raw_count == 0, "SetCursorPos is not input: %u WM_INPUT\n", raw_count);      /* (Godot warps the cursor while capturing the mouse) */

    raw_count = 0;
    send_mouse(0, 0, MOUSEEVENTF_LEFTDOWN);
    send_mouse(0, 0, MOUSEEVENTF_WHEEL);
    pump();
    ok(raw_count == 2, "a button and a wheel: %u\n", raw_count);
    ok(raw_last_mouse.data.mouse.usButtonFlags == RI_MOUSE_WHEEL, "wheel flags %#x\n", raw_last_mouse.data.mouse.usButtonFlags);
    send_mouse(0, 0, MOUSEEVENTF_LEFTUP);
    pump();

    raw_count = 0;
    send_key('A', 0);
    send_key('A', KEYEVENTF_KEYUP);
    pump();
    ok(raw_count == 2, "two key records: %u\n", raw_count);
    ok(raw_last_key.data.keyboard.VKey == 'A' && raw_last_key.data.keyboard.MakeCode == 0x1E && (raw_last_key.data.keyboard.Flags & RI_KEY_BREAK) &&
       raw_last_key.data.keyboard.Message == WM_KEYUP, "key %#x make %#x flags %#x message %#x\n", raw_last_key.data.keyboard.VKey,
       raw_last_key.data.keyboard.MakeCode, raw_last_key.data.keyboard.Flags, raw_last_key.data.keyboard.Message);

    /* RIDEV_NOLEGACY: raw input only */
    devices[0].dwFlags = RIDEV_NOLEGACY;
    ok(RegisterRawInputDevices(devices, 1, sizeof(RAWINPUTDEVICE)), "NOLEGACY %lu\n", GetLastError());
    raw_count = legacy_moves = 0;
    send_mouse(4, 4, MOUSEEVENTF_MOVE);
    pump();
    ok(raw_count == 1 && legacy_moves == 0, "NOLEGACY: %u raw, %u WM_MOUSEMOVE\n", raw_count, legacy_moves);

    /* removed: no more WM_INPUT */
    devices[0].dwFlags = RIDEV_REMOVE; devices[0].hwndTarget = NULL;
    devices[1].dwFlags = RIDEV_REMOVE;
    ok(RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)), "remove %lu\n", GetLastError());
    raw_count = legacy_moves = 0;
    send_mouse(-4, -4, MOUSEEVENTF_MOVE);
    pump();
    ok(raw_count == 0 && legacy_moves >= 1, "after removing: %u raw, %u WM_MOUSEMOVE\n", raw_count, legacy_moves);
    ok(DefRawInputProc(NULL, 0, sizeof(RAWINPUTHEADER)) == 0 && DefRawInputProc(NULL, 0, 4) == -1, "DefRawInputProc\n");
    DestroyWindow(hwnd);
}

START_TEST(tcos_input)
{
    test_keyboard_layout();
    test_map_virtual_key();
    test_to_unicode();
    test_key_scan_and_names();
    test_dpi_contexts();
    test_dpi_window();
    test_display_config();
    test_mouse_settings();
    test_raw_input();
}
