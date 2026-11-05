# win32k: the user-misc module

`src/Process/Win32k/Win32kUserMisc*.cpp` implements the part of USER that is not windows, messages, classes, queues, timers, hooks, focus, scroll bars, carets, painting, window
stations or desktops (those belong to the window manager, `user::`), and not GDI (`gdi::`). It implements the `user_misc::` contract of `Win32kUser.hpp` and registers 119 `NtUser*`
services with `register_handler()` from `install_user_misc()`. The conventions of the services follow Wine's `ntuser.h` (the `NtUserCall*` code numbers, the
`NtUserThunkedMenuItemInfo` methods, the `get_clipboard_params` blocks, the DPI context encoding); where Wine has no prototype the real win32k / ReactOS signature is used.

All state lives in memory from the allocator of the system (made on first use, guarded by the system lock) and is dropped for a process by one exit hook. Objects have
Windows-like handles: `(generation << 16) | (slot + 1)`, the generation starting at a base of its own per kind (menus 1, cursors/icons 2, accelerator tables 3, input contexts 4,
raw input records 5) and counting up when a slot is reused, so stale and foreign handles are refused. Every service validates user pointers with the `Call` helpers (a bad
pointer is `ERROR_NOACCESS`, 998) and sizes with checked arithmetic.

## Files

| File | Content |
| --- | --- |
| `Win32kUserMisc.hpp` | the shared header: `Registry<T>` (handle tables over `Hive`), process/thread records, the additions other modules may use |
| `Win32kUserMisc.cpp` | records, DPI of a thread, the exit hook, `window_destroyed()`, `install_user_misc()` |
| `...Call.cpp` | `NtUserCallNoParam/OneParam/TwoParam` |
| `...Menu.cpp` | menus, the menu of a window, system menus |
| `...Cursor.cpp` | cursors and icons, the cursor of the screen |
| `...Accel.cpp` | accelerator tables |
| `...Clipboard.cpp` | the clipboard |
| `...Settings.cpp` | `SystemParametersInfo`, metrics, colours, double-click time |
| `...Monitor.cpp` | the monitor, display modes, display configuration, DPI awareness |
| `...Keyboard.cpp` | keyboard layouts, US layout tables, input contexts (IME) |
| `...Input.cpp` | raw input, pointer services |
| `...Shell.cpp` | shell windows, shell hooks, shutdown block reasons, display affinity, beep |

## Services

* **Multiplexers**: `CallNoParam` (GetDesktopWindow, GetDialogBaseUnits, GetLastInputTime, GetProgmanWindow, GetShellWindow, GetTaskmanWindow, DisplayModeChanged, ExitingThread,
  ThreadDetach), `CallOneParam` (CreateCursorIcon, EnableThunkLock, GetIconParam, GetMenuItemCount, GetPrimaryMonitorRect, GetSysColor, GetSysColorBrush/Pen, GetSystemMetrics,
  GetVirtualScreenRect, SetKeyboardAutoRepeat, SetThreadDpiAwarenessContext), `CallTwoParam` (GetDialogProc, GetMenuInfo, GetMonitorInfo, GetSystemMetricsForDpi,
  MonitorFromRect, SetIconParam, AdjustWindowRect, GetVirtualScreenRect). Codes that belong to other modules (EnableDC, the Direct3D adapter, the async key state,
  SetIMECompositionRect, AllocWinProc, and everything of `CallHwnd*`) fail with `ERROR_INVALID_PARAMETER`. `GetSysColorBrush/Pen` fail with `ERROR_NOT_SUPPORTED` until the
  GDI module calls `set_solid_brush_factory()` / `set_solid_pen_factory()` (the gdi:: contract cannot make a brush); the objects are cached per colour.
* **Menus**: CreateMenu, CreatePopupMenu, DestroyMenu (recursive), RemoveMenu, DeleteMenu, CheckMenuItem, EnableMenuItem, HiliteMenuItem, SetMenuDefaultItem,
  SetMenuContextHelpId, SetMenuFlagRtoL, ThunkedMenuInfo (+ `GetMenuInfo` code), ThunkedMenuItemInfo (SetMenuItemInfo, InsertMenuItem, CheckMenuRadioItem, GetMenuDefaultItem,
  GetMenuItemID, GetMenuItemInfoA/W, GetMenuState, GetSubMenu), SetMenu, DrawMenuBar, GetSystemMenu, SetSystemMenu. Items hold string, id, type, state, submenu, check-mark
  bitmaps, item data, bitmap. Items are found by position or by command (commands are searched in the submenus as well); cycles of submenus are refused; a window's menu and
  system menu are destroyed with the window. `CheckMenuRadioItem` arrives as `ThunkedMenuItemInfo(menu, first, last, 2, check, by_position)` (Wine's extension; the
  `info`/`str` pointers carry the two numbers). `window_menu(hwnd)` gives the menu bar for `GetWindowLong(GWLP_ID)`.
* **Cursors and icons**: SetCursorIconData (takes over the bitmaps of the frames, animated sequences and rates), FindExistingCursorIcon, DestroyCursor, GetIconInfo (copies
  of the bitmaps; they are 32-bit), GetIconSize, GetCursorFrameInfo, SetCursor/GetCursor (per thread), ShowCursor (counter), SetSystemCursor, GetCursorInfo, GetCursorPos,
  SetCursorPos, ClipCursor, GetClipCursor. The position services call into the window manager (`cursor_position`, `inject_mouse_move`), which take the system lock
  themselves, so these five register with `locked = false` and take the lock only around their own state.
* **Accelerator tables**: Create/Copy/DestroyAcceleratorTable; `accelerator_entries()` for TranslateAccelerator.
* **Clipboard**: Open/Close/Empty, SetClipboardData, GetClipboardData, Enum/IsAvailable/Count formats, owner, open window, sequence number, viewer chain (SetClipboardViewer,
  ChangeClipboardChain, GetClipboardViewer), format listeners (WM_CLIPBOARDUPDATE), GetUpdatedClipboardFormats, GetPriorityClipboardFormat. See below.
* **Settings**: SystemParametersInfo and ...ForDpi, SetSysColors, Get/SetDoubleClickTime, SwapMouseButton, UpdatePerUserSystemParameters,
  ScaleSystemMetricForDPIWithoutCache. Some 80 simple settings (BOOL/UINT) with Windows 10 defaults, the accessibility structures, NONCLIENTMETRICS / ICONMETRICS /
  MINIMIZEDMETRICS / LOGFONT / work area / wallpaper / mouse thresholds. `system_metrics()` has the Windows 10 values at 96 dpi and scales the size metrics for other DPIs;
  `system_color()` the default scheme. Nothing is read from or written to a registry.
* **Monitor and display**: one monitor, `0x10001`, sized from `gdi::screen_width()/height()`; MonitorFrom*, GetMonitorInfo (MONITORINFO/EX), EnumDisplaySettings (a list of
  standard modes up to 4K plus the current one), ChangeDisplaySettings (resizes the screen surface; CDS_TEST, registry mode), EnumDisplayDevices, GetDisplayConfigBufferSizes,
  QueryDisplayConfig, DisplayConfigGetDeviceInfo, GetDpiForMonitor, GetDpiForCurrentProcess, GetSystemDpiForProcess, Get/SetProcessDpiAwarenessContext (once), the thread
  context, the two point conversions (identity: one monitor).
* **Keyboard**: GetKeyboardLayout/List/Name, GetKeyboardType, ActivateKeyboardLayout, LoadKeyboardLayoutEx, UnloadKeyboardLayout (only the US layout 00000409 has tables),
  MapVirtualKeyEx (all five types, with the quirks of Windows), VkKeyScanEx, ToUnicodeEx, GetKeyNameText (with the Windows quirks for the numpad/extended names); input
  contexts: Create/Destroy/Update/QueryInputContext, AssociateInputContext, Get/SetAppImeLevel.
* **Raw input**: RegisterRawInputDevices, GetRegisteredRawInputDevices, GetRawInputDeviceList (the mouse and the keyboard), GetRawInputDeviceInfo, GetRawInputData,
  GetRawInputBuffer. The records come from `record_raw_input()`, which the input layer calls for each device event. **Pointer**: Enable/IsMouseInPointer, InitializeTouchInjection,
  GetPointerDevices (none).
* **Shell and process**: SetShellWindowEx, SetProgmanWindow, SetTaskmanWindow, Register/DeregisterShellHookWindow (`shell_hook_windows()` for the window manager),
  Get/SetProcessDefaultLayout, ShutdownBlockReasonCreate/Query/ReasonDestroy, Get/SetWindowDisplayAffinity, MessageBeep (recorded; the sound belongs to the audio layer).

## The clipboard

**One clipboard for the whole system.** The desktop's own programs (kernel mode: text boxes, the editor, the spreadsheet, the browser, the paint program) use `kernel::Clipboard`
(`src/Kernel/Clipboard.hpp`: text or a picture); Windows programs use this module. `install_clipboard_bridge` (called by `main.cpp` with the desktop's clipboard) keeps the two the
same: each side has a sequence number and the bridge, run before every read and after every change of either side (`kernel::Clipboard::set_observer`, and at the start of every
clipboard service here), copies a change across - text as CF_UNICODETEXT (UTF-8 <-> UTF-16), a picture as a 32-bit bottom-up CF_DIB (a 24-bit DIB works too; no alpha means opaque).
A change on the desktop side wins unless a Windows program has just closed the clipboard (`CloseClipboard` pushes what it wrote); a Windows clipboard that is open is neither read
nor written (it is half filled), the desktop's change is taken when it closes. A change that comes from the desktop wakes the Windows listeners (WM_CLIPBOARDUPDATE) and viewers.
Other Windows formats stay on the Windows side. The bridge works before any Windows program ran: the first clipboard service call brings the Windows side up to date. Tests:
`Win32kMiscTest` (bridge section), `tests/windows/user32test.c` (user32's clipboard functions and Edit copy/paste). user32 side: `src/Windows/user32/clipboard.inc`.

Formats keep their bytes (the user-mode side keeps handles and passes them back). Availability follows Windows: the formats set, then the synthesized ones - CF_LOCALE and
the missing members of the text family first, then the metafile pair, then the bitmap family. Text is converted between CF_TEXT, CF_OEMTEXT and CF_UNICODETEXT (cp1252; the
OEM code page is treated as the ANSI one); the metafile and bitmap conversions are left to user mode (they are listed but `GetClipboardData` returns no data for them).
`SetClipboardData` returns an NTSTATUS (0, or `STATUS_INVALID_DEVICE_STATE` when the clipboard is not open by the caller, `STATUS_INVALID_PARAMETER`, ...) and sets the
Win32 error too. A format set without data and without handle is *delayed*: `GetClipboardData` posts `WM_RENDERFORMAT` to the owner and waits (system lock given up, five
seconds) for the owner's thread to set it; the owner may set that format although the clipboard is open by the asker. `EmptyClipboard` posts `WM_DESTROYCLIPBOARD` to the
previous owner, `CloseClipboard` posts `WM_CLIPBOARDUPDATE` to the listeners and `WM_DRAWCLIPBOARD` to the viewer when the sequence number moved while it was open. Windows that
go (`window_destroyed()`) leave the chain, the listeners, and ownership (their delayed formats are dropped). `clipboard_unicode_text()` / `clipboard_set_unicode_text()` are the
bridge to the kernel's text clipboard (`src/Kernel/Clipboard.hpp`), not wired yet.

## Not implemented (they keep answering STATUS_NOT_IMPLEMENTED)

* Layout and the modal loop of menus (TrackPopupMenuEx, EndMenu, GetMenuBarInfo, GetMenuItemRect, MenuItemFromPoint, CalcMenuBar, DrawMenuBarTemp) are done in user mode (user32 `menuui.inc`, see User.md): they need the font metrics and a message loop that the kernel does not have. `NtUserTrackPopupMenuEx` and friends keep answering STATUS_NOT_IMPLEMENTED.
* Needing GDI drawing that the gdi:: contract lacks: DrawIconEx, DrawCaption*, CreateEmptyCursorObject-style loaders (LoadCursorsAndIcons), GetClassIcoCur, GetOemBitmapSize.
* Needing a user-mode callback: EnumDisplayMonitors (Wine's form calls back into the process for each monitor).
* Needing registry/atom services that do not exist yet: GetClipboardFormatName (registered formats are atoms), RegisterClipboardFormat.
* Needing queue/input state of the window manager: GetKeyboardState/SetKeyboardState/GetKeyState/GetAsyncKeyState, GetMouseMovePointsEx, touch/gesture/pointer message
  services (GetTouchInputInfo, GetGestureInfo, GetPointerInfoList, Inject*), WaitForInputIdle, BlockInput.
* Other: GetGuiResources/QueryUserCounters (the GDI object count is not in the contract), DisplayConfigSetDeviceInfo, SetDisplayConfig, ChangeDisplaySettingsEx flags for secondary
  displays (there is one), auto-rotation, Mag* services and the remote-session Dwm* ones (DwmGetRemoteSession*), session and logon services, ShellHandwriting*, the interactive-control services, the IME UI services beyond those under *Input method manager* below.

## Input method manager (`Win32kUserMiscKeyboard.cpp`)

The state behind `imm32.dll` (src/Windows/imm32). `CreateInputContext(client ptr)` makes a handle owned by the calling process and thread; `QueryInputContext(h, 0/1/2)` gives the
client pointer, thread id and process id; `UpdateInputContext(h, 0/1, v)` changes the client pointer / thread. `AssociateInputContext(hwnd, h, flags)` records the context of a window
(the window must belong to the context's thread; context 0 is stored and means the IME is off for the window); `CallHwnd(GetWindowInputContext)` answers it, or 0xFFFFFFFF for a window
nothing was associated with (imm32 then uses the default context of the window's thread). `BuildHimcList(tid, count, out, &needed)` lists the process's contexts of a thread (0: the caller,
-1: all). `Get/SetImeInfoEx` keep the 352-byte `IMEINFOEX` of a layout (set only for a loaded layout or an IME layout, 0xE.......; get by layout or by IME file name). `Get/SetImeHotKey`
is a table of hot keys (action 0 sets, 1 deletes, 2 empties it). `NotifyIMEStatus`, `BroadcastImeShowStatusChange`/`GetIMEShowStatus`/`CheckImeShowStatusInThread` and
`SetImeOwnerWindow` are recorded state; `DisableThreadIme(tid)` (-1: the process) drops the thread's associations and makes later `CreateInputContext` calls fail with
`ERROR_ACCESS_DENIED` until the process ends; `SetThreadLayoutHandles` sets the thread's layout to a loaded one. Contexts, associations and the disabled threads go with their
process; window entries with the window. Tests: `test_keyboard` in `tests/Win32kMiscTest.cpp`.

## Desktop window manager (`Win32kUserMiscDwm.cpp`)

The state behind `dwmapi.dll` (src/Windows/dwmapi). Windows keeps it in dwm.exe; here it is win32k's. `NtTcosDwm` (service 506, outside the win32k range: `register_dwm_handler`)
stores per-window values (`SetProp`/`GetProp`: the `DWMWA_*` attributes, frame margins, blur behind, iconic bitmaps; only the window's process may set, anyone may read;
`DWMWA_EXTENDED_FRAME_BOUNDS` is the window rectangle) and thumbnails (`RegisterThumbnail`/`UnregisterThumbnail`/`UpdateThumbnail`/`ThumbnailSourceSize`; a process sees only its
own; destroyed with their destination window or process). The real services `NtUserDwmValidateWindow`, `DwmWindowNotificationsEnabled`, `DwmLockScreenUpdates` (nested),
`DwmKernelStartup`/`Shutdown` (one compositor at a time) and `Get/SetWindowCompositionAttribute` are recorded state. Nothing is composited from these values yet: they are what a
program set and reads back. Tests: `test_dwm` in `tests/Win32kMiscTest.cpp`.

## Contract additions

`Win32kUserMisc.hpp` declares, besides the `user_misc::` contract: `window_menu`, `window_system_menu`, `accelerator_entries`, `shell_hook_windows`, `window_input_context`,
`system_cursor`/`system_icon` (stock objects by OCR/OIC id, created frame-less), `cursor_clip_rect`, `record_raw_input`, `clipboard_*`, `set_system_dpi`,
`set_solid_brush_factory`/`set_solid_pen_factory`, `note_last_input`, `monitor_handle`/`is_monitor`. The window manager must call `user_misc::window_destroyed(hwnd)` when it
destroys a window (menus, clipboard chain, input contexts, shell windows depend on it) and should answer `GetMenu`/`GWLP_ID` of a top-level window with `window_menu()`.

## Tests

`tests/Win32kMiscTest.cpp` (`tests/run_host_tests.sh Win32kMiscTest`, ASan + UBSan): the functions of the window manager and GDI contracts this module calls have weak stand-ins
in the test (fake windows, a fake screen and bitmaps), so the test links without the other modules and uses the real ones when they are linked. Threads are simulated with
`set_thread_override()`. The test covers item arithmetic of menus (insert/set/find/recursion/default/radio), icon descriptions, the clipboard flows (ownership, synthesis,
delayed rendering, notifications, process exit), SPI round trips and errors, metrics at several DPIs, display modes and configuration, keyboard translation against the
cases of Wine's `input.c`, raw input, bad pointers and truncated buffers.
