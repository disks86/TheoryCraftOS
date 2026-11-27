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
/* The display: its modes (the window manager's, through user32's EnumDisplaySettings/ChangeDisplaySettingsEx; the kernel lists every size at 32, 16
 * and 8 bits and scans out 32 bits whatever the mode's depth), the cooperative level with its windows (the focus window, which ddraw watches for
 * WM_ACTIVATEAPP in exclusive mode, and the device window), the vertical blank and scan line (D3DKMT), the caps (HAL and HEL) and the device
 * identifier. Included by ddraw.c. */

typedef struct DisplayMode { DWORD width, height, bpp, refresh; } DisplayMode;

static BOOL display_current(DisplayMode *m) {
  DEVMODEW dm;
  zero(&dm, sizeof dm);
  dm.dmSize = sizeof dm;
  if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
    m->width = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    m->height = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    m->bpp = 32;
    m->refresh = 60;
    return FALSE;
  }
  m->width = dm.dmPelsWidth;
  m->height = dm.dmPelsHeight;
  m->bpp = dm.dmBitsPerPel ? dm.dmBitsPerPel : 32;
  m->refresh = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
  return TRUE;
}
/* The `index`th mode the display lists (FALSE: no more). */
static BOOL display_mode_at(DWORD index, DisplayMode *m) {
  DEVMODEW dm;
  zero(&dm, sizeof dm);
  dm.dmSize = sizeof dm;
  if (!EnumDisplaySettingsW(NULL, index, &dm)) return FALSE;
  m->width = dm.dmPelsWidth;
  m->height = dm.dmPelsHeight;
  m->bpp = dm.dmBitsPerPel;
  m->refresh = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
  return TRUE;
}
static BOOL display_has_mode(DWORD w, DWORD h, DWORD bpp, DWORD refresh) {
  DisplayMode m;
  for (DWORD i = 0; display_mode_at(i, &m); ++i)
    if (m.width == w && m.height == h && m.bpp == bpp && (!refresh || m.refresh == refresh)) return TRUE;
  return FALSE;
}
static HRESULT display_set(DWORD w, DWORD h, DWORD bpp, DWORD refresh) {
  DEVMODEW dm;
  zero(&dm, sizeof dm);
  dm.dmSize = sizeof dm;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
  dm.dmPelsWidth = w;
  dm.dmPelsHeight = h;
  dm.dmBitsPerPel = bpp == 15 ? 16 : bpp;
  if (refresh) { dm.dmFields |= DM_DISPLAYFREQUENCY; dm.dmDisplayFrequency = refresh; }
  const LONG r = ChangeDisplaySettingsExW(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
  if (r == DISP_CHANGE_SUCCESSFUL) return DD_OK;
  return r == DISP_CHANGE_BADMODE ? DDERR_INVALIDMODE : DDERR_UNSUPPORTEDMODE;
}
static HRESULT display_restore(void) {
  return ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL) == DISP_CHANGE_SUCCESSFUL ? DD_OK : DDERR_UNSUPPORTEDMODE;
}

/* ---- lost surfaces ---------------------------------------------------------------------------------------------------------------------------------
 * Surfaces in video memory (and the primary surface and z buffers wherever they are) are lost when the display mode changes or the program loses
 * exclusive mode. The object's state goes NOT_RESTORED then; the next look at a surface marks every surface that can be lost as lost (Restore brings
 * each back). A mode change made by anyone (ChangeDisplaySettings too) counts: the mode the object last saw is kept. */
static BOOL surface_can_be_lost(const Surface *s) {
  const DWORD caps = s->desc.ddsCaps.dwCaps;
  if (!(caps & DDSCAPS_SYSTEMMEMORY) || (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_ZBUFFER))) return TRUE;
  return s->sysmem_fallback;
}
static void ddraw_lost_mode_check(DDraw *dd);
static void ddraw_update_lost_surfaces(DDraw *dd) {
  ddraw_lost_mode_check(dd);
  if (dd->device_state == DD_STATE_LOST) {
    const HWND w = dd->focus_window ? dd->focus_window : dd->dest_window;
    if (w && GetForegroundWindow() == w) dd->device_state = DD_STATE_NOT_RESTORED;
  }
  if (dd->device_state != DD_STATE_NOT_RESTORED) return;
  for (Surface *s = dd->surfaces; s; s = s->list_next) s->lost = surface_can_be_lost(s);
  dd->device_state = DD_STATE_OK;
}
static BOOL surface_is_lost(Surface *s) {
  if (!s->dd || (s->dd->flags & DD_DEAD)) return FALSE;
  ddraw_lost_mode_check(s->dd);
  return surface_can_be_lost(s) && (s->dd->device_state != DD_STATE_OK || s->lost);
}

/* The modes each object saw last: a change from elsewhere makes its surfaces lost. */
static DisplayMode g_seen_mode;
static BOOL g_seen_valid;
static void ddraw_lost_mode_check(DDraw *dd) {
  (void)dd;
  DisplayMode now;
  display_current(&now);
  if (g_seen_valid && (now.width != g_seen_mode.width || now.height != g_seen_mode.height || now.bpp != g_seen_mode.bpp)) {
    for (DDraw *o = g_ddraws; o; o = o->next)
      if (!(o->flags & DD_DEAD) && o->device_state == DD_STATE_OK) o->device_state = DD_STATE_NOT_RESTORED;
  }
  g_seen_mode = now;
  g_seen_valid = TRUE;
}

/* ---- the focus window ------------------------------------------------------------------------------------------------------------------------------
 * In exclusive mode ddraw puts its own window procedure in front of the focus window's: when the program loses the foreground (WM_ACTIVATEAPP
 * FALSE) the window is minimized, the display mode goes back to the desktop's and every surface is lost; when it comes back the mode the program set
 * is put back and the surfaces can be restored. */
static DisplayMode g_program_mode;         /* the mode SetDisplayMode set (to put back on activation) */
static BOOL g_program_mode_set;

/* The windows ddraw's procedure is in: the program's own procedure, and the object whose focus window it is now (NULL: none - a window whose
 * procedure the program changed while ddraw's was in keeps its entry, and a later exclusive mode takes it up again without touching the procedure,
 * as Windows does). */
typedef struct HookedWindow { HWND window; WNDPROC saved; BOOL unicode; DDraw *owner; } HookedWindow;
#define MAX_HOOKED 16
static HookedWindow g_hooked[MAX_HOOKED];

static HookedWindow *hooked_find(HWND w) {
  for (UINT i = 0; i < MAX_HOOKED; ++i) if (g_hooked[i].window == w && w) return &g_hooked[i];
  return NULL;
}
static void exclusive_window_fill_screen(DDraw *dd, HWND w, BOOL show, BOOL topmost);
/* Windows checks every 1.5 seconds that the window of exclusive mode is still topmost while the program is in the foreground, and makes it topmost
 * again if not (Wine's test_window_style): a timer of the focus window's (its WM_TIMER does not reach the program). */
#define DD_TOPMOST_TIMER ((UINT_PTR)0x6464AA01)
static void window_set_pos_later(HWND w);
static void topmost_check(DDraw *dd) {
  const HWND w = dd->dest_window;
  if (!(dd->coop & DDSCL_EXCLUSIVE) || (dd->coop & DDSCL_NOWINDOWCHANGES) || !w || !IsWindow(w)) return;
  DWORD pid = 0;
  const HWND fg = GetForegroundWindow();
  if (!fg || (GetWindowThreadProcessId(fg, &pid), pid) != GetCurrentProcessId()) return;
  if (!(GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) window_set_pos_later(w);
}
static LRESULT CALLBACK focus_window_proc_w(HWND w, UINT m, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK focus_window_proc_a(HWND w, UINT m, WPARAM wp, LPARAM lp);

static LRESULT focus_window_message(HWND w, UINT message, WPARAM wparam, LPARAM lparam, BOOL unicode) {
  dd_lock();
  HookedWindow *h = hooked_find(w);
  WNDPROC proc = h ? h->saved : NULL;
  DDraw *dd = h && h->owner && !(h->owner->flags & DD_DEAD) ? h->owner : NULL;
  if (dd && message == WM_ACTIVATEAPP) {
    if (!wparam) {
      /* (the desktop's mode first - the program hears WM_DISPLAYCHANGE while the device is still fine -, then the loss, then the window minimized,
       * and only then the program's WM_ACTIVATEAPP: Windows' order, Wine's test_coop_level_mode_set) */
      if (dd->flags & DD_RESTORE_MODE) display_restore();
      dd->device_state = DD_STATE_LOST;
      g_exclusive_window = NULL;
      g_seen_valid = FALSE;
      if (!(dd->coop & DDSCL_NOWINDOWCHANGES) && IsWindowVisible(w)) ShowWindow(w, SW_MINIMIZE);
    } else {
      if ((dd->flags & DD_RESTORE_MODE) && g_program_mode_set) display_set(g_program_mode.width, g_program_mode.height, g_program_mode.bpp, 0);
      g_seen_valid = FALSE;
      if (!(dd->coop & DDSCL_NOWINDOWCHANGES)) exclusive_window_fill_screen(dd, dd->dest_window, FALSE, FALSE);
      if (dd->device_state == DD_STATE_LOST) dd->device_state = DD_STATE_NOT_RESTORED;
    }
  } else if (message == WM_TIMER && wparam == DD_TOPMOST_TIMER) {
    if (dd) topmost_check(dd);
    dd_unlock();
    return 0;
  } else if (dd && message == WM_DISPLAYCHANGE) {
    if ((dd->coop & DDSCL_EXCLUSIVE) && dd->dest_window) exclusive_window_fill_screen(dd, dd->dest_window, TRUE, FALSE);
    if (dd->device_state == DD_STATE_OK) dd->device_state = DD_STATE_NOT_RESTORED;
  } else if (h && message == WM_DESTROY) {
    /* (the window goes: its own procedure back, the entry free) */
    if (proc) { if (h->unicode) SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)proc); else SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)proc); }
    if (h->owner) h->owner->hooked_window = NULL;
    h->window = NULL;
    h->owner = NULL;
  }
  dd_unlock();
  if (!proc) return unicode ? DefWindowProcW(w, message, wparam, lparam) : DefWindowProcA(w, message, wparam, lparam);
  return unicode ? CallWindowProcW(proc, w, message, wparam, lparam) : CallWindowProcA(proc, w, message, wparam, lparam);
}
/* (the procedure is put in as the window's own kind - ANSI or Unicode - so that the program reads back its own procedure as it gave it) */
static LRESULT CALLBACK focus_window_proc_w(HWND w, UINT m, WPARAM wp, LPARAM lp) { return focus_window_message(w, m, wp, lp, TRUE); }
static LRESULT CALLBACK focus_window_proc_a(HWND w, UINT m, WPARAM wp, LPARAM lp) { return focus_window_message(w, m, wp, lp, FALSE); }

static void focus_window_hook(DDraw *dd, HWND w) {
  if (dd->hooked_window == w) return;
  HookedWindow *old = dd->hooked_window ? hooked_find(dd->hooked_window) : NULL;
  if (old) {
    /* (the program's procedure comes back only if ddraw's is still the window's: a program that put in another keeps it) */
    const HWND hw = old->window;
    BOOL ours;
    if (old->unicode) ours = (WNDPROC)GetWindowLongPtrW(hw, GWLP_WNDPROC) == focus_window_proc_w;
    else ours = (WNDPROC)GetWindowLongPtrA(hw, GWLP_WNDPROC) == focus_window_proc_a;
    if (IsWindow(hw) && GetWindowThreadProcessId(hw, NULL) == GetCurrentThreadId()) KillTimer(hw, DD_TOPMOST_TIMER);
    if (ours && IsWindow(hw)) {
      if (old->unicode) SetWindowLongPtrW(hw, GWLP_WNDPROC, (LONG_PTR)old->saved); else SetWindowLongPtrA(hw, GWLP_WNDPROC, (LONG_PTR)old->saved);
      old->window = NULL;
    }
    old->owner = NULL;
  }
  dd->hooked_window = NULL;
  if (!w || !IsWindow(w)) return;
  /* (only a window of this process can be hooked) */
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (pid != GetCurrentProcessId()) return;
  HookedWindow *h = hooked_find(w);
  if (!h) {
    for (UINT i = 0; i < MAX_HOOKED && !h; ++i) if (!g_hooked[i].window) h = &g_hooked[i];
    if (!h) return;
    h->window = w;
    h->unicode = IsWindowUnicode(w);
    if (h->unicode) h->saved = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)focus_window_proc_w);
    else h->saved = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)focus_window_proc_a);
  }
  h->owner = dd;
  dd->hooked_window = w;
  /* (a timer can only be set on a window of this thread) */
  if (GetWindowThreadProcessId(w, NULL) == GetCurrentThreadId()) SetTimer(w, DD_TOPMOST_TIMER, 1500, NULL);
}
static void focus_window_unhook(DDraw *dd) { focus_window_hook(dd, NULL); }

/* SetWindowPos of a window another thread owns is done by a thread of its own: the window's messages go to its thread, which may be waiting for this
 * one (Wine's tests set the cooperative level of a window of another thread that pumps its messages only now and then). */
typedef struct WindowPos { HWND window, after; int x, y, w, h; UINT flags; } WindowPos;
static DWORD WINAPI window_pos_thread(void *p) {
  WindowPos *wp = p;
  SetWindowPos(wp->window, wp->after, wp->x, wp->y, wp->w, wp->h, wp->flags);
  mem_free(wp);
  return 0;
}
static void window_set_pos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags) {
  if (GetWindowThreadProcessId(w, NULL) == GetCurrentThreadId()) { SetWindowPos(w, after, x, y, cx, cy, flags); return; }
  WindowPos *wp = mem_alloc(sizeof *wp);
  if (!wp) return;
  wp->window = w; wp->after = after; wp->x = x; wp->y = y; wp->w = cx; wp->h = cy; wp->flags = flags;
  HANDLE t = CreateThread(NULL, 0, window_pos_thread, wp, 0, NULL);
  if (t) CloseHandle(t); else mem_free(wp);
}

/* Makes a window topmost again without moving, sizing, showing or activating it. */
static void window_set_pos_later(HWND w) { window_set_pos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }

/* The window of exclusive full-screen mode covers the screen (shown and on top when the program is in the foreground, as Windows does). */
static void exclusive_window_fill_screen(DDraw *dd, HWND w, BOOL show, BOOL topmost) {
  (void)dd;
  if (!w || !IsWindow(w)) return;
  DisplayMode m;
  display_current(&m);
  UINT flags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
  HWND after = NULL;
  if (topmost) after = HWND_TOPMOST; else flags |= SWP_NOZORDER;
  if (show) flags |= SWP_SHOWWINDOW;
  window_set_pos(w, after, 0, 0, (int)m.width, (int)m.height, flags);
}

/* Exclusive mode brings the program to the foreground (Wine's tests record Windows: the program gets WM_ACTIVATEAPP if it was not there, and
 * GetForegroundWindow is the window after). A visible window then covers the screen on top; a hidden one stays hidden and in its place. Nothing when
 * the window (or the focus window) is the foreground already. */
static void exclusive_activate_now(DDraw *dd, HWND window);
static BOOL g_activating;      /* (a program that sets the level again from its WM_ACTIVATEAPP is not activated again) */
static void exclusive_activate(DDraw *dd, HWND window) {
  if (g_activating) return;
  g_activating = TRUE;
  exclusive_activate_now(dd, window);
  g_activating = FALSE;
}
static void exclusive_activate_now(DDraw *dd, HWND window) {
  const HWND fg = GetForegroundWindow();
  if (fg == window || (dd->focus_window && fg == dd->focus_window)) return;
  /* (the window first comes to the top of its level, which it hears as WM_WINDOWPOSCHANGING before WM_ACTIVATE - Windows' activation does that
   * z-order change itself; the window manager here moves it without the message: Wine's test_wndproc) */
  window_set_pos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  SetForegroundWindow(window);
  /* (shown over the screen, but not made topmost: only a window that was the foreground when the level was set is - Wine's test_window_style, where
   * another window had the foreground; the 1.5 s check makes it topmost later while the program stays in front) */
  if (GetForegroundWindow() == window && IsWindowVisible(window)) exclusive_window_fill_screen(dd, window, TRUE, FALSE);
}

/* ---- the cooperative level -------------------------------------------------------------------------------------------------------------------------
 * Windows' rules (Wine's tests check them): one of DDSCL_NORMAL, DDSCL_EXCLUSIVE and DDSCL_SETFOCUSWINDOW; DDSCL_EXCLUSIVE needs DDSCL_FULLSCREEN and
 * a window (or DDSCL_CREATEDEVICEWINDOW with a focus window); DDSCL_SETFOCUSWINDOW alone takes no other flags except DDSCL_NOWINDOWCHANGES and
 * DDSCL_CREATEDEVICEWINDOW; DDSCL_CREATEDEVICEWINDOW needs DDSCL_EXCLUSIVE; a second object asking for exclusive mode while another has it gets
 * DDERR_EXCLUSIVEMODEALREADYSET. Leaving exclusive mode takes the window off the top (unless the call says DDSCL_NOWINDOWCHANGES) and, but for
 * IDirectDraw's SetCooperativeLevel, puts the display mode back. */
static const char kDeviceWindowClass[] = "DirectDrawDeviceWnd";
static BOOL g_device_class_registered;
static LRESULT CALLBACK device_window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }
static HWND device_window_create(void) {
  if (!g_device_class_registered) {
    WNDCLASSA wc;
    zero(&wc, sizeof wc);
    wc.lpfnWndProc = device_window_proc;
    wc.hInstance = g_module;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = kDeviceWindowClass;
    if (RegisterClassA(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) g_device_class_registered = TRUE;
  }
  HWND w = CreateWindowExA(0, kDeviceWindowClass, "DirectDrawDeviceWnd", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                           NULL, NULL, g_module, NULL);
  if (w) ShowWindow(w, SW_SHOW);
  return w;
}

static HRESULT ddraw_restore_display_mode(DDraw *dd);
static void primary_follow_mode(DDraw *dd);
static void present_primary_all(Surface *primary);

static HRESULT ddraw_set_cooperative_level(DDraw *dd, HWND window, DWORD flags, BOOL restore_mode_on_normal) {
  if (dd->flags & DD_SCL_RECURSIVE) return DD_OK;
  if (window && !IsWindow(window)) return DDERR_INVALIDPARAMS;
  /* (exclusive mode on the desktop window is taken, as Windows - Wine's refcount test -; ddraw does not touch that window: no window changes) */
  if (window && window == GetDesktopWindow() && (flags & DDSCL_EXCLUSIVE)) flags |= DDSCL_NOWINDOWCHANGES;
  if (!(flags & (DDSCL_SETFOCUSWINDOW | DDSCL_NORMAL | DDSCL_EXCLUSIVE))) return DDERR_INVALIDPARAMS;
  /* (DDSCL_NORMAL with DDSCL_EXCLUSIVE: exclusive mode, as Windows; without DDSCL_FULLSCREEN or a window it fails as exclusive mode does) */
  if ((flags & DDSCL_CREATEDEVICEWINDOW) && !(flags & DDSCL_EXCLUSIVE)) return DDERR_INVALIDPARAMS;
  HRESULT hr = DD_OK;
  dd->flags |= DD_SCL_RECURSIVE;
  if ((flags & DDSCL_SETFOCUSWINDOW) && !(flags & DDSCL_CREATEDEVICEWINDOW)) {
    if (flags & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE | DDSCL_ALLOWREBOOT | DDSCL_ALLOWMODEX | DDSCL_SETDEVICEWINDOW | DDSCL_NORMAL |
                 DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) { hr = DDERR_INVALIDPARAMS; goto done; }
    if ((dd->coop & DDSCL_EXCLUSIVE) && dd->dest_window) { hr = DDERR_HWNDALREADYSET; goto done; }
    dd->focus_window = window;
    goto done;
  }
  if (flags & DDSCL_EXCLUSIVE) {
    if (!(flags & DDSCL_FULLSCREEN) || !(window || (flags & DDSCL_CREATEDEVICEWINDOW))) { hr = DDERR_INVALIDPARAMS; goto done; }
    if (g_exclusive && g_exclusive != dd) { hr = DDERR_EXCLUSIVEMODEALREADYSET; goto done; }
    if (flags & DDSCL_CREATEDEVICEWINDOW) {
      if (!dd->focus_window && !(flags & DDSCL_SETFOCUSWINDOW)) { hr = DDERR_NOFOCUSWINDOW; goto done; }
      HWND device = device_window_create();
      if (!device) { hr = E_FAIL; goto done; }
      if (dd->device_window && dd->device_window != device) DestroyWindow(dd->device_window);
      dd->coop |= DDSCL_CREATEDEVICEWINDOW;
      dd->device_window = device;
      if (flags & DDSCL_SETFOCUSWINDOW) {
        if (!window) { hr = DDERR_NOHWND; goto done; }
        if ((dd->coop & DDSCL_EXCLUSIVE) && dd->dest_window) { hr = DDERR_HWNDALREADYSET; goto done; }
        dd->focus_window = window;
      }
      window = device;
    }
  } else {
    if ((dd->coop & DDSCL_CREATEDEVICEWINDOW) && dd->device_window) DestroyWindow(dd->device_window);
    dd->device_window = NULL;
    dd->focus_window = NULL;
  }
  if ((flags & DDSCL_EXCLUSIVE) && g_exclusive_window != window) {
    dd->device_state = DD_STATE_NOT_RESTORED;
    g_exclusive_window = window;
  }
  if (!(flags & DDSCL_EXCLUSIVE) && (dd->coop & DDSCL_EXCLUSIVE)) {
    /* leaving exclusive mode: the window off the top, the mode back */
    if (!(flags & DDSCL_NOWINDOWCHANGES) && window && IsWindow(window) && window != GetDesktopWindow()) window_set_pos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (restore_mode_on_normal) ddraw_restore_display_mode(dd);
    /* (IDirectDraw2 and later lose the surfaces that can be lost when exclusive mode goes to DDSCL_NORMAL, not to DDSCL_NORMAL | DDSCL_FULLSCREEN;
     * IDirectDraw's SetCooperativeLevel keeps them - Wine's test_lost_device of each version. The object itself stays fine: TestCooperativeLevel
     * answers DD_OK, a new surface is not lost.) */
    if (restore_mode_on_normal && !(flags & DDSCL_FULLSCREEN)) {
      ddraw_update_lost_surfaces(dd);
      for (Surface *s = dd->surfaces; s; s = s->list_next)
        if (surface_can_be_lost(s)) s->lost = TRUE;
    }
    ClipCursor(NULL);
    focus_window_unhook(dd);
    /* (going exclusive again later loses the surfaces, as Windows) */
    if (g_exclusive_window == dd->dest_window) g_exclusive_window = NULL;
  }
  if (flags & DDSCL_EXCLUSIVE) {
    const HWND hook = dd->focus_window ? dd->focus_window : window;
    focus_window_hook(dd, hook);
    if (!(flags & DDSCL_NOWINDOWCHANGES)) {
      const HWND fg = GetForegroundWindow();
      const BOOL foreground = fg == window || (dd->focus_window && fg == dd->focus_window);
      exclusive_window_fill_screen(dd, window, foreground || GetActiveWindow() == window, foreground);
    }
    DisplayMode m;
    display_current(&m);
    RECT clip;
    rect_set(&clip, 0, 0, (LONG)m.width, (LONG)m.height);
    ClipCursor(&clip);
    g_exclusive = dd;
  } else if (g_exclusive == dd) {
    g_exclusive = NULL;
  }
  dd->coop = flags | (dd->coop & DDSCL_CREATEDEVICEWINDOW & flags);
  if (flags & DDSCL_CREATEDEVICEWINDOW) dd->coop |= DDSCL_CREATEDEVICEWINDOW;
  dd->dest_window = window;
  if ((flags & DDSCL_EXCLUSIVE) && !(flags & DDSCL_NOWINDOWCHANGES) && window && IsWindow(window)) exclusive_activate(dd, window);
done:
  dd->flags &= ~DD_SCL_RECURSIVE;
  return hr;
}

/* ---- display modes ---------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT ddraw_restore_display_mode(DDraw *dd) {
  if (!(dd->flags & DD_RESTORE_MODE)) return DD_OK;
  if (g_exclusive && g_exclusive != dd) return DDERR_NOEXCLUSIVEMODE;
  HRESULT hr = display_restore();
  if (SUCCEEDED(hr)) primary_follow_mode(dd);
  if (SUCCEEDED(hr)) {
    dd->flags &= ~DD_RESTORE_MODE;
    g_program_mode_set = FALSE;
    if (dd->coop & DDSCL_EXCLUSIVE) {
      DisplayMode m;
      display_current(&m);
      RECT clip;
      rect_set(&clip, 0, 0, (LONG)m.width, (LONG)m.height);
      ClipCursor(&clip);
    }
  }
  if (dd->device_state == DD_STATE_OK) dd->device_state = DD_STATE_NOT_RESTORED;
  return hr;
}

static HRESULT ddraw_set_display_mode(DDraw *dd, DWORD width, DWORD height, DWORD bpp, DWORD refresh, DWORD flags) {
  (void)flags;      /* (DDSDM_STANDARDVGAMODE: there is no mode 13h here; the mode set is the same) */
  if (g_exclusive && g_exclusive != dd) return DDERR_NOEXCLUSIVEMODE;
  if (!width || !height) return DD_OK;      /* (Windows answers DD_OK and changes nothing; some programs count on it) */
  if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return DDERR_INVALIDMODE;
  const DWORD list_bpp = bpp == 15 ? 16 : bpp;
  if (!display_has_mode(width, height, list_bpp, refresh)) return refresh && display_has_mode(width, height, list_bpp, 0) ? DDERR_INVALIDMODE : DDERR_INVALIDMODE;
  HRESULT hr = display_set(width, height, bpp, refresh);
  if (SUCCEEDED(hr)) primary_follow_mode(dd);
  if (SUCCEEDED(hr)) {
    dd->flags |= DD_RESTORE_MODE;
    g_program_mode.width = width;
    g_program_mode.height = height;
    g_program_mode.bpp = list_bpp;
    g_program_mode.refresh = refresh;
    g_program_mode_set = TRUE;
    if (dd->coop & DDSCL_EXCLUSIVE) {
      RECT clip;
      rect_set(&clip, 0, 0, (LONG)width, (LONG)height);
      ClipCursor(&clip);
      if (dd->dest_window && !(dd->coop & DDSCL_NOWINDOWCHANGES)) exclusive_window_fill_screen(dd, dd->dest_window, TRUE, FALSE);
    }
  }
  if (dd->device_state == DD_STATE_OK) dd->device_state = DD_STATE_NOT_RESTORED;
  return hr;
}

/* ---- vertical blank, scan line ----------------------------------------------------------------------------------------------------------------------- */

static BOOL kmt_display_adapter(UINT *handle) {
  KmtAdapter a[4];
  const UINT n = kmt_enum_adapters(a, 4, NULL);
  if (!n) return FALSE;
  *handle = a[0].handle;
  for (UINT i = 1; i < n; ++i) kmt_close_adapter(a[i].handle);
  return TRUE;
}
/* The beam: in the blank (TRUE) or on line *line. FALSE: the display cannot say. */
static BOOL display_scanline(DWORD *line, BOOL *blank) {
  UINT adapter;
  if (!kmt_display_adapter(&adapter)) return FALSE;
  BYTE block[16] __attribute__((aligned(8))) = {0};      /* D3DKMT_GETSCANLINE { hAdapter @0; VidPnSourceId @4; InVerticalBlank @8; ScanLine @12 } */
  KPUT32(block, 0, adapter);
  const NTSTATUS st = D3DKMTGetScanLine(block);
  kmt_close_adapter(adapter);
  if (!KMT_OK(st)) return FALSE;
  *blank = block[8] != 0;
  *line = KGET32(block, 12);
  return TRUE;
}
static BOOL display_wait_vblank(void) {
  UINT adapter;
  if (!kmt_display_adapter(&adapter)) return FALSE;
  BYTE wait[12] __attribute__((aligned(8))) = {0};      /* D3DKMT_WAITFORVERTICALBLANKEVENT { hAdapter @0; hDevice @4; VidPnSourceId @8 } */
  KPUT32(wait, 0, adapter);
  const NTSTATUS st = D3DKMTWaitForVerticalBlankEvent(wait);
  kmt_close_adapter(adapter);
  return KMT_OK(st);
}
/* The adapter's memory: dedicated video memory if it has some, else the system memory it may use. */
static UINT64 display_video_memory(void) {
  static UINT64 known;
  if (known) return known;
  UINT adapter;
  UINT64 sizes[3] = {0, 0, 0};
  if (kmt_display_adapter(&adapter)) {
    kmt_segment_sizes(adapter, sizes);
    kmt_close_adapter(adapter);
  }
  /* (what the adapter may put surfaces in: its own memory and the system memory it uses, as Windows counts a WDDM adapter's for DirectDraw) */
  UINT64 total = sizes[0] + sizes[1] + sizes[2];
  if (total < (256ull << 20)) total = 256ull << 20;
  known = total;
  return total;
}

/* ---- caps ---------------------------------------------------------------------------------------------------------------------------------------------
 * What this DirectDraw has, as a driver reports it: blits with colour fill, depth fill, stretching (any factor), mirroring and turning by 90 degrees,
 * source and destination colour keys (single colours: no ranges in hardware), clipping, blits from and to system memory, palettes of 1 .. 8 bits,
 * flipping, mipmaps, z buffers of 16, 24 and 32 bits, the primary's gamma ramp, wide surfaces, and what Direct3D adds when its part is here. No
 * overlays (no overlay hardware: DDERR_NOOVERLAYHW), no colour controls, no stereo, no video ports. The HEL (software emulation) reports what the
 * software layer of Windows does. */
static DWORD fourcc_codes(DWORD *out, DWORD capacity) {
  static const UINT fmts[] = {DD_FMT_YUY2, DD_FMT_UYVY, DD_FMT_YV12, DD_FMT_DXT1, DD_FMT_DXT2, DD_FMT_DXT3, DD_FMT_DXT4, DD_FMT_DXT5, DD_FMT_NV12};
  const DWORD n = sizeof fmts / sizeof fmts[0];
  for (DWORD i = 0; out && i < n && i < capacity; ++i) out[i] = dd_format(fmts[i])->fourcc;
  return n;
}
static void ddraw_caps(const DDraw *dd, DDCAPS *caps, BOOL hel) {
  zero(caps, sizeof *caps);
  caps->dwSize = sizeof *caps;
  /* (an object of the hardware only has no HEL, one of the emulation only no HAL) */
  if ((hel && dd->kind == DD_DRIVER_HARDWARE) || (!hel && dd->kind == DD_DRIVER_EMULATION)) return;
  const DWORD ckey = DDCKEYCAPS_DESTBLT | DDCKEYCAPS_SRCBLT;
  const DWORD fx = DDFXCAPS_BLTALPHA | DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN | DDFXCAPS_BLTROTATION90 | DDFXCAPS_BLTSHRINKX |
                   DDFXCAPS_BLTSHRINKXN | DDFXCAPS_BLTSHRINKY | DDFXCAPS_BLTSHRINKYN | DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHXN |
                   DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSTRETCHYN;
  const DWORD blit = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH | DDCAPS_CANBLTSYSMEM | DDCAPS_CANCLIP |
                     DDCAPS_CANCLIPSTRETCHED | DDCAPS_COLORKEY | DDCAPS_COLORKEYHWASSIST | DDCAPS_BLTFOURCC;
  const BOOL d3d = g_d3d.d3d7_vtbl != NULL;
  if (hel) {
    caps->dwCaps = DDCAPS_GDI | DDCAPS_PALETTE | blit | DDCAPS_ALIGNSTRIDE;
    caps->dwCaps &= ~(DWORD)DDCAPS_COLORKEYHWASSIST;
    caps->dwCaps2 = DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_WIDESURFACES | DDCAPS2_CANRENDERWINDOWED;
    caps->ddsCaps.dwCaps = DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
    caps->dwZBufferBitDepths = DDBD_16;
  } else {
    caps->dwCaps = DDCAPS_GDI | DDCAPS_PALETTE | blit | DDCAPS_ALIGNSTRIDE;
    caps->dwCaps2 = DDCAPS2_CERTIFIED | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_PRIMARYGAMMA | DDCAPS2_WIDESURFACES | DDCAPS2_CANRENDERWINDOWED |
                    DDCAPS2_FLIPINTERVAL | DDCAPS2_FLIPNOVSYNC | DDCAPS2_COPYFOURCC;
    caps->ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE |
                           DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_VISIBLE | DDSCAPS_ZBUFFER |
                           DDSCAPS_MIPMAP | DDSCAPS_LOCALVIDMEM | (d3d ? DDSCAPS_3DDEVICE : 0);
    caps->dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    const UINT64 mem = display_video_memory();
    DisplayMode m;
    display_current(&m);
    const UINT64 fb = (UINT64)m.width * m.height * ((m.bpp + 7) / 8);
    const UINT64 total = mem > fb ? mem - fb : 0;
    caps->dwVidMemTotal = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)total;
    caps->dwVidMemFree = caps->dwVidMemTotal;
    caps->dwNumFourCCCodes = fourcc_codes(NULL, 0);
  }
  caps->dwCKeyCaps = ckey;
  caps->dwFXCaps = fx;
  caps->dwPalCaps = DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT | DDPCAPS_8BITENTRIES | DDPCAPS_ALLOW256 | DDPCAPS_PRIMARYSURFACE |
                    DDPCAPS_ALPHA | DDPCAPS_VSYNC;
  caps->dwAlignStrideAlign = 8;
  caps->dwMaxVisibleOverlays = 0;
  caps->dwSVBCaps = caps->dwVSBCaps = caps->dwSSBCaps = blit;
  caps->dwSVBCKeyCaps = caps->dwVSBCKeyCaps = caps->dwSSBCKeyCaps = ckey;
  caps->dwSVBFXCaps = caps->dwVSBFXCaps = caps->dwSSBFXCaps = fx;
  caps->dwNLVBCaps = blit;
  caps->dwNLVBCKeyCaps = ckey;
  caps->dwNLVBFXCaps = fx;
  /* (every ROP a blit takes: SRCCOPY, BLACKNESS and WHITENESS - dwRops is a bit per ROP index, the high byte of the ROP code) */
  caps->dwRops[0xCC / 32] |= 1u << (0xCC % 32);
  caps->dwRops[0x00 / 32] |= 1u << (0x00 % 32);
  caps->dwRops[0xFF / 32] |= 1u << (0xFF % 32);
  caps->ddsOldCaps.dwCaps = caps->ddsCaps.dwCaps;
  caps->ddsCaps.dwCaps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_MIPMAPSUBLEVEL | DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE;
  if (hel) caps->ddsCaps.dwCaps2 = 0;
}

/* The device identifier (DDDEVICEIDENTIFIER2): the display adapter as Direct3D 9 names it (d3d9.dll's GetAdapterIdentifier: the same adapter, the
 * same vendor, device and identifier). */
static void ddraw_identifier(DDDEVICEIDENTIFIER2 *id) {
  zero(id, sizeof *id);
  static const char driver[] = "ddraw.dll";
  static const char description[] = "TheoryCraftOS Direct3D adapter";
  memcpy(id->szDriver, driver, sizeof driver);
  memcpy(id->szDescription, description, sizeof description);
  id->liDriverVersion.QuadPart = 0x000A000000000000ll;
  id->dwVendorId = 0x1414;      /* (Microsoft's, as its software adapters have) */
  id->dwDeviceId = 0x008C;
  static const GUID identifier = {0x7c0e5a10, 0x2d5e, 0x4a5d, {0x9a, 0x7e, 0x54, 0x43, 0x4f, 0x53, 0x09, 0x09}};
  id->guidDeviceIdentifier = identifier;
  id->dwWHQLLevel = 1;
}
