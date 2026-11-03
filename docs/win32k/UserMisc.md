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

* Needing text metrics or a modal loop: TrackPopupMenuEx, EndMenu, GetMenuBarInfo, GetMenuItemRect, MenuItemFromPoint, CalcMenuBar, PaintMenuBar, DrawMenuBarTemp.
* Needing GDI drawing that the gdi:: contract lacks: DrawIconEx, DrawCaption*, CreateEmptyCursorObject-style loaders (LoadCursorsAndIcons), GetClassIcoCur, GetOemBitmapSize.
* Needing a user-mode callback: EnumDisplayMonitors (Wine's form calls back into the process for each monitor).
* Needing registry/atom services that do not exist yet: GetClipboardFormatName (registered formats are atoms), RegisterClipboardFormat.
* Needing queue/input state of the window manager: GetKeyboardState/SetKeyboardState/GetKeyState/GetAsyncKeyState, GetMouseMovePointsEx, touch/gesture/pointer message
  services (GetTouchInputInfo, GetGestureInfo, GetPointerInfoList, Inject*), WaitForInputIdle, BlockInput.
* Other: GetGuiResources/QueryUserCounters (the GDI object count is not in the contract), DisplayConfigSetDeviceInfo, SetDisplayConfig, ChangeDisplaySettingsEx flags for secondary
  displays (there is one), auto-rotation, Mag*/Dwm* services, session and logon services, ShellHandwriting*, the interactive-control services, the IME hot-key/info services.

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
