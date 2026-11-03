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

/* OleCreatePropertyFrame / OleCreatePropertyFrameIndirect: a modal window with one button per property page (the pages are created with CoCreateInstance, found in ole32
 * when the frame is first used), the page area, and OK / Cancel / Apply. The pages draw themselves in the area through IPropertyPage::Activate. */

SO_IMP("user32.dll", ATOM, RegisterClassW, (const WNDCLASSW *wc), (wc))
SO_IMP("user32.dll", HWND, CreateWindowExW, (DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int cx, int cy, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param), (ex, cls, name, style, x, y, cx, cy, parent, menu, inst, param))
SO_IMP("user32.dll", LRESULT, DefWindowProcW, (HWND w, UINT m, WPARAM wp, LPARAM lp), (w, m, wp, lp))
SO_IMP("user32.dll", BOOL, DestroyWindow, (HWND w), (w))
SO_IMP("user32.dll", BOOL, ShowWindow, (HWND w, int cmd), (w, cmd))
SO_IMP("user32.dll", BOOL, EnableWindow, (HWND w, BOOL enable), (w, enable))
SO_IMP("user32.dll", BOOL, GetMessageW, (MSG *msg, HWND w, UINT lo, UINT hi), (msg, w, lo, hi))
SO_IMP("user32.dll", BOOL, TranslateMessage, (const MSG *msg), (msg))
SO_IMP("user32.dll", LRESULT, DispatchMessageW, (const MSG *msg), (msg))
SO_IMP("user32.dll", BOOL, IsDialogMessageW, (HWND w, MSG *msg), (w, msg))
SO_IMP("user32.dll", LRESULT, SendMessageW, (HWND w, UINT m, WPARAM wp, LPARAM lp), (w, m, wp, lp))
SO_IMP("user32.dll", LONG_PTR, GetWindowLongPtrW, (HWND w, int index), (w, index))
SO_IMP("user32.dll", LONG_PTR, SetWindowLongPtrW, (HWND w, int index, LONG_PTR value), (w, index, value))
SO_IMP("user32.dll", BOOL, AdjustWindowRectEx, (RECT *rect, DWORD style, BOOL menu, DWORD ex), (rect, style, menu, ex))
SO_IMP("user32.dll", BOOL, SetWindowTextW, (HWND w, LPCWSTR text), (w, text))
SO_IMP("ole32.dll", HRESULT, CoCreateInstance, (REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID riid, LPVOID *ppv), (clsid, outer, context, riid, ppv))
SO_IMPV("ole32.dll", CoTaskMemFree, (LPVOID p), (p))

#define SO_ID_OK 1
#define SO_ID_CANCEL 2
#define SO_ID_APPLY 3
#define SO_ID_TAB 100

typedef struct so_frame so_frame;
typedef struct so_site {
  IPropertyPageSite iface;
  so_frame *frame;
  LCID lcid;
} so_site;

struct so_frame {
  HWND hwnd;
  so_site site;
  ULONG count;                          /* the pages */
  IPropertyPage **pages;
  BOOL *active;
  HWND *tabs;
  HWND apply_button;
  RECT area;
  ULONG current;
  BOOL done;
};

static HRESULT WINAPI so_site_QueryInterface(IPropertyPageSite *iface, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPropertyPageSite)) { *ppv = iface; return S_OK; }
  *ppv = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI so_site_AddRef(IPropertyPageSite *iface) { (void)iface; return 2; }
static ULONG WINAPI so_site_Release(IPropertyPageSite *iface) { (void)iface; return 1; }
static HRESULT WINAPI so_site_OnStatusChange(IPropertyPageSite *iface, DWORD flags) {
  so_site *s = CONTAINING_RECORD(iface, so_site, iface);
  if ((flags & (PROPPAGESTATUS_DIRTY | PROPPAGESTATUS_VALIDATE)) && s->frame->apply_button) so_EnableWindow(s->frame->apply_button, TRUE);
  return S_OK;
}
static HRESULT WINAPI so_site_GetLocaleID(IPropertyPageSite *iface, LCID *lcid) {
  if (!lcid) return E_POINTER;
  *lcid = CONTAINING_RECORD(iface, so_site, iface)->lcid;
  return S_OK;
}
static HRESULT WINAPI so_site_GetPageContainer(IPropertyPageSite *iface, IUnknown **container) { (void)iface; if (container) *container = NULL; return E_NOTIMPL; }
static HRESULT WINAPI so_site_TranslateAccelerator(IPropertyPageSite *iface, MSG *msg) { (void)iface; (void)msg; return S_FALSE; }
static const IPropertyPageSiteVtbl so_site_vtbl = {
  so_site_QueryInterface, so_site_AddRef, so_site_Release, so_site_OnStatusChange, so_site_GetLocaleID, so_site_GetPageContainer, so_site_TranslateAccelerator
};

static void so_frame_apply(so_frame *f) {
  for (ULONG i = 0; i < f->count; i++)
    if (f->pages[i] && f->active[i] && IPropertyPage_IsPageDirty(f->pages[i]) == S_OK) IPropertyPage_Apply(f->pages[i]);
  if (f->apply_button) so_EnableWindow(f->apply_button, FALSE);
}
static void so_frame_select(so_frame *f, ULONG index) {
  if (index >= f->count || !f->pages[index]) return;
  if (f->current < f->count && f->active[f->current] && f->current != index) IPropertyPage_Show(f->pages[f->current], SW_HIDE);
  if (!f->active[index]) {
    if (FAILED(IPropertyPage_Activate(f->pages[index], f->hwnd, &f->area, FALSE))) return;
    f->active[index] = TRUE;
  }
  IPropertyPage_Show(f->pages[index], SW_SHOW);
  f->current = index;
}

static LRESULT CALLBACK so_frame_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  so_frame *f = (so_frame *)so_GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (message) {
  case WM_NCCREATE:
    so_SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams);
    break;
  case WM_COMMAND:
    if (!f) break;
    if (LOWORD(wparam) == SO_ID_OK) { so_frame_apply(f); so_DestroyWindow(hwnd); return 0; }
    if (LOWORD(wparam) == SO_ID_CANCEL) { so_DestroyWindow(hwnd); return 0; }
    if (LOWORD(wparam) == SO_ID_APPLY) { so_frame_apply(f); return 0; }
    if (LOWORD(wparam) >= SO_ID_TAB && LOWORD(wparam) < SO_ID_TAB + f->count) { so_frame_select(f, LOWORD(wparam) - SO_ID_TAB); return 0; }
    break;
  case WM_CLOSE:
    so_DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    if (f) f->done = TRUE;
    return 0;
  default: break;
  }
  return so_DefWindowProcW(hwnd, message, wparam, lparam);
}

static HWND so_frame_button(so_frame *f, const WCHAR *text, DWORD style, int x, int y, int cx, int cy, int id) {
  static const WCHAR button_class[] = {'B','U','T','T','O','N',0};
  HWND h = so_CreateWindowExW(0, button_class, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, cx, cy, f->hwnd, (HMENU)(ULONG_PTR)id, GetModuleHandleW(NULL), NULL);
  if (h) so_SendMessageW(h, WM_SETFONT, (WPARAM)so_GetStockObject(DEFAULT_GUI_FONT), TRUE);
  return h;
}

EXPORT HRESULT WINAPI OleCreatePropertyFrameIndirect(LPOCPFIPARAMS params) {
  static const WCHAR class_name[] = {'O','l','e','A','u','t','3','2','P','r','o','p','F','r','a','m','e',0};
  static const WCHAR ok_text[] = {'O','K',0}, cancel_text[] = {'C','a','n','c','e','l',0}, apply_text[] = {'A','p','p','l','y',0};
  static const WCHAR default_caption[] = {'P','r','o','p','e','r','t','i','e','s',0};
  if (!params) return E_POINTER;
  if (params->cbStructSize < sizeof(OCPFIPARAMS)) return E_INVALIDARG;
  if (params->cObjects == 0 || params->cPages == 0) return E_INVALIDARG;
  if (!params->lplpUnk || !params->lpPages) return E_POINTER;
  if (params->cObjects > 4096 || params->cPages > 256) return E_INVALIDARG;

  so_frame *f = oa_calloc(sizeof(*f));
  if (!f) return E_OUTOFMEMORY;
  f->pages = oa_calloc(sizeof(*f->pages) * params->cPages);
  f->active = oa_calloc(sizeof(*f->active) * params->cPages);
  f->tabs = oa_calloc(sizeof(*f->tabs) * params->cPages);
  PROPPAGEINFO *infos = oa_calloc(sizeof(*infos) * params->cPages);
  HRESULT hr = E_OUTOFMEMORY, first_error = S_OK;
  if (!f->pages || !f->active || !f->tabs || !infos) goto done;
  f->site.iface.lpVtbl = (void *)&so_site_vtbl;
  f->site.frame = f;
  f->site.lcid = params->lcid;
  f->count = params->cPages;
  f->current = (ULONG)-1;

  /* the pages */
  LONG area_cx = 240, area_cy = 120;
  ULONG made = 0;
  for (ULONG i = 0; i < params->cPages; i++) {
    IPropertyPage *page = NULL;
    HRESULT page_hr = so_CoCreateInstance(&params->lpPages[i], NULL, CLSCTX_INPROC_SERVER, &IID_IPropertyPage, (void **)&page);
    if (SUCCEEDED(page_hr) && !page) page_hr = E_NOINTERFACE;          /* (the stand-in for ole32 reports success without an object) */
    if (FAILED(page_hr)) { if (SUCCEEDED(first_error)) first_error = page_hr; continue; }
    infos[i].cb = sizeof(PROPPAGEINFO);
    if (FAILED(IPropertyPage_SetPageSite(page, &f->site.iface)) || FAILED(IPropertyPage_SetObjects(page, params->cObjects, params->lplpUnk))
        || FAILED(IPropertyPage_GetPageInfo(page, &infos[i]))) {
      IPropertyPage_SetPageSite(page, NULL);
      IPropertyPage_Release(page);
      continue;
    }
    f->pages[i] = page;
    made++;
    if (infos[i].size.cx > area_cx) area_cx = infos[i].size.cx;
    if (infos[i].size.cy > area_cy) area_cy = infos[i].size.cy;
  }
  if (made == 0) { hr = FAILED(first_error) ? first_error : E_FAIL; goto done; }

  /* the window: a row of page buttons, the page area, then the buttons */
  const int margin = 8, row = 24, button_cx = 75, button_cy = 23;
  int tab_cx = 90;
  f->area.left = margin; f->area.top = margin + row + margin; f->area.right = f->area.left + area_cx; f->area.bottom = f->area.top + area_cy;
  int client_cx = area_cx + 2 * margin, client_cy = f->area.bottom + margin + button_cy + margin;
  if ((int)made * tab_cx + 2 * margin > client_cx) client_cx = (int)made * tab_cx + 2 * margin;
  {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = so_frame_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
    wc.lpszClassName = class_name;
    so_RegisterClassW(&wc);                 /* (a second registration fails harmlessly) */
  }
  RECT outer = {0, 0, client_cx, client_cy};
  so_AdjustWindowRectEx(&outer, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
  f->hwnd = so_CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, params->lpszCaption ? params->lpszCaption : default_caption, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               params->x, params->y, outer.right - outer.left, outer.bottom - outer.top, params->hWndOwner, NULL, GetModuleHandleW(NULL), f);
  if (!f->hwnd) { hr = E_FAIL; goto done; }
  int tab_x = margin;
  for (ULONG i = 0; i < params->cPages; i++) {
    if (!f->pages[i]) continue;
    f->tabs[i] = so_frame_button(f, infos[i].pszTitle, BS_PUSHBUTTON, tab_x, margin, tab_cx, row, SO_ID_TAB + (int)i);
    tab_x += tab_cx;
  }
  int by = client_cy - margin - button_cy, bx = client_cx - margin - 3 * button_cx - 2 * 6;
  so_frame_button(f, ok_text, BS_DEFPUSHBUTTON, bx, by, button_cx, button_cy, SO_ID_OK);
  so_frame_button(f, cancel_text, BS_PUSHBUTTON, bx + button_cx + 6, by, button_cx, button_cy, SO_ID_CANCEL);
  f->apply_button = so_frame_button(f, apply_text, BS_PUSHBUTTON, bx + 2 * (button_cx + 6), by, button_cx, button_cy, SO_ID_APPLY);
  if (f->apply_button) so_EnableWindow(f->apply_button, FALSE);
  /* the page that is shown first (the first one that exists) */
  for (ULONG i = 0; i < params->cPages; i++) if (f->pages[i]) { so_frame_select(f, i); break; }

  /* the modal loop: the owner is disabled until the frame is closed */
  if (params->hWndOwner) so_EnableWindow(params->hWndOwner, FALSE);
  so_ShowWindow(f->hwnd, SW_SHOW);
  MSG msg;
  while (!f->done && so_GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (so_IsDialogMessageW(f->hwnd, &msg)) continue;
    so_TranslateMessage(&msg);
    so_DispatchMessageW(&msg);
  }
  if (params->hWndOwner) so_EnableWindow(params->hWndOwner, TRUE);
  hr = S_OK;
done:
  if (f->hwnd && !f->done) so_DestroyWindow(f->hwnd);
  for (ULONG i = 0; f->pages && i < params->cPages; i++) {
    if (!f->pages[i]) continue;
    if (f->active && f->active[i]) IPropertyPage_Deactivate(f->pages[i]);
    IPropertyPage_SetPageSite(f->pages[i], NULL);
    IPropertyPage_Release(f->pages[i]);
  }
  for (ULONG i = 0; infos && i < params->cPages; i++) {
    if (infos[i].pszTitle) so_CoTaskMemFree(infos[i].pszTitle);
    if (infos[i].pszDocString) so_CoTaskMemFree(infos[i].pszDocString);
    if (infos[i].pszHelpFile) so_CoTaskMemFree(infos[i].pszHelpFile);
  }
  oa_free(infos);
  oa_free(f->pages); oa_free(f->active); oa_free(f->tabs);
  oa_free(f);
  return hr;
}

EXPORT HRESULT WINAPI OleCreatePropertyFrame(HWND owner, UINT x, UINT y, LPCOLESTR caption, ULONG count, LPUNKNOWN *objects, ULONG page_count, LPCLSID pages, LCID lcid,
                                             DWORD reserved, LPVOID reserved2) {
  OCPFIPARAMS params;
  (void)reserved; (void)reserved2;
  memset(&params, 0, sizeof(params));
  params.cbStructSize = sizeof(params);
  params.hWndOwner = owner;
  params.x = (int)x; params.y = (int)y;
  params.lpszCaption = caption;
  params.cObjects = count; params.lplpUnk = objects;
  params.cPages = page_count; params.lpPages = pages;
  params.lcid = lcid;
  params.dispidInitialProperty = DISPID_UNKNOWN;
  return OleCreatePropertyFrameIndirect(&params);
}
