# win32k: the window manager (USER core)

Files: `src/Process/Win32k/Win32kUser*.cpp` (state, classes, windows, geometry, painting, messages, default window procedure, input, hooks, stations, weak fallbacks),
`Win32kUserInternal.hpp` (private state), `Win32kUser.hpp` (the contract with the other win32k modules and the kernel), `src/Windows/win32u/win32u.c` (the user-mode half),
`tests/Win32kUserTest.cpp`. All state lives in one `State` (allocator-backed `Hive`/`List`), guarded by the system lock; `user::reset_state()` drops it (host tests only).

## What is implemented (146 services)

* Classes and atoms: RegisterClassExWOW, UnregisterClass, GetClassInfoEx, GetClassName, SetClassLong/LongPtr/Word (+ get through NtUserCallHwndParam), RegisterWindowMessage,
  GetAtomName, InitializeClientPfnArrays.
* Windows: CreateWindowEx (full NCCREATE / NCCALCSIZE / CREATE / PARENTNOTIFY / show sequence), DestroyWindow (WM_DESTROY down the tree, WM_NCDESTROY from the leaves up, owned
  windows), SetParent, GetAncestor, FindWindowEx, BuildHwndList, EnableWindow, SetWindowLong/Ptr/Word (with WM_STYLECHANGING/CHANGED), SetWindowFNID, AlterWindowStyle,
  InternalGetWindowText, DefSetText, properties (Set/Get/RemoveProp, BuildPropList), QueryWindow, layered attributes, display affinity, help id, NtUserCallHwnd and
  NtUserCallHwndParam (every code of `ntuser.h` except MirrorRgn, SendHardwareInput, ExposeWindowSurface, which answer ERROR_NOT_SUPPORTED).
* Geometry: SetWindowPos (WM_WINDOWPOSCHANGING may change the position; WM_NCCALCSIZE; WM_SHOWWINDOW; WM_WINDOWPOSCHANGED), MoveWindow, ShowWindow (all commands, min/max/restore
  with saved placement), deferred positioning, Get/SetWindowPlacement, GetInternalWindowPos, window regions, z-order (topmost group, HWND_TOP/BOTTOM/TOPMOST/NOTOPMOST), hit
  testing (WindowFromPoint, ChildWindowFromPointEx, RealChildWindowFromPoint), `user::adjust_window_rect` (Windows 10 metrics from `user_misc::system_metrics`).
* Painting: update regions (rectangle lists), Invalidate/Validate Rect/Rgn, GetUpdateRect/Rgn, BeginPaint (WM_NCPAINT, WM_ERASEBKGND, PAINTSTRUCT), EndPaint, RedrawWindow
  (UPDATENOW/ERASENOW, children), UpdateWindow, LockWindowUpdate, GetDC/GetDCEx/GetWindowDC/ReleaseDC/WindowFromDC through the `gdi::` contract (the clip is the visible
  region: ancestors, covering siblings, children, window shape), scroll bar state (SetScrollInfo, GetScrollInfo, ShowScrollBar, EnableScrollBar).
* Messages: PostMessage (broadcast, thread messages), PostThreadMessage, PostQuitMessage, GetMessage/PeekMessage/WaitMessage (filters, PM_*, order: sent, posted, quit, input,
  paint, timers), MsgWaitForMultipleObjectsEx, GetQueueStatus, MessageCall (SendMessage, SendMessageTimeout, SendNotifyMessage, DefWindowProc), ReplyMessage, DispatchMessage,
  TranslateMessage (answers FALSE: characters are queued by `inject_char`), GetMessagePos, SetMessageExtraInfo, timers (SetTimer, KillTimer, system timers).
* DefWindowProc in the kernel: text messages, NCCREATE, NCCALCSIZE, NCHITTEST, WINDOWPOSCHANGED (-> WM_MOVE/WM_SIZE), NCACTIVATE, ACTIVATE (-> focus), SETREDRAW,
  MOUSEACTIVATE, PAINT, CLOSE (-> DestroyWindow), SYSCOMMAND (close/minimize/maximize/restore).
* Input: focus, activation, foreground, capture, AttachThreadInput (shared input state), GetThreadState, GetGUIThreadInfo, key state (GetKeyState, GetAsyncKeyState,
  Get/SetKeyboardState), cursor position and ClipCursor, hot keys, caret, TrackMouseEvent (WM_MOUSELEAVE), SendInput; `user::inject_*` for the machine's keyboard/mouse (hot keys,
  system keys, auto repeat, double clicks, click activation, hit test -> WM_NCMOUSEMOVE, wheel), `user::pump`.
* Hooks: SetWindowsHookEx, UnhookWindowsHookEx, UnhookWindowsHook, CallNextHookEx, CallMsgFilter (WH_MSGFILTER, WH_SYSMSGFILTER), WinEvent hooks (out of context).
* Stations: Create/Open/Close window station, Get/SetProcessWindowStation, CreateDesktopEx, OpenDesktop, OpenInputDesktop, CloseDesktop, SwitchDesktop, Get/SetThreadDesktop,
  BuildNameList, Get/SetObjectInformation.

Left unregistered (they keep answering STATUS_NOT_IMPLEMENTED): ScrollWindowEx and ScrollDC (they need a pixel copy through the GDI module), ExcludeUpdateRgn (needs region
operations on a DC), ShowOwnedPopups, ArrangeIconicWindows, SetInternalWindowPos, GetScrollBarInfo, GetTitleBarInfo, SBGetParms, DragDetect, the low-level and CBT/shell/journal
hooks, SendMessageCallback, CallWindowProc (user32 does it), cross-process hook DLL injection, WH_CALLWNDPROC/WH_GETMESSAGE/WH_KEYBOARD/WH_MOUSE calls made by the kernel
(only CallNextHookEx/CallMsgFilter call hooks), MDI and dialog services beyond the info slots (`NtUserCallHwnd` GetDialogInfo/GetMDIClientInfo and the Set variants).

## The callback protocol (kernel to user mode)

The kernel cannot run user code in the middle of a service. A service that may have to call a window procedure (or a hook) does its bookkeeping, queues the calls and answers
`TCOS_UPCALL_DUE` (0x7FFFFFF1); the stubs of these services are renamed `NtUserXxx_raw` and the exported `NtUserXxx` in `win32u.c` is a wrapper:

1. **Operations.** A service that needs calls in the middle starts an *operation* (`Op`, one per nesting level on the thread's stack) with a step function and a few words of
   state. `op_continue` runs the operation until it needs a call: the step queues *upcalls* (a window-procedure call with the message, optionally with a copy of a structure that
   `lparam` points to: CREATESTRUCT, WINDOWPOS, NCCALCSIZE_PARAMS, STYLESTRUCT, MINMAXINFO, ...) and the service answers `TCOS_UPCALL_DUE`; or the step adds *notes* (messages to
   send in order) that `op_continue` turns into upcalls. When every call is done, the step is called again with the result of the last one (`first_result`, `payload`) and
   continues or returns the answer of the service. Services that do not need a call never see any of this.
2. **The wrapper** (`win32u.c`): `r = raw(...)`; while `r == TCOS_UPCALL_DUE`: `drain_upcalls()` then `r = NtUserTcosResume()` (it runs the top operation again; it may answer
   `TCOS_UPCALL_DUE` again). `drain_upcalls()` loops `NtUserTcosGetUpcall(&record)` (fills `user::UpcallRecord`: id, kind, flags, procedure, hwnd, message, wparam, lparam, payload),
   calls the procedure (kinds: window procedure, hook procedure, WinEvent procedure, timer procedure), and reports the result with `NtUserTcosUpcallDone(id, result, payload,
   length)`. A payload (flag 2) is copied to the wrapper's stack and `lparam` points to it; the kernel can name a pointer inside it (`fix_at`/`fix_to`) and the creation
   messages get their name and class pointers from the wrapper (flag 1). The changed payload goes back with the result (WM_WINDOWPOSCHANGING, NCCALCSIZE, ...). Calls nest:
   a procedure may call services that start operations of their own.
3. **Message retrieval** (GetMessage, PeekMessage, WaitMessage, MsgWaitForMultipleObjectsEx) has no operation: when sent messages (or other calls) are queued for the thread they answer
   `TCOS_DRAIN_RETRY` (0x7FFFFFF3); the wrapper makes the calls and calls the service again with the same arguments.
4. **Sends between threads.** Only threads own queues (made on the first window/message call). SendMessage to a window of the same thread is a call in the operation. To a window
   of another thread (or process) the sender queues an upcall for that thread, wakes it, and waits (`wait_until`, with the SendMessageTimeout deadline) inside its operation. The
   target runs it when it next retrieves messages (or waits in a send itself: a sender that waits also answers calls for its own thread, so threads sending to each other do
   not deadlock) and the result wakes the sender. Timeout abandons the call (erased if still queued, ignored when it finishes); a thread or process that goes away fails all calls
   waiting for it (`thread_gone`, the process-exit hook). ReplyMessage completes the call early. SendNotifyMessage queues the call without waiting. Messages with a pointer in
   `lparam` sent to another process are marshalled for WM_COPYDATA (the data is copied, `lpData` points into the copy), WM_SETTEXT, WM_GETTEXT, WM_WINDOWPOSCHANGING/CHANGED,
   WM_GETMINMAXINFO, WM_NCCALCSIZE, WM_STYLECHANGING/CHANGED; others pass the scalars only. The payload is limited to 4096 bytes.
5. **Messages of the system** (activation caused by a click, the notifications of `inject_*`) are queued as upcalls nobody waits for; the window's thread handles them first when it
   next looks at its queue.
6. MsgWaitForMultipleObjectsEx with handles: the wrapper polls the objects (NtWaitForMultipleObjects with no wait) between slices of at most 10 ms of the kernel's wait for input.

Window procedures are called as stored (class `lpfnWndProc` / `SetWindowLongPtr`); the ANSI/Unicode text conversion of Wine's WINPROC handles is not done (flag 4 tells the
wrapper the window is ANSI). A window with procedure 0 (the desktop) is never called.

## Other rules

* HWNDs are `generation << 16 | (slot + 0x20)`: a handle of a destroyed window never becomes valid again until the generation wraps. The desktop window is the root of the tree;
  message-only windows hang under a hidden child of it; z-order is the order of the sibling list (first = topmost).
* Window rectangles are kept in the parent's client coordinates (screen coordinates for top-level windows); WM_NCCALCSIZE results set the client rectangle.
* The window manager calls `user_misc::window_destroyed(hwnd)` for every window it destroys and `gdi::` for device contexts and regions. `Win32kUserFallback.cpp` has weak
  stand-ins for those functions so the module links alone; the real modules' definitions replace them. `GetWindowLong(GWL_ID)` of a top-level window answers the menu handle the
  window keeps (`SetMenu` of the user-misc module should call `NtUserSetWindowLong(GWL_ID)`'s equivalent; if it offers `window_menu(hwnd)` the answer can be taken from there).
* Threads that end are noticed when their process ends (process-exit hook) or when a send to them finds the thread record finished; a per-thread exit hook is missing.
* Not done: WM_ERASEBKGND painting (needs a fill in the GDI contract), the visible-region update of device contexts held across moves is done on SetWindowPos only for the
  window moved, WM_GETMINMAXINFO on maximize, tooltips/hover (WM_MOUSEHOVER), foreground lock, input contexts.

## The user-mode half: user32.dll

`src/Windows/user32/window.inc` (included by `user32.c`) is the window system of user32.dll over win32u.dll. It makes the same objects kernel-mode code makes: RegisterClass(Ex), CreateWindow(Ex),
DestroyWindow, Show/Move/SetWindowPos, parent/owner/tree functions, styles and longs, text, properties, focus/activation/capture, painting entry points, the message functions (Send/Post/
Peek/Get/Dispatch/Translate, SendMessageTimeout, SendNotifyMessage, DefWindowProc, CallWindowProc), timers, enumeration (BuildHwndList), placement, defer-window-pos, key and cursor state, the
system metrics (NtUserCallOneParam). Calls follow Wine's `ntuser` interface (the codes of `NtUserCallHwnd` etc. are in the enums at the top of the file).

* Classes: the built-in classes (Button, Static, Edit, ListBox, ComboBox, ScrollBar, #32770, MDIClient, ...) are registered on first use with the styles Windows has. `controls.inc` has the
  procedures of **Static**, **Button** (push, check, 3-state, radio with groups, group box; BM_* messages, WM_COMMAND notifications, WM_CTLCOLORBTN) and **Edit** (single and multi line text model,
  selection, EM_* messages, WM_CHAR / key input, EN_* notifications, plain painting) over gdi32 (user32 imports gdi32; FillRect, FrameRect, DrawFocusRect are exports). A control's state block
  is kept in the window's private data (NtUserCallHwndParam GetPrivateData/SetPrivateData) and the control reports to the parent it was created with. `scroll.inc` has the SetScrollInfo family and the **ScrollBar** control, `listbox.inc` the **ListBox** (and "ComboLBox"), `combo.inc` the **ComboBox** (an Edit child plus a list box child or popup), `dialog.inc` the **dialog** class (#32770 = DefDlgProc), templates, DialogBox / CreateDialog,
  IsDialogMessage and the item functions, `icon.inc` carets, cursors, icons, LoadImage and DrawIconEx. Not done: menus (CreateMenu ... are stubs), the clipboard, hooks.
* Painting: InvalidateRect on a window without WS_CLIPCHILDREN also invalidates the children it covers, and UpdateWindow paints the window and its children (win32k). New child windows go below
  their siblings (a maximized child on top), as in Windows.
* ANSI: the window manager only knows Unicode; every window is a Unicode window. A functions convert what they send (`a_to_w`); a procedure given to RegisterClassA / SetWindowLongA is held behind
  one of 64 thunks (`ansi_wrap`) that converts what it receives (`w_to_a`); the A getters answer the program's own procedure. Converted: WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH, WM_CREATE /
  WM_NCCREATE, the character messages. `IsWindowUnicode` is "the procedure is not a thunk". Not done: Wine's WINPROC handles (GetWindowLongPtrA does not return a handle for an A procedure), the
  other messages with strings (WM_SETTINGCHANGE, list/combo/edit messages, ...).
* The console window (kernel32's GetConsoleWindow) is the one window that is not win32k's: IsWindow, GetClientRect and GetWindowRect still answer for it by themselves.
* Tests: `tests/windows/user32test.c` (build with tests/windows/build.sh, copy to `build/winetests/user32_tcos.exe`, `scripts/winetest.sh user32 tcos`), Wine's `user32` win/class/msg suites.
