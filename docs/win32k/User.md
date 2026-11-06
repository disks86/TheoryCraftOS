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
  Get/SetKeyboardState), cursor position and ClipCursor, hot keys, caret, TrackMouseEvent (WM_MOUSELEAVE), SendInput; raw input (each event, also SendInput's, goes to
  `user_misc::raw_input_event` as RAWMOUSE / RAWKEYBOARD, which posts WM_INPUT; RIDEV_NOLEGACY drops the legacy messages); `user::inject_*` for the machine's keyboard/mouse (hot keys,
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
* **Redirection** (`Win32kUserRedirect.cpp`, on when the kernel's desktop attaches its bridge): each visible top-level window of the desktop draws, with its tree, on a
  GDI window surface of its own covering the inside of the desktop's frame (`surface_rect_screen`; created at the first show, resized with the window, dropped on destroy).
  `visible_region` stops at the top-level (top-levels do not clip one another), `invalidate_screen_rect` stays inside the changed window's tree, and a move that keeps
  the size, frame and visibility (`composited_move`) invalidates nothing. A top-level with a caption has the desktop's frame sizes as its non-client margins and maximizes
  to the work area exactly. The observer (`IWindowObserver::top_level_changed`) hears of creation, show/hide, move/size, title, style, activation and destruction.
  The desktop's side: `top_level_info`, `top_levels`, `move_top_level` (posts `WM_SYSCOMMAND` 0xF1F0 with the rectangle packed in lParam: DefWindowProc runs SetWindowPos
  in the program's thread, leaving out an unchanged size or position), `activate_top_level` (0: no foreground window), `command_top_level` (SC_*),
  `present_to_window_locked` (a GPU flip into the client area). The settings live outside `State` (`redirect_config()`): the desktop sets them before the state exists.
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
  IsDialogMessage and the item functions, `icon.inc` carets, cursors, icons, LoadImage and DrawIconEx, `clipboard.inc` the clipboard (see UserMisc.md), `hooks.inc` hooks and WinEvents, `menu.inc` and
  `menuui.inc` menus.
* Hooks (`hooks.inc`): SetWindowsHookEx/UnhookWindowsHookEx/CallNextHookEx (the handle is ignored, as in Windows: the hook procedure being run is the current one), SetWindowsHook, CallMsgFilter,
  SetWinEventHook/NotifyWinEvent. win32k keeps the hooks and calls them for CallNextHookEx and CallMsgFilter only, so user32 raises the events that happen in user mode, for the hooks of its own
  process: WH_GETMESSAGE / WH_KEYBOARD / WH_MOUSE for a message GetMessage or PeekMessage returns (a nonzero result of the keyboard and mouse hooks discards a removed message), WH_CALLWNDPROC and
  WH_CALLWNDPROCRET for SendMessage and DispatchMessage (a hook that sends a message does not raise them again), WH_CBT for DestroyWindow, SetFocus, SetActiveWindow, ShowWindow (min/max/restore)
  with the veto of Windows. The start of a chain is `NtUserCallNextHookEx((1 << 32) | (id + 1), ...)`. Counters per hook id keep the no-hook case free of system calls. Not done: HCBT_CREATEWND,
  HCBT_MOVESIZE / SYSCOMMAND / CLICKSKIPPED, WH_SHELL / FOREGROUNDIDLE / DEBUG, hooks in other processes, WH_CALLWNDPROC for messages sent by another thread.
* Scrolling (`scrollwin.inc`): ScrollDC, ScrollWindowEx and ScrollWindow copy the pixels in the window's device context and invalidate the uncovered strip (up to four rectangles, as the update
  region or rectangle too); SW_SCROLLCHILDREN moves the child windows; the update region the window already had is not moved along. GetScrollBarInfo answers for OBJID_VSCROLL / HSCROLL (the strip
  beside or below the client area; arrow size, thumb) and for a scroll bar control (OBJID_CLIENT).
* Dialogs: DialogBox waits like Windows: the owner is disabled (a child window given as owner stands for its nearest ancestor that is not a child), WM_ENTERIDLE (MSGF_DIALOGBOX) is sent to it once whenever no input is waiting, EndDialog (not the end of the loop) enables the owner or, for a child dialog, its current parent, and WM_QUIT ends the loop with result 1 and is posted again. Layered windows (`scrollwin.inc`): Set/GetLayeredWindowAttributes and UpdateLayeredWindow keep their state in window properties; nothing is blended.
* MDI (`mdi.inc`): the "MDIClient" class (WM_MDICREATE / DESTROY / ACTIVATE / RESTORE / NEXT / MAXIMIZE / TILE / CASCADE / ICONARRANGE / GETACTIVE / SETMENU / REFRESHMENU), DefFrameProc,
  DefMDIChildProc, CreateMDIWindow, CreateWindowEx with WS_EX_MDICHILD under a client (the child gets the MDICREATESTRUCT, the program's parameter is its lParam), TileWindows, CascadeWindows,
  ArrangeIconicWindows. The client's state block is in its private data; children get ids from CLIENTCREATESTRUCT.idFirstChild; the window menu lists them ("&1 title", the active one
  checked); a maximized child (or a WM_SIZE with SIZE_MAXIMIZED sent to it) puts the system icon and the minimize / restore / close buttons (magic bitmaps, drawn as letters) in the frame's
  menu bar. Not done: MDIS_ALLCHILDSTYLES style rules of Windows, the "More windows" dialog, a maximized child's caption, TranslateMDISysAccel, WM_MDICREATE sent with the A structure from
  another process.
* Menus: `menu.inc` is the data half over win32k's HMENU objects: AppendMenu/InsertMenu/ModifyMenu/ChangeMenu (MF_ flags turned into MENUITEMINFO; MF_DEFAULT is not taken; an item without a
  string is a separator, which is grayed; a bad submenu handle is kept and the error set), the MENUITEMINFO calls, GetMenuString, CheckMenuItem/RadioItem, EnableMenuItem, defaults, bitmaps, MENUINFO,
  SetMenu/GetMenu (SetMenu changes the client area), GetSystemMenu, LoadMenu/LoadMenuIndirect from resources (old and extended templates). `menuui.inc` is the interface half: layout (a column or
  several for MFT_MENUBREAK, a bar), drawing (text with mnemonics, accelerators after the tab, check marks and bullets, bitmaps, submenu arrows, owner-draw through WM_MEASUREITEM/WM_DRAWITEM),
  the popup window of class "#32768", TrackPopupMenu(Ex) and the modal loop of the menu bar with the keyboard (arrows, Return, Escape, mnemonics, WM_MENUCHAR) and the mouse, EndMenu,
  GetMenuItemRect/MenuItemFromPoint/GetMenuBarInfo, and what DefWindowProc adds (WM_NCPAINT draws the bar, WM_NCHITTEST gives HTMENU, Alt / F10 / Alt+letter / Alt+Space send SC_KEYMENU, a click on
  the bar, the right button on the caption opens the system menu, WM_CANCELMODE ends a menu). Order of messages: WM_ENTERMENULOOP, WM_INITMENU, WM_INITMENUPOPUP, WM_MENUSELECT, WM_ENTERIDLE (once
  when no input is waiting), WM_UNINITMENUPOPUP, WM_EXITMENULOOP, then the command is posted (WM_COMMAND, WM_SYSCOMMAND, WM_MENUCOMMAND for MNS_NOTIFYBYPOS) or returned (TPM_RETURNCMD).
  win32k keeps the menu of a window (SetMenu sets the window record, CreateWindowEx attaches it) and destroys it with the window. Not done: scroll arrows for menus taller than the screen, the delay
  before a submenu opens, multi-row bars, MDI system buttons, the default item in bold, themes, WM_MENURBUTTONUP / WM_MENUDRAG, the exact pixel metrics of Windows.
* Painting: InvalidateRect on a window without WS_CLIPCHILDREN also invalidates the children it covers, and UpdateWindow paints the window and its children (win32k). New child windows go below
  their siblings (a maximized child on top), as in Windows.
* ANSI: the window manager only knows Unicode; every window is a Unicode window. A functions convert what they send (`a_to_w`); a procedure given to RegisterClassA / SetWindowLongA is held behind
  one of 64 thunks (`ansi_wrap`) that converts what it receives (`w_to_a`); the A getters answer the program's own procedure. Converted: WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH, WM_CREATE /
  WM_NCCREATE, the character messages. `IsWindowUnicode` is "the procedure is not a thunk". Not done: Wine's WINPROC handles (GetWindowLongPtrA does not return a handle for an A procedure), the
  other messages with strings (WM_SETTINGCHANGE, list/combo/edit messages, ...).
* The console window (kernel32's GetConsoleWindow) is the one window that is not win32k's: IsWindow, GetClientRect and GetWindowRect still answer for it by themselves.
* Tests: `tests/windows/user32test.c` (build with tests/windows/build.sh, copy to `build/winetests/user32_tcos.exe`, `scripts/winetest.sh user32 tcos`), Wine's `user32` win/class/msg suites.
