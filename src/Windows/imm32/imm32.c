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

/* imm32.dll for TheoryCraftOS: the Input Method Manager. Like Windows, the input contexts (HIMC) are two objects: the handle, which win32k owns (NtUserCreateInputContext ...
 * and the association of windows with contexts, src/Process/Win32k/Win32kUserMiscKeyboard.cpp), and the INPUTCONTEXT with its HIMCC components (composition string,
 * candidate lists, guide line, message buffer), which lives here in the process; win32k keeps a pointer to it. The default context of a thread is made when a window of the
 * thread is first asked for its context. The IME layer: IMEINFOEX and the hot keys are win32k's (NtUserGetImeInfoEx ..), the layouts that are installed are the ones win32k
 * has (the US layout, which has no IME), so no IME module is ever loaded: ImmIsIME is FALSE, the IME-driven functions (ImmEscape, ImmConfigureIME, ImmSetCompositionString,
 * ImmNotifyIME, the word registration, the conversion list ...) answer as Windows does without an IME, and the composition, candidate and guide line getters read what is in the
 * context (empty unless a program put something there through ImmLockIMC). The default IME window ("IME" class) is not made, ImmGetDefaultIMEWnd answers NULL. The exports
 * for the text services framework (CtfImm*) that matter without it are here; the rest stays a stub (src/Windows/stubs/imm32.inc). Built freestanding over kernel32.dll and
 * win32u.dll with the OS's mingw compiler. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* ---- win32k services (the stubs of win32u.dll) ---------------------------------------------------------------------------------------------------------------------- */
#define SVC __declspec(dllimport) WINAPI
ULONG_PTR SVC NtUserCallHwnd(HWND hwnd, DWORD code);
ULONG_PTR SVC NtUserQueryWindow(HWND hwnd, DWORD type);
LRESULT SVC NtUserMessageCall(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *info, DWORD type, BOOL ansi);
HIMC SVC NtUserCreateInputContext(ULONG_PTR client);
BOOL SVC NtUserDestroyInputContext(HIMC himc);
BOOL SVC NtUserUpdateInputContext(HIMC himc, DWORD what, ULONG_PTR value);
ULONG_PTR SVC NtUserQueryInputContext(HIMC himc, DWORD what);
DWORD SVC NtUserAssociateInputContext(HWND hwnd, HIMC himc, DWORD flags);
BOOL SVC NtUserGetImeInfoEx(void *info, DWORD by);
BOOL SVC NtUserGetImeHotKey(DWORD id, UINT *modifiers, UINT *vk, HKL *hkl);
BOOL SVC NtUserSetImeHotKey(DWORD id, UINT modifiers, UINT vk, HKL hkl, DWORD action);
UINT SVC NtUserBuildHimcList(DWORD tid, DWORD count, HIMC *list, UINT *needed);
void SVC NtUserNotifyIMEStatus(HWND hwnd, DWORD open, DWORD conversion);
BOOL SVC NtUserDisableThreadIme(DWORD tid);
BOOL SVC NtUserSetThreadLayoutHandles(HKL new_layout, HKL old_layout);
HKL SVC NtUserGetKeyboardLayout(DWORD tid);
UINT SVC NtUserSendInput(UINT count, void *inputs, int size);
LONG SVC NtUserBuildHwndList(HANDLE desktop, HWND parent, BOOL children, BOOL non_immersive, ULONG tid, ULONG count, HWND *list, ULONG *needed);

#define QIC_CLIENT_POINTER 0
#define QIC_THREAD 1
#define UIC_THREAD 1
#define CALLHWND_GETWINDOWINPUTCONTEXT 7
#define CALLHWND_ISWINDOW 10
#define QUERYWINDOW_PROCESS 0
#define QUERYWINDOW_THREAD 2
#define NO_ASSOCIATION 0xFFFFFFFFu      /* what win32k answers for a window nothing was associated with */
#define AICR_FAILED 2
#define MESSAGECALL_SENDMESSAGE 0x2b1

static BOOL window_exists(HWND hwnd) { return hwnd && NtUserCallHwnd(hwnd, CALLHWND_ISWINDOW) != 0; }

/* ---- structures that mingw's imm.h does not have ------------------------------------------------------------------------------------------------------------------- */
#ifndef HIMCC
typedef HANDLE HIMCC_;
#define HIMCC HIMCC_
#endif

typedef struct {
  DWORD dwSize, dwCompReadAttrLen, dwCompReadAttrOffset, dwCompReadClauseLen, dwCompReadClauseOffset, dwCompReadStrLen, dwCompReadStrOffset, dwCompAttrLen, dwCompAttrOffset,
      dwCompClauseLen, dwCompClauseOffset, dwCompStrLen, dwCompStrOffset, dwCursorPos, dwDeltaStart, dwResultReadClauseLen, dwResultReadClauseOffset, dwResultReadStrLen,
      dwResultReadStrOffset, dwResultClauseLen, dwResultClauseOffset, dwResultStrLen, dwResultStrOffset, dwPrivateSize, dwPrivateOffset;
} COMPSTR;
typedef struct { DWORD dwSize, dwCount, dwOffset[32], dwPrivateSize, dwPrivateOffset; } CANDINFO;
typedef struct { DWORD dwSize, dwLevel, dwIndex, dwStrLen, dwStrOffset, dwPrivateSize, dwPrivateOffset; } GUIDELINE_;
typedef struct { UINT message; WPARAM wParam; LPARAM lParam; } TRANSMSG_;
typedef struct { DWORD dwPrivateDataSize, fdwProperty, fdwConversionCaps, fdwSentenceCaps, fdwUICaps, fdwSCSCaps, fdwSelectCaps; } IMEINFO_;
typedef struct {
  HKL hkl;
  IMEINFO_ ImeInfo;
  WCHAR wszUIClass[16];
  ULONG fdwInitConvMode;
  INT fInitOpen, fLoadFlag;
  DWORD dwProcessId, dwFlags;
  WCHAR wszImeDescription[50], wszImeFile[80];
} IMEINFOEX_;      /* the 352 bytes win32k keeps (kImeInfoExSize) */
typedef char imeinfoex_size_check[sizeof(IMEINFOEX_) == 352 ? 1 : -1];

#define INIT_STATUSWNDPOS 0x00000001
#define INIT_CONVERSION 0x00000002
#define INIT_SENTENCE 0x00000004
#define INIT_LOGFONT 0x00000008
#define INIT_COMPFORM 0x00000010
#define INIT_SOFTKBDPOS 0x00000020

typedef struct {
  HWND hWnd;
  BOOL fOpen;
  POINT ptStatusWndPos, ptSoftKbdPos;
  DWORD fdwConversion, fdwSentence;
  LOGFONTW lfFont;      /* kept wide; the A forms convert */
  COMPOSITIONFORM cfCompForm;
  CANDIDATEFORM cfCandForm[4];
  HIMCC hCompStr, hCandInfo, hGuideLine, hPrivate;
  DWORD dwNumMsgBuf;
  HIMCC hMsgBuf;
  DWORD fdwInit, dwReserve[3];
} INPUTCTX;

/* The client side of an input context (what win32k keeps a pointer to). */
typedef struct client_imc {
  HIMC himc;
  DWORD tid;
  LONG ref;                /* ImmLockClientImc */
  LONG lock;               /* ImmLockIMC */
  BOOL is_default;
  DWORD app_compat;
  INPUTCTX ic;
} CLIENTIMC_;

/* ---- state ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { BYTE *data; DWORD size; LONG lock; BOOL used; } Imcc;

static volatile LONG g_lock;
static Imcc *g_imcc;
static DWORD g_imcc_count;
static CLIENTIMC_ **g_clients;
static DWORD g_client_count;
static DWORD g_app_compat_flags;
static BOOL g_nls_enabled = TRUE;
static BOOL g_cicero_started;

#define IMCC_BASE 0xCC000000u      /* a HIMCC is IMCC_BASE | slot + 1 */

static void lock_state(void) { while (InterlockedCompareExchange(&g_lock, 1, 0) != 0) SwitchToThread(); }
static void unlock_state(void) { InterlockedExchange(&g_lock, 0); }

static void *alloc_zero(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void free_block(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void copy_bytes(void *dest, const void *source, SIZE_T n) {
  BYTE *d = dest;
  const BYTE *s = source;
  while (n--) *d++ = *s++;
}
static void zero_bytes(void *dest, SIZE_T n) {
  BYTE *d = dest;
  while (n--) *d++ = 0;
}
static SIZE_T wide_length(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }

/* ---- HIMCC: component blocks of an input context ------------------------------------------------------------------------------------------------------------------ */
static HIMCC imcc_create(DWORD size) {
  if (size < sizeof(DWORD)) size = sizeof(DWORD);
  BYTE *data = alloc_zero(size);
  if (!data) return NULL;
  lock_state();
  DWORD slot = 0;
  while (slot < g_imcc_count && g_imcc[slot].used) ++slot;
  if (slot == g_imcc_count) {
    Imcc *bigger = alloc_zero(sizeof(Imcc) * (g_imcc_count + 32));
    if (!bigger) { unlock_state(); free_block(data); return NULL; }
    copy_bytes(bigger, g_imcc, sizeof(Imcc) * g_imcc_count);
    free_block(g_imcc);
    g_imcc = bigger;
    g_imcc_count += 32;
  }
  g_imcc[slot].data = data;
  g_imcc[slot].size = size;
  g_imcc[slot].lock = 0;
  g_imcc[slot].used = TRUE;
  unlock_state();
  return (HIMCC)(ULONG_PTR)(IMCC_BASE | (slot + 1));
}

/* The table entry of a handle; the state lock must be held. */
static Imcc *imcc_find(HIMCC h) {
  const ULONG_PTR v = (ULONG_PTR)h;
  if ((v & 0xFF000000u) != IMCC_BASE || v > 0xFFFFFFFFu) return NULL;
  const DWORD slot = (DWORD)(v & 0xFFFFFF);
  if (slot == 0 || slot > g_imcc_count || !g_imcc[slot - 1].used) return NULL;
  return &g_imcc[slot - 1];
}

EXPORT HIMCC WINAPI ImmCreateIMCC(DWORD size) { return imcc_create(size); }

EXPORT HIMCC WINAPI ImmDestroyIMCC(HIMCC h) {
  lock_state();
  Imcc *e = imcc_find(h);
  if (!e) { unlock_state(); return h; }
  BYTE *data = e->data;
  zero_bytes(e, sizeof(*e));
  unlock_state();
  free_block(data);
  return NULL;
}

EXPORT LPVOID WINAPI ImmLockIMCC(HIMCC h) {
  lock_state();
  Imcc *e = imcc_find(h);
  void *p = NULL;
  if (e) { ++e->lock; p = e->data; }
  unlock_state();
  return p;
}

EXPORT BOOL WINAPI ImmUnlockIMCC(HIMCC h) {
  lock_state();
  Imcc *e = imcc_find(h);
  BOOL still = FALSE;
  if (e && e->lock > 0) still = --e->lock > 0;
  unlock_state();
  return still;
}

EXPORT DWORD WINAPI ImmGetIMCCLockCount(HIMCC h) {
  lock_state();
  Imcc *e = imcc_find(h);
  const DWORD n = e ? (DWORD)e->lock : 0;
  unlock_state();
  return n;
}

EXPORT DWORD WINAPI ImmGetIMCCSize(HIMCC h) {
  lock_state();
  Imcc *e = imcc_find(h);
  const DWORD n = e ? e->size : 0;
  unlock_state();
  return n;
}

EXPORT HIMCC WINAPI ImmReSizeIMCC(HIMCC h, DWORD size) {
  if (size < sizeof(DWORD)) size = sizeof(DWORD);
  BYTE *data = alloc_zero(size);
  if (!data) return NULL;
  lock_state();
  Imcc *e = imcc_find(h);
  if (!e) { unlock_state(); free_block(data); return NULL; }
  copy_bytes(data, e->data, e->size < size ? e->size : size);
  BYTE *old = e->data;
  e->data = data;
  e->size = size;
  unlock_state();
  free_block(old);
  return h;
}

/* ---- HIMC: client side input contexts ------------------------------------------------------------------------------------------------------------------------------ */
/* The client structure of a handle of this process (null for any other: the handles of win32k are global). The state lock must be held. */
static CLIENTIMC_ *client_find(HIMC himc) {
  for (DWORD i = 0; i < g_client_count; ++i) if (g_clients[i] && g_clients[i]->himc == himc) return g_clients[i];
  return NULL;
}

static void init_input_context(INPUTCTX *ic) {
  ic->fOpen = FALSE;
  ic->fdwConversion = IME_CMODE_ALPHANUMERIC;
  ic->fdwSentence = IME_SMODE_NONE;
  for (int i = 0; i < 4; ++i) ic->cfCandForm[i].dwIndex = (DWORD)-1;
  ic->hCompStr = imcc_create(sizeof(COMPSTR));
  ic->hCandInfo = imcc_create(sizeof(CANDINFO));
  ic->hGuideLine = imcc_create(sizeof(GUIDELINE_));
  ic->hPrivate = imcc_create(sizeof(DWORD));
  ic->hMsgBuf = imcc_create(sizeof(TRANSMSG_));
  COMPSTR *comp = ImmLockIMCC(ic->hCompStr);
  if (comp) { comp->dwSize = sizeof(COMPSTR); ImmUnlockIMCC(ic->hCompStr); }
  CANDINFO *cand = ImmLockIMCC(ic->hCandInfo);
  if (cand) { cand->dwSize = sizeof(CANDINFO); ImmUnlockIMCC(ic->hCandInfo); }
  GUIDELINE_ *guide = ImmLockIMCC(ic->hGuideLine);
  if (guide) { guide->dwSize = sizeof(GUIDELINE_); ImmUnlockIMCC(ic->hGuideLine); }
}

static void free_input_context(INPUTCTX *ic) {
  ImmDestroyIMCC(ic->hCompStr);
  ImmDestroyIMCC(ic->hCandInfo);
  ImmDestroyIMCC(ic->hGuideLine);
  ImmDestroyIMCC(ic->hPrivate);
  ImmDestroyIMCC(ic->hMsgBuf);
}

/* Makes a context for thread `tid` (the calling thread or another one of the process). */
static HIMC context_create(DWORD tid, BOOL is_default) {
  CLIENTIMC_ *client = alloc_zero(sizeof(*client));
  if (!client) return NULL;
  client->tid = tid;
  client->is_default = is_default;
  const HIMC himc = NtUserCreateInputContext((ULONG_PTR)client);
  if (!himc) { free_block(client); return NULL; }
  if (tid != GetCurrentThreadId()) NtUserUpdateInputContext(himc, UIC_THREAD, tid);
  client->himc = himc;
  init_input_context(&client->ic);
  lock_state();
  DWORD slot = 0;
  while (slot < g_client_count && g_clients[slot]) ++slot;
  if (slot == g_client_count) {
    CLIENTIMC_ **bigger = alloc_zero(sizeof(*bigger) * (g_client_count + 16));
    if (!bigger) { unlock_state(); free_input_context(&client->ic); NtUserDestroyInputContext(himc); free_block(client); return NULL; }
    copy_bytes(bigger, g_clients, sizeof(*bigger) * g_client_count);
    free_block(g_clients);
    g_clients = bigger;
    g_client_count += 16;
  }
  g_clients[slot] = client;
  unlock_state();
  return himc;
}

static BOOL context_free(HIMC himc) {
  lock_state();
  CLIENTIMC_ *client = NULL;
  for (DWORD i = 0; i < g_client_count; ++i) if (g_clients[i] && g_clients[i]->himc == himc) { client = g_clients[i]; g_clients[i] = NULL; break; }
  unlock_state();
  if (!client) return FALSE;
  free_input_context(&client->ic);
  NtUserDestroyInputContext(himc);
  free_block(client);
  return TRUE;
}

/* The thread's own client structure for himc, or null if himc is not a context of this process. ic is the INPUTCONTEXT inside it. */
static INPUTCTX *context_ic(HIMC himc, CLIENTIMC_ **out_client) {
  lock_state();
  CLIENTIMC_ *client = himc ? client_find(himc) : NULL;
  unlock_state();
  if (out_client) *out_client = client;
  return client ? &client->ic : NULL;
}

static BOOL cross_thread(CLIENTIMC_ *client) { return client->tid != GetCurrentThreadId(); }

/* The default context of a thread, made on first use. */
static HIMC default_context(DWORD tid) {
  lock_state();
  for (DWORD i = 0; i < g_client_count; ++i) {
    if (g_clients[i] && g_clients[i]->is_default && g_clients[i]->tid == tid) { const HIMC h = g_clients[i]->himc; unlock_state(); return h; }
  }
  unlock_state();
  return context_create(tid, TRUE);
}

/* The context a window uses: the one associated with it, or the default one of its thread. Null for a window of another process, one that is gone, or one with the IME off. */
static HIMC window_context(HWND hwnd) {
  if (!hwnd) return NULL;
  const ULONG_PTR pid = NtUserQueryWindow(hwnd, QUERYWINDOW_PROCESS);
  if (pid != GetCurrentProcessId()) return NULL;
  const ULONG_PTR raw = NtUserCallHwnd(hwnd, CALLHWND_GETWINDOWINPUTCONTEXT);
  if (raw == NO_ASSOCIATION) {
    const DWORD tid = (DWORD)NtUserQueryWindow(hwnd, QUERYWINDOW_THREAD);
    const HIMC himc = tid ? default_context(tid) : NULL;
    if (himc) {
      INPUTCTX *ic = context_ic(himc, NULL);
      if (ic && !ic->hWnd) ic->hWnd = hwnd;
    }
    return himc;
  }
  return (HIMC)raw;
}

EXPORT HIMC WINAPI ImmGetContext(HWND hwnd) {
  if (!window_exists(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return NULL; }
  return window_context(hwnd);
}

EXPORT BOOL WINAPI ImmReleaseContext(HWND hwnd, HIMC himc) { (void)hwnd; (void)himc; return TRUE; }

EXPORT HIMC WINAPI ImmCreateContext(void) { return context_create(GetCurrentThreadId(), FALSE); }

EXPORT BOOL WINAPI ImmDestroyContext(HIMC himc) {
  CLIENTIMC_ *client;
  if (!context_ic(himc, &client)) return FALSE;
  if (client->is_default || cross_thread(client)) return FALSE;
  return context_free(himc);
}

EXPORT HIMC WINAPI ImmAssociateContext(HWND hwnd, HIMC himc) {
  if (!window_exists(hwnd)) return NULL;
  const HIMC previous = window_context(hwnd);
  if (NtUserQueryWindow(hwnd, QUERYWINDOW_PROCESS) != GetCurrentProcessId()) return NULL;
  if (himc) {
    CLIENTIMC_ *client;
    if (!context_ic(himc, &client)) return NULL;
  }
  if (NtUserAssociateInputContext(hwnd, himc, 0) == AICR_FAILED) return NULL;
  if (himc) { INPUTCTX *ic = context_ic(himc, NULL); if (ic) ic->hWnd = hwnd; }
  return previous;
}

EXPORT BOOL WINAPI ImmAssociateContextEx(HWND hwnd, HIMC himc, DWORD flags) {
  if (flags & ~(DWORD)(IACE_CHILDREN | IACE_DEFAULT | IACE_IGNORENOCONTEXT)) return FALSE;
  if ((flags & IACE_DEFAULT) && himc) return FALSE;
  if (!window_exists(hwnd)) return FALSE;
  if (flags & IACE_DEFAULT) {      /* back to the default context of the thread: the association is dropped (win32k has none to drop; associating the default one is the same) */
    himc = default_context((DWORD)NtUserQueryWindow(hwnd, QUERYWINDOW_THREAD));
    if (!himc) return FALSE;
  }
  BOOL ok = TRUE;
  if (!(flags & IACE_IGNORENOCONTEXT) || window_context(hwnd) != NULL) ok = ImmAssociateContext(hwnd, himc) != NULL || himc == NULL || window_context(hwnd) == himc;
  if (flags & IACE_CHILDREN) {
    ULONG needed = 0;
    NtUserBuildHwndList(NULL, hwnd, TRUE, FALSE, 0, 0, NULL, &needed);
    HWND *children = needed ? alloc_zero(sizeof(HWND) * needed) : NULL;
    if (children && NtUserBuildHwndList(NULL, hwnd, TRUE, FALSE, 0, needed, children, &needed) >= 0) {
      for (ULONG i = 0; i < needed; ++i) {
        if ((flags & IACE_IGNORENOCONTEXT) && window_context(children[i]) == NULL) continue;
        ImmAssociateContext(children[i], himc);
      }
    }
    free_block(children);
  }
  return ok;
}

EXPORT BOOL WINAPI ImmEnumInputContext(DWORD tid, IMCENUMPROC proc, LPARAM param) {
  UINT needed = 0;
  NtUserBuildHimcList(tid, 0, NULL, &needed);
  if (needed == 0) return TRUE;
  HIMC *list = alloc_zero(sizeof(HIMC) * needed);
  if (!list) return FALSE;
  const UINT n = NtUserBuildHimcList(tid, needed, list, &needed);
  BOOL result = TRUE;
  for (UINT i = 0; i < n && result; ++i) result = proc(list[i], param);
  free_block(list);
  return TRUE;
}

EXPORT LPVOID WINAPI ImmLockIMC(HIMC himc) {
  CLIENTIMC_ *client;
  if (!context_ic(himc, &client)) return NULL;
  InterlockedIncrement(&client->lock);
  return &client->ic;
}

EXPORT BOOL WINAPI ImmUnlockIMC(HIMC himc) {
  CLIENTIMC_ *client;
  if (!context_ic(himc, &client)) return FALSE;
  if (client->lock <= 0) return FALSE;
  return InterlockedDecrement(&client->lock) > 0;
}

EXPORT DWORD WINAPI ImmGetIMCLockCount(HIMC himc) {
  CLIENTIMC_ *client;
  if (!context_ic(himc, &client)) return 0;
  return (DWORD)client->lock;
}

EXPORT LPVOID WINAPI ImmLockClientImc(HIMC himc) {
  CLIENTIMC_ *client;
  if (!context_ic(himc, &client)) return NULL;
  InterlockedIncrement(&client->ref);
  return client;
}

EXPORT void WINAPI ImmUnlockClientImc(LPVOID client) {
  CLIENTIMC_ *c = client;
  if (c && c->ref > 0) InterlockedDecrement(&c->ref);
}

/* ---- notifications to the window of a context ---------------------------------------------------------------------------------------------------------------------- */
static LRESULT send_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (!hwnd || !window_exists(hwnd)) return 0;
  return NtUserMessageCall(hwnd, message, wparam, lparam, NULL, MESSAGECALL_SENDMESSAGE, FALSE);
}

static void notify_window(HWND hwnd, WPARAM code, LPARAM data) { send_message(hwnd, WM_IME_NOTIFY, code, data); }

/* ---- status of a context -------------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT BOOL WINAPI ImmGetOpenStatus(HIMC himc) {
  INPUTCTX *ic = context_ic(himc, NULL);
  return ic ? ic->fOpen != 0 : FALSE;
}

EXPORT BOOL WINAPI ImmSetOpenStatus(HIMC himc, BOOL open) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic || cross_thread(client)) return FALSE;
  open = open != 0;
  if (ic->fOpen == open) return TRUE;
  ic->fOpen = open;
  if (ic->hWnd) NtUserNotifyIMEStatus(ic->hWnd, open, ic->fdwConversion);
  notify_window(ic->hWnd, IMN_SETOPENSTATUS, 0);
  return TRUE;
}

EXPORT BOOL WINAPI ImmGetConversionStatus(HIMC himc, LPDWORD conversion, LPDWORD sentence) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic) return FALSE;
  if (conversion) *conversion = ic->fdwConversion;
  if (sentence) *sentence = ic->fdwSentence;
  return TRUE;
}

EXPORT BOOL WINAPI ImmSetConversionStatus(HIMC himc, DWORD conversion, DWORD sentence) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic || cross_thread(client)) return FALSE;
  const BOOL conversion_changed = ic->fdwConversion != conversion, sentence_changed = ic->fdwSentence != sentence;
  ic->fdwConversion = conversion;
  ic->fdwSentence = sentence;
  if (conversion_changed) {
    if (ic->hWnd) NtUserNotifyIMEStatus(ic->hWnd, ic->fOpen, conversion);
    notify_window(ic->hWnd, IMN_SETCONVERSIONMODE, 0);
  }
  if (sentence_changed) notify_window(ic->hWnd, IMN_SETSENTENCEMODE, 0);
  return TRUE;
}

EXPORT BOOL WINAPI ImmGetStatusWindowPos(HIMC himc, LPPOINT pos) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic || !pos || !(ic->fdwInit & INIT_STATUSWNDPOS)) return FALSE;
  *pos = ic->ptStatusWndPos;
  return TRUE;
}

EXPORT BOOL WINAPI ImmSetStatusWindowPos(HIMC himc, LPPOINT pos) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic || !pos || cross_thread(client)) return FALSE;
  ic->ptStatusWndPos = *pos;
  ic->fdwInit |= INIT_STATUSWNDPOS;
  notify_window(ic->hWnd, IMN_SETSTATUSWINDOWPOS, 0);
  return TRUE;
}

EXPORT BOOL WINAPI ImmGetCompositionWindow(HIMC himc, LPCOMPOSITIONFORM form) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic || !form || !(ic->fdwInit & INIT_COMPFORM)) return FALSE;
  *form = ic->cfCompForm;
  return TRUE;
}

EXPORT BOOL WINAPI ImmSetCompositionWindow(HIMC himc, LPCOMPOSITIONFORM form) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic || !form || cross_thread(client)) return FALSE;
  ic->cfCompForm = *form;
  ic->fdwInit |= INIT_COMPFORM;
  notify_window(ic->hWnd, IMN_SETCOMPOSITIONWINDOW, 0);
  return TRUE;
}

EXPORT BOOL WINAPI ImmGetCandidateWindow(HIMC himc, DWORD index, LPCANDIDATEFORM form) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic || !form || index >= 4 || ic->cfCandForm[index].dwIndex == (DWORD)-1) return FALSE;
  *form = ic->cfCandForm[index];
  return TRUE;
}

EXPORT BOOL WINAPI ImmSetCandidateWindow(HIMC himc, LPCANDIDATEFORM form) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic || !form || form->dwIndex >= 4 || cross_thread(client)) return FALSE;
  ic->cfCandForm[form->dwIndex] = *form;
  notify_window(ic->hWnd, IMN_SETCANDIDATEPOS, (LPARAM)1 << form->dwIndex);
  return TRUE;
}

static void logfont_to_wide(const LOGFONTA *a, LOGFONTW *w) {
  copy_bytes(w, a, FIELD_OFFSET(LOGFONTA, lfFaceName));
  zero_bytes(w->lfFaceName, sizeof(w->lfFaceName));
  MultiByteToWideChar(CP_ACP, 0, a->lfFaceName, -1, w->lfFaceName, LF_FACESIZE);
  w->lfFaceName[LF_FACESIZE - 1] = 0;
}

static void logfont_to_ansi(const LOGFONTW *w, LOGFONTA *a) {
  copy_bytes(a, w, FIELD_OFFSET(LOGFONTA, lfFaceName));
  zero_bytes(a->lfFaceName, sizeof(a->lfFaceName));
  WideCharToMultiByte(CP_ACP, 0, w->lfFaceName, -1, a->lfFaceName, LF_FACESIZE, NULL, NULL);
  a->lfFaceName[LF_FACESIZE - 1] = 0;
}

EXPORT BOOL WINAPI ImmGetCompositionFontW(HIMC himc, LPLOGFONTW font) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic || !font || !(ic->fdwInit & INIT_LOGFONT)) return FALSE;
  *font = ic->lfFont;
  return TRUE;
}

EXPORT BOOL WINAPI ImmGetCompositionFontA(HIMC himc, LPLOGFONTA font) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic || !font || !(ic->fdwInit & INIT_LOGFONT)) return FALSE;
  logfont_to_ansi(&ic->lfFont, font);
  return TRUE;
}

EXPORT BOOL WINAPI ImmSetCompositionFontW(HIMC himc, LPLOGFONTW font) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic || !font || cross_thread(client)) return FALSE;
  ic->lfFont = *font;
  ic->fdwInit |= INIT_LOGFONT;
  notify_window(ic->hWnd, IMN_SETCOMPOSITIONFONT, 0);
  return TRUE;
}

EXPORT BOOL WINAPI ImmSetCompositionFontA(HIMC himc, LPLOGFONTA font) {
  LOGFONTW wide;
  if (!font) return FALSE;
  logfont_to_wide(font, &wide);
  return ImmSetCompositionFontW(himc, &wide);
}

/* Sends the messages the IME queued in the message buffer of the context to its window. */
EXPORT BOOL WINAPI ImmGenerateMessage(HIMC himc) {
  CLIENTIMC_ *client;
  INPUTCTX *ic = context_ic(himc, &client);
  if (!ic) return FALSE;
  const DWORD n = ic->dwNumMsgBuf;
  if (n == 0) return TRUE;
  TRANSMSG_ *messages = alloc_zero(sizeof(TRANSMSG_) * n);
  if (!messages) return FALSE;
  TRANSMSG_ *queue = ImmLockIMCC(ic->hMsgBuf);
  if (!queue) { free_block(messages); return FALSE; }
  const DWORD room = ImmGetIMCCSize(ic->hMsgBuf) / sizeof(TRANSMSG_);
  const DWORD count = n < room ? n : room;
  copy_bytes(messages, queue, sizeof(TRANSMSG_) * count);
  ImmUnlockIMCC(ic->hMsgBuf);
  ic->dwNumMsgBuf = 0;
  for (DWORD i = 0; i < count; ++i) send_message(ic->hWnd, messages[i].message, messages[i].wParam, messages[i].lParam);
  free_block(messages);
  return TRUE;
}

EXPORT BOOL WINAPI CtfImmGenerateMessage(HIMC himc, BOOL send) { (void)send; return ImmGenerateMessage(himc); }

/* ---- composition string, candidate lists, guide line ----------------------------------------------------------------------------------------------------------------------- */
/* The bytes [offset, offset + bytes) of a component block of `size` bytes: a pointer into it, null if the block is too small for them. */
static const BYTE *range_of(const BYTE *base, DWORD size, DWORD offset, DWORD bytes) {
  if (offset > size || bytes > size - offset) return NULL;
  return base + offset;
}

/* How many ANSI bytes the first `chars` characters of a wide text make. */
static int ansi_length(const WCHAR *text, DWORD chars) {
  if (chars == 0) return 0;
  return WideCharToMultiByte(CP_ACP, 0, text, (int)chars, NULL, 0, NULL, NULL);
}

static LONG copy_out(const void *source, DWORD size, void *buffer, DWORD buffer_size) {
  if (buffer_size == 0 || !buffer) return (LONG)size;
  const DWORD n = size < buffer_size ? size : buffer_size;
  copy_bytes(buffer, source, n);
  return (LONG)n;
}

#define GCS_KIND_STRING 0
#define GCS_KIND_ATTR 1
#define GCS_KIND_CLAUSE 2

static LONG composition_string(HIMC himc, DWORD index, void *buffer, DWORD buffer_size, BOOL wide) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic) return IMM_ERROR_GENERAL;
  if (buffer_size && !buffer) return IMM_ERROR_GENERAL;
  const DWORD header_size = ImmGetIMCCSize(ic->hCompStr);
  COMPSTR *comp = ImmLockIMCC(ic->hCompStr);
  if (!comp || header_size < sizeof(COMPSTR)) { if (comp) ImmUnlockIMCC(ic->hCompStr); return IMM_ERROR_NODATA; }
  LONG result = 0;
  if (index == GCS_CURSORPOS || index == GCS_DELTASTART) {
    DWORD value = index == GCS_CURSORPOS ? comp->dwCursorPos : comp->dwDeltaStart;
    if (!wide && value) {      /* characters -> ANSI bytes */
      const WCHAR *text = (const WCHAR *)range_of((const BYTE *)comp, header_size, comp->dwCompStrOffset, comp->dwCompStrLen * sizeof(WCHAR));
      if (text) value = (DWORD)ansi_length(text, value < comp->dwCompStrLen ? value : comp->dwCompStrLen);
    }
    ImmUnlockIMCC(ic->hCompStr);
    return (LONG)value;
  }
  DWORD len = 0, offset = 0, kind = GCS_KIND_STRING, text_len = 0, text_offset = 0;
  switch (index) {
    case GCS_COMPREADSTR: len = comp->dwCompReadStrLen; offset = comp->dwCompReadStrOffset; text_len = len; text_offset = offset; break;
    case GCS_COMPREADATTR: len = comp->dwCompReadAttrLen; offset = comp->dwCompReadAttrOffset; kind = GCS_KIND_ATTR; text_len = comp->dwCompReadStrLen; text_offset = comp->dwCompReadStrOffset; break;
    case GCS_COMPREADCLAUSE: len = comp->dwCompReadClauseLen; offset = comp->dwCompReadClauseOffset; kind = GCS_KIND_CLAUSE; text_len = comp->dwCompReadStrLen; text_offset = comp->dwCompReadStrOffset; break;
    case GCS_COMPSTR: len = comp->dwCompStrLen; offset = comp->dwCompStrOffset; text_len = len; text_offset = offset; break;
    case GCS_COMPATTR: len = comp->dwCompAttrLen; offset = comp->dwCompAttrOffset; kind = GCS_KIND_ATTR; text_len = comp->dwCompStrLen; text_offset = comp->dwCompStrOffset; break;
    case GCS_COMPCLAUSE: len = comp->dwCompClauseLen; offset = comp->dwCompClauseOffset; kind = GCS_KIND_CLAUSE; text_len = comp->dwCompStrLen; text_offset = comp->dwCompStrOffset; break;
    case GCS_RESULTREADSTR: len = comp->dwResultReadStrLen; offset = comp->dwResultReadStrOffset; text_len = len; text_offset = offset; break;
    case GCS_RESULTREADCLAUSE: len = comp->dwResultReadClauseLen; offset = comp->dwResultReadClauseOffset; kind = GCS_KIND_CLAUSE; text_len = comp->dwResultReadStrLen; text_offset = comp->dwResultReadStrOffset; break;
    case GCS_RESULTSTR: len = comp->dwResultStrLen; offset = comp->dwResultStrOffset; text_len = len; text_offset = offset; break;
    case GCS_RESULTCLAUSE: len = comp->dwResultClauseLen; offset = comp->dwResultClauseOffset; kind = GCS_KIND_CLAUSE; text_len = comp->dwResultStrLen; text_offset = comp->dwResultStrOffset; break;
    default: ImmUnlockIMCC(ic->hCompStr); return 0;
  }
  if (len == 0) { ImmUnlockIMCC(ic->hCompStr); return 0; }
  const DWORD bytes = kind == GCS_KIND_STRING ? len * sizeof(WCHAR) : len;      /* attribute and clause lengths are in bytes */
  const BYTE *data = range_of((const BYTE *)comp, header_size, offset, bytes);
  if (!data) { ImmUnlockIMCC(ic->hCompStr); return IMM_ERROR_NODATA; }
  if (wide) {
    if (kind == GCS_KIND_STRING && buffer_size) buffer_size &= ~(DWORD)1;
    result = copy_out(data, kind == GCS_KIND_STRING ? bytes : bytes, buffer, buffer_size);
  } else if (kind == GCS_KIND_STRING) {
    const int needed = ansi_length((const WCHAR *)data, len);
    if (buffer_size == 0 || !buffer) {
      result = needed;
    } else {
      char *tmp = alloc_zero((SIZE_T)needed + 1);
      if (!tmp) result = IMM_ERROR_GENERAL;
      else {
        WideCharToMultiByte(CP_ACP, 0, (const WCHAR *)data, (int)len, tmp, needed, NULL, NULL);
        result = copy_out(tmp, (DWORD)needed, buffer, buffer_size);
        free_block(tmp);
      }
    }
  } else {
    const WCHAR *text = (const WCHAR *)range_of((const BYTE *)comp, header_size, text_offset, text_len * sizeof(WCHAR));
    if (!text) {
      result = copy_out(data, bytes, buffer, buffer_size);
    } else if (kind == GCS_KIND_ATTR) {      /* one attribute per character -> one per ANSI byte */
      DWORD total = 0;
      for (DWORD i = 0; i < len && i < text_len; ++i) total += (DWORD)ansi_length(text + i, 1);
      if (buffer_size == 0 || !buffer) result = (LONG)total;
      else {
        BYTE *tmp = alloc_zero(total ? total : 1);
        if (!tmp) result = IMM_ERROR_GENERAL;
        else {
          DWORD at = 0;
          for (DWORD i = 0; i < len && i < text_len; ++i) for (int k = ansi_length(text + i, 1); k > 0; --k) tmp[at++] = data[i];
          result = copy_out(tmp, total, buffer, buffer_size);
          free_block(tmp);
        }
      }
    } else {      /* clause boundaries: character offsets -> byte offsets */
      const DWORD n = len / sizeof(DWORD);
      DWORD *tmp = alloc_zero(len);
      if (!tmp) result = IMM_ERROR_GENERAL;
      else {
        const DWORD *source = (const DWORD *)data;
        for (DWORD i = 0; i < n; ++i) { const DWORD v = source[i]; tmp[i] = (DWORD)ansi_length(text, v < text_len ? v : text_len); }
        result = copy_out(tmp, n * sizeof(DWORD), buffer, buffer_size);
        free_block(tmp);
      }
    }
  }
  ImmUnlockIMCC(ic->hCompStr);
  return result;
}

EXPORT LONG WINAPI ImmGetCompositionStringW(HIMC himc, DWORD index, LPVOID buffer, DWORD size) { return composition_string(himc, index, buffer, size, TRUE); }
EXPORT LONG WINAPI ImmGetCompositionStringA(HIMC himc, DWORD index, LPVOID buffer, DWORD size) { return composition_string(himc, index, buffer, size, FALSE); }

/* Setting the composition is the IME's: without one it fails. */
static BOOL set_composition_string(HIMC himc, DWORD index, DWORD comp_size, DWORD read_size) {
  if (!context_ic(himc, NULL)) return FALSE;
  if (index != SCS_SETSTR && index != SCS_CHANGEATTR && index != SCS_CHANGECLAUSE && index != SCS_SETRECONVERTSTRING && index != SCS_QUERYRECONVERTSTRING) return FALSE;
  (void)comp_size;
  (void)read_size;
  return FALSE;
}
EXPORT BOOL WINAPI ImmSetCompositionStringW(HIMC himc, DWORD index, LPVOID comp, DWORD comp_size, LPVOID read, DWORD read_size) { (void)comp; (void)read; return set_composition_string(himc, index, comp_size, read_size); }
EXPORT BOOL WINAPI ImmSetCompositionStringA(HIMC himc, DWORD index, LPVOID comp, DWORD comp_size, LPVOID read, DWORD read_size) { (void)comp; (void)read; return set_composition_string(himc, index, comp_size, read_size); }

/* The candidate list `index` of the context as stored (wide). Locks hCandInfo; the caller unlocks. */
static const CANDIDATELIST *candidate_list(INPUTCTX *ic, DWORD index, const CANDINFO **info_out) {
  const CANDINFO *info = ImmLockIMCC(ic->hCandInfo);
  if (!info) return NULL;
  *info_out = info;
  if (ImmGetIMCCSize(ic->hCandInfo) < sizeof(CANDINFO) || index >= info->dwCount || index >= 32) return NULL;
  const BYTE *base = (const BYTE *)info;
  const DWORD size = ImmGetIMCCSize(ic->hCandInfo), offset = info->dwOffset[index];
  if (offset > size || size - offset < sizeof(CANDIDATELIST)) return NULL;
  const CANDIDATELIST *list = (const CANDIDATELIST *)(base + offset);
  if (list->dwSize < sizeof(CANDIDATELIST) || list->dwSize > size - offset) return NULL;
  return list;
}

static BOOL candidate_strings_valid(const CANDIDATELIST *list) {
  if (list->dwCount > (list->dwSize - FIELD_OFFSET(CANDIDATELIST, dwOffset)) / sizeof(DWORD) + 1) return FALSE;
  for (DWORD i = 0; i < list->dwCount; ++i) if (list->dwOffset[i] >= list->dwSize || (list->dwSize - list->dwOffset[i]) < sizeof(WCHAR)) return FALSE;
  return TRUE;
}

static DWORD candidate_list_size(HIMC himc, DWORD index, void *buffer, DWORD buffer_size, BOOL wide) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic) return 0;
  const CANDINFO *info = NULL;
  const CANDIDATELIST *list = candidate_list(ic, index, &info);
  DWORD result = 0;
  if (list && wide) {
    if (buffer_size == 0 || !buffer) result = list->dwSize;
    else if (buffer_size >= list->dwSize) { copy_bytes(buffer, list, list->dwSize); result = list->dwSize; }
  } else if (list && candidate_strings_valid(list)) {
    const DWORD header = FIELD_OFFSET(CANDIDATELIST, dwOffset) + list->dwCount * sizeof(DWORD);
    DWORD total = header;
    for (DWORD i = 0; i < list->dwCount; ++i) {
      const WCHAR *s = (const WCHAR *)((const BYTE *)list + list->dwOffset[i]);
      const DWORD max = (list->dwSize - list->dwOffset[i]) / sizeof(WCHAR);
      DWORD n = 0;
      while (n < max && s[n]) ++n;
      total += (DWORD)ansi_length(s, n) + 1;
    }
    total = (total + 3) & ~3u;
    if (buffer_size == 0 || !buffer) result = total;
    else if (buffer_size >= total) {
      CANDIDATELIST *out = buffer;
      zero_bytes(out, total);
      out->dwSize = total;
      out->dwStyle = list->dwStyle;
      out->dwCount = list->dwCount;
      out->dwSelection = list->dwSelection;
      out->dwPageStart = list->dwPageStart;
      out->dwPageSize = list->dwPageSize;
      DWORD at = header;
      for (DWORD i = 0; i < list->dwCount; ++i) {
        const WCHAR *s = (const WCHAR *)((const BYTE *)list + list->dwOffset[i]);
        const DWORD max = (list->dwSize - list->dwOffset[i]) / sizeof(WCHAR);
        DWORD n = 0;
        while (n < max && s[n]) ++n;
        out->dwOffset[i] = at;
        const int bytes = ansi_length(s, n);
        if (bytes) WideCharToMultiByte(CP_ACP, 0, s, (int)n, (char *)out + at, bytes, NULL, NULL);
        at += (DWORD)bytes + 1;
      }
      result = total;
    }
  }
  if (info) ImmUnlockIMCC(ic->hCandInfo);
  return result;
}

EXPORT DWORD WINAPI ImmGetCandidateListW(HIMC himc, DWORD index, LPCANDIDATELIST buffer, DWORD size) { return candidate_list_size(himc, index, buffer, size, TRUE); }
EXPORT DWORD WINAPI ImmGetCandidateListA(HIMC himc, DWORD index, LPCANDIDATELIST buffer, DWORD size) { return candidate_list_size(himc, index, buffer, size, FALSE); }

static DWORD candidate_list_count(HIMC himc, LPDWORD count, BOOL wide) {
  if (!count) return 0;
  *count = 0;
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic) return 0;
  const CANDINFO *info = ImmLockIMCC(ic->hCandInfo);
  if (!info) return 0;
  DWORD total = 0;
  if (ImmGetIMCCSize(ic->hCandInfo) >= sizeof(CANDINFO) && info->dwCount <= 32) {
    *count = info->dwCount;
    ImmUnlockIMCC(ic->hCandInfo);
    for (DWORD i = 0; i < *count; ++i) total += candidate_list_size(himc, i, NULL, 0, wide);
    return total;
  }
  ImmUnlockIMCC(ic->hCandInfo);
  return total;
}
EXPORT DWORD WINAPI ImmGetCandidateListCountW(HIMC himc, LPDWORD count) { return candidate_list_count(himc, count, TRUE); }
EXPORT DWORD WINAPI ImmGetCandidateListCountA(HIMC himc, LPDWORD count) { return candidate_list_count(himc, count, FALSE); }

static DWORD guide_line(HIMC himc, DWORD what, void *buffer, DWORD buffer_size, BOOL wide) {
  INPUTCTX *ic = context_ic(himc, NULL);
  if (!ic) return 0;
  const DWORD size = ImmGetIMCCSize(ic->hGuideLine);
  const GUIDELINE_ *g = ImmLockIMCC(ic->hGuideLine);
  if (!g) return 0;
  DWORD result = 0;
  if (size >= sizeof(GUIDELINE_)) {
    switch (what) {
      case GGL_LEVEL: result = g->dwLevel; break;
      case GGL_INDEX: result = g->dwIndex; break;
      case GGL_STRING:
        if (g->dwStrLen == 0 || g->dwStrOffset > size || g->dwStrLen > (size - g->dwStrOffset) / sizeof(WCHAR)) break;
        {
          const WCHAR *text = (const WCHAR *)((const BYTE *)g + g->dwStrOffset);
          if (wide) result = (DWORD)copy_out(text, g->dwStrLen * sizeof(WCHAR), buffer, buffer_size);
          else {
            const int needed = ansi_length(text, g->dwStrLen);
            if (buffer_size == 0 || !buffer) result = (DWORD)needed;
            else {
              char *tmp = alloc_zero((SIZE_T)needed + 1);
              if (tmp) {
                WideCharToMultiByte(CP_ACP, 0, text, (int)g->dwStrLen, tmp, needed, NULL, NULL);
                result = (DWORD)copy_out(tmp, (DWORD)needed, buffer, buffer_size);
                free_block(tmp);
              }
            }
          }
        }
        break;
      case GGL_PRIVATE:
        if (g->dwPrivateSize == 0 || g->dwPrivateOffset > size || g->dwPrivateSize > size - g->dwPrivateOffset) break;
        result = (DWORD)copy_out((const BYTE *)g + g->dwPrivateOffset, g->dwPrivateSize, buffer, buffer_size);
        break;
      default: break;
    }
  }
  ImmUnlockIMCC(ic->hGuideLine);
  return result;
}
EXPORT DWORD WINAPI ImmGetGuideLineW(HIMC himc, DWORD what, LPWSTR buffer, DWORD size) { return guide_line(himc, what, buffer, size, TRUE); }
EXPORT DWORD WINAPI ImmGetGuideLineA(HIMC himc, DWORD what, LPSTR buffer, DWORD size) { return guide_line(himc, what, buffer, size, FALSE); }

/* ---- IME modules (none: the layouts that are installed have no IME) ---------------------------------------------------------------------------------------------- */
static BOOL ime_info(HKL hkl, IMEINFOEX_ *info) {
  zero_bytes(info, sizeof(*info));
  info->hkl = hkl;
  return NtUserGetImeInfoEx(info, 0);
}

EXPORT BOOL WINAPI ImmIsIME(HKL hkl) {
  IMEINFOEX_ info;
  return ime_info(hkl, &info);
}

EXPORT DWORD WINAPI ImmGetProperty(HKL hkl, DWORD what) {
  IMEINFOEX_ info;
  if (!ime_info(hkl, &info)) return 0;
  switch (what) {
    case IGP_PROPERTY: return info.ImeInfo.fdwProperty;
    case IGP_CONVERSION: return info.ImeInfo.fdwConversionCaps;
    case IGP_SENTENCE: return info.ImeInfo.fdwSentenceCaps;
    case IGP_UI: return info.ImeInfo.fdwUICaps;
    case IGP_SETCOMPSTR: return info.ImeInfo.fdwSCSCaps;
    case IGP_SELECT: return info.ImeInfo.fdwSelectCaps;
    case IGP_GETIMEVERSION: return IMEVER_0400;
    default: return 0;
  }
}

static UINT text_w(const WCHAR *s, LPWSTR out, UINT size) {
  const UINT n = (UINT)wide_length(s);
  if (!out || size == 0) return n;
  const UINT copy = n < size - 1 ? n : size - 1;
  copy_bytes(out, s, copy * sizeof(WCHAR));
  out[copy] = 0;
  return copy;
}

static UINT text_a(const WCHAR *s, LPSTR out, UINT size) {
  const UINT n = (UINT)wide_length(s);
  const int needed = n ? WideCharToMultiByte(CP_ACP, 0, s, (int)n, NULL, 0, NULL, NULL) : 0;
  if (!out || size == 0) return (UINT)needed;
  char *tmp = alloc_zero((SIZE_T)needed + 1);
  if (!tmp) return 0;
  if (needed) WideCharToMultiByte(CP_ACP, 0, s, (int)n, tmp, needed, NULL, NULL);
  const UINT copy = (UINT)needed < size - 1 ? (UINT)needed : size - 1;
  copy_bytes(out, tmp, copy);
  out[copy] = 0;
  free_block(tmp);
  return copy;
}

EXPORT UINT WINAPI ImmGetDescriptionW(HKL hkl, LPWSTR out, UINT size) { IMEINFOEX_ i; return ime_info(hkl, &i) ? text_w(i.wszImeDescription, out, size) : 0; }
EXPORT UINT WINAPI ImmGetDescriptionA(HKL hkl, LPSTR out, UINT size) { IMEINFOEX_ i; return ime_info(hkl, &i) ? text_a(i.wszImeDescription, out, size) : 0; }
EXPORT UINT WINAPI ImmGetIMEFileNameW(HKL hkl, LPWSTR out, UINT size) { IMEINFOEX_ i; return ime_info(hkl, &i) ? text_w(i.wszImeFile, out, size) : 0; }
EXPORT UINT WINAPI ImmGetIMEFileNameA(HKL hkl, LPSTR out, UINT size) { IMEINFOEX_ i; return ime_info(hkl, &i) ? text_a(i.wszImeFile, out, size) : 0; }
EXPORT UINT WINAPI CtfImmGetIMEFileName(HKL hkl, LPSTR out, UINT size) { return ImmGetIMEFileNameA(hkl, out, size); }

EXPORT BOOL WINAPI ImmGetImeInfoEx(void *info, DWORD by, void *data) {
  (void)data;
  return info ? NtUserGetImeInfoEx(info, by) : FALSE;
}

/* The functions that are the IME's: Windows hands them to the IME module of the layout; without one they do nothing. */
EXPORT BOOL WINAPI ImmNotifyIME(HIMC himc, DWORD action, DWORD index, DWORD value) { (void)action; (void)index; (void)value; (void)himc; return FALSE; }
EXPORT LRESULT WINAPI ImmEscapeW(HKL hkl, HIMC himc, UINT escape, LPVOID data) { (void)hkl; (void)himc; (void)escape; (void)data; return 0; }
EXPORT LRESULT WINAPI ImmEscapeA(HKL hkl, HIMC himc, UINT escape, LPVOID data) { (void)hkl; (void)himc; (void)escape; (void)data; return 0; }
EXPORT BOOL WINAPI ImmConfigureIMEW(HKL hkl, HWND hwnd, DWORD mode, LPVOID data) { (void)hkl; (void)hwnd; (void)mode; (void)data; return FALSE; }
EXPORT BOOL WINAPI ImmConfigureIMEA(HKL hkl, HWND hwnd, DWORD mode, LPVOID data) { (void)hkl; (void)hwnd; (void)mode; (void)data; return FALSE; }
EXPORT DWORD WINAPI ImmGetConversionListW(HKL hkl, HIMC himc, LPCWSTR s, LPCANDIDATELIST l, DWORD n, UINT f) { (void)hkl; (void)himc; (void)s; (void)l; (void)n; (void)f; return 0; }
EXPORT DWORD WINAPI ImmGetConversionListA(HKL hkl, HIMC himc, LPCSTR s, LPCANDIDATELIST l, DWORD n, UINT f) { (void)hkl; (void)himc; (void)s; (void)l; (void)n; (void)f; return 0; }
EXPORT DWORD WINAPI ImmGetImeMenuItemsW(HIMC himc, DWORD flags, DWORD type, LPIMEMENUITEMINFOW parent, LPIMEMENUITEMINFOW items, DWORD size) { (void)himc; (void)flags; (void)type; (void)parent; (void)items; (void)size; return 0; }
EXPORT DWORD WINAPI ImmGetImeMenuItemsA(HIMC himc, DWORD flags, DWORD type, LPIMEMENUITEMINFOA parent, LPIMEMENUITEMINFOA items, DWORD size) { (void)himc; (void)flags; (void)type; (void)parent; (void)items; (void)size; return 0; }
EXPORT BOOL WINAPI ImmRegisterWordW(HKL hkl, LPCWSTR reading, DWORD style, LPCWSTR string) { (void)hkl; (void)reading; (void)style; (void)string; return FALSE; }
EXPORT BOOL WINAPI ImmRegisterWordA(HKL hkl, LPCSTR reading, DWORD style, LPCSTR string) { (void)hkl; (void)reading; (void)style; (void)string; return FALSE; }
EXPORT BOOL WINAPI ImmUnregisterWordW(HKL hkl, LPCWSTR reading, DWORD style, LPCWSTR string) { (void)hkl; (void)reading; (void)style; (void)string; return FALSE; }
EXPORT BOOL WINAPI ImmUnregisterWordA(HKL hkl, LPCSTR reading, DWORD style, LPCSTR string) { (void)hkl; (void)reading; (void)style; (void)string; return FALSE; }
EXPORT UINT WINAPI ImmGetRegisterWordStyleW(HKL hkl, UINT n, LPSTYLEBUFW buffer) { (void)hkl; (void)n; (void)buffer; return 0; }
EXPORT UINT WINAPI ImmGetRegisterWordStyleA(HKL hkl, UINT n, LPSTYLEBUFA buffer) { (void)hkl; (void)n; (void)buffer; return 0; }
EXPORT UINT WINAPI ImmEnumRegisterWordW(HKL hkl, REGISTERWORDENUMPROCW proc, LPCWSTR reading, DWORD style, LPCWSTR string, LPVOID data) { (void)hkl; (void)proc; (void)reading; (void)style; (void)string; (void)data; return 0; }
EXPORT UINT WINAPI ImmEnumRegisterWordA(HKL hkl, REGISTERWORDENUMPROCA proc, LPCSTR reading, DWORD style, LPCSTR string, LPVOID data) { (void)hkl; (void)proc; (void)reading; (void)style; (void)string; (void)data; return 0; }
EXPORT HKL WINAPI ImmInstallIMEW(LPCWSTR file, LPCWSTR text) { (void)file; (void)text; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
EXPORT HKL WINAPI ImmInstallIMEA(LPCSTR file, LPCSTR text) { (void)file; (void)text; SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return NULL; }
EXPORT BOOL WINAPI ImmSimulateHotKey(HWND hwnd, DWORD id) { (void)hwnd; (void)id; return FALSE; }
EXPORT BOOL WINAPI ImmProcessKey(HWND hwnd, HKL hkl, UINT vk, LPARAM lparam, DWORD hotkey) { (void)hwnd; (void)hkl; (void)vk; (void)lparam; (void)hotkey; return FALSE; }
EXPORT BOOL WINAPI ImmTranslateMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) { (void)hwnd; (void)message; (void)wparam; (void)lparam; return FALSE; }
EXPORT LRESULT WINAPI ImmRequestMessageW(HIMC himc, WPARAM wparam, LPARAM lparam) { INPUTCTX *ic = context_ic(himc, NULL); return ic ? send_message(ic->hWnd, WM_IME_REQUEST, wparam, lparam) : 0; }
EXPORT LRESULT WINAPI ImmRequestMessageA(HIMC himc, WPARAM wparam, LPARAM lparam) { return ImmRequestMessageW(himc, wparam, lparam); }
EXPORT LRESULT WINAPI ImmSendIMEMessageExW(HWND hwnd, LPARAM lparam) { (void)hwnd; (void)lparam; return 0; }
EXPORT LRESULT WINAPI ImmSendIMEMessageExA(HWND hwnd, LPARAM lparam) { (void)hwnd; (void)lparam; return 0; }
EXPORT BOOL WINAPI ImmIMPGetIMEW(HWND hwnd, void *imep) { (void)hwnd; (void)imep; return FALSE; }
EXPORT BOOL WINAPI ImmIMPGetIMEA(HWND hwnd, void *imep) { (void)hwnd; (void)imep; return FALSE; }
EXPORT BOOL WINAPI ImmIMPSetIMEW(HWND hwnd, void *imep) { (void)hwnd; (void)imep; return FALSE; }
EXPORT BOOL WINAPI ImmIMPSetIMEA(HWND hwnd, void *imep) { (void)hwnd; (void)imep; return FALSE; }
EXPORT BOOL WINAPI ImmIMPQueryIMEW(void *imep) { (void)imep; return FALSE; }
EXPORT BOOL WINAPI ImmIMPQueryIMEA(void *imep) { (void)imep; return FALSE; }
EXPORT HWND WINAPI ImmCreateSoftKeyboard(UINT type, HWND owner, int x, int y) { (void)type; (void)owner; (void)x; (void)y; return NULL; }
EXPORT BOOL WINAPI ImmShowSoftKeyboard(HWND hwnd, int show) { (void)hwnd; (void)show; return FALSE; }
EXPORT BOOL WINAPI ImmDestroySoftKeyboard(HWND hwnd) { (void)hwnd; return FALSE; }
EXPORT LRESULT WINAPI ImmSystemHandler(HIMC himc, WPARAM wparam, LPARAM lparam) { (void)himc; (void)wparam; (void)lparam; return 0; }
EXPORT DWORD WINAPI ImmPutImeMenuItemsIntoMappedFile(HIMC himc) { (void)himc; return 0; }
EXPORT LRESULT WINAPI ImmCallImeConsoleIME(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT *result) { (void)hwnd; (void)message; (void)wparam; (void)lparam; if (result) *result = 0; return 0; }

/* The default IME window ("IME" class, made by win32k per GUI thread in Windows) does not exist here. */
EXPORT HWND WINAPI ImmGetDefaultIMEWnd(HWND hwnd) { (void)hwnd; return NULL; }

static BOOL is_ui_message(UINT message) {
  switch (message) {
    case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_COMPOSITION: case WM_IME_SETCONTEXT: case WM_IME_NOTIFY: case WM_IME_COMPOSITIONFULL:
    case WM_IME_SELECT: case 0x0287:      /* WM_IME_SYSTEM */
      return TRUE;
    default: return FALSE;
  }
}

EXPORT BOOL WINAPI ImmIsUIMessageW(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (!is_ui_message(message)) return FALSE;
  if (hwnd) send_message(hwnd, message, wparam, lparam);
  return TRUE;
}
EXPORT BOOL WINAPI ImmIsUIMessageA(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) { return ImmIsUIMessageW(hwnd, message, wparam, lparam); }

EXPORT UINT WINAPI ImmGetVirtualKey(HWND hwnd) { (void)hwnd; return VK_PROCESSKEY; }

/* ---- hot keys ------------------------------------------------------------------------------------------------------------------------------------------------------------ */
EXPORT BOOL WINAPI ImmGetHotKey(DWORD id, LPUINT modifiers, LPUINT vk, HKL *hkl) { return NtUserGetImeHotKey(id, modifiers, vk, hkl); }
EXPORT BOOL WINAPI ImmSetHotKey(DWORD id, UINT modifiers, UINT vk, HKL hkl) { return NtUserSetImeHotKey(id, modifiers, vk, hkl, 0); }

/* ---- threads and layouts ------------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT BOOL WINAPI ImmDisableIME(DWORD tid) { return NtUserDisableThreadIme(tid); }
EXPORT BOOL WINAPI ImmDisableIme(DWORD tid) { return NtUserDisableThreadIme(tid); }
EXPORT BOOL WINAPI ImmDisableLegacyIME(void) { return TRUE; }
EXPORT BOOL WINAPI ImmDisableTextFrameService(DWORD tid) { (void)tid; return TRUE; }

EXPORT BOOL WINAPI ImmActivateLayout(HKL hkl) {
  const HKL current = NtUserGetKeyboardLayout(0);
  return current == hkl || NtUserSetThreadLayoutHandles(hkl, current);
}
EXPORT BOOL WINAPI ImmLoadIME(HKL hkl) { (void)hkl; return FALSE; }
EXPORT void *WINAPI ImmLoadLayout(HKL hkl, void *info) { (void)hkl; (void)info; return NULL; }
EXPORT void WINAPI ImmFreeLayout(DWORD flags) { (void)flags; }
EXPORT void *WINAPI ImmLockImeDpi(HKL hkl) { (void)hkl; return NULL; }
EXPORT void WINAPI ImmUnlockImeDpi(void *dpi) { (void)dpi; }
EXPORT BOOL WINAPI ImmRegisterClient(void *shared, HINSTANCE module) { (void)shared; (void)module; return TRUE; }
EXPORT BOOL WINAPI ImmSetActiveContext(HWND hwnd, HIMC himc, BOOL activate) {
  (void)hwnd;
  (void)activate;
  return himc == NULL || context_ic(himc, NULL) != NULL;
}
EXPORT BOOL WINAPI ImmSetActiveContextConsoleIME(HWND hwnd, BOOL activate) { (void)hwnd; (void)activate; return TRUE; }

EXPORT DWORD WINAPI ImmGetAppCompatFlags(HIMC himc) {
  CLIENTIMC_ *client;
  if (!context_ic(himc, &client)) return 0;
  return client->app_compat;
}
EXPORT void WINAPI CtfImmSetAppCompatFlags(DWORD flags) { g_app_compat_flags = flags; }
EXPORT void WINAPI CtfImmAppCompatEnableIMEonProtectedCode(BOOL enable) { (void)enable; }

EXPORT BOOL WINAPI ImmWINNLSEnableIME(HWND hwnd, BOOL enable) {
  (void)hwnd;
  const BOOL previous = g_nls_enabled;
  g_nls_enabled = enable != 0;
  return previous;
}
EXPORT BOOL WINAPI ImmWINNLSGetEnableStatus(HWND hwnd) { (void)hwnd; return g_nls_enabled; }
EXPORT UINT WINAPI ImmWINNLSGetIMEHotkey(HWND hwnd) { (void)hwnd; return 0; }

/* The ANSI code page a keyboard layout's language uses. */
EXPORT UINT WINAPI GetKeyboardLayoutCP(WORD lang) {
  DWORD cp = 0;
  if (!GetLocaleInfoW(MAKELCID(lang, SORT_DEFAULT), LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, (LPWSTR)&cp, sizeof(cp) / sizeof(WCHAR)) || cp == 0) return GetACP();
  return cp;
}

/* SendInput belongs to user32; imm32 re-exports it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"      /* (winuser.h declares it imported: this is the definition) */
EXPORT UINT WINAPI SendInput(UINT count, LPINPUT inputs, int size) { return NtUserSendInput(count, inputs, size); }
#pragma GCC diagnostic pop

/* ---- the text services framework (Cicero) is not there ------------------------------------------------------------------------------------------------------------------- */
EXPORT BOOL WINAPI CtfImmIsCiceroEnabled(void) { return FALSE; }
EXPORT BOOL WINAPI CtfImmIsCiceroStartedInThread(void) { return g_cicero_started; }
EXPORT void WINAPI CtfImmSetCiceroStartInThread(BOOL started) { g_cicero_started = started != 0; }
EXPORT BOOL WINAPI CtfImmIsComStartedInThread(void) { return FALSE; }
EXPORT BOOL WINAPI CtfImmIsTextFrameServiceDisabled(void) { return TRUE; }
EXPORT BOOL WINAPI CtfImmIsGuidMapEnable(HIMC himc) { (void)himc; return FALSE; }
EXPORT HKL WINAPI CtfImmGetCompatibleKeyboardLayout(HKL hkl) { return hkl; }
EXPORT DWORD WINAPI CtfImmGetTMAEFlags(void) { return 0; }
EXPORT void WINAPI CtfImmEnterCoInitCountSkipMode(void) {}
EXPORT BOOL WINAPI CtfImmLeaveCoInitCountSkipMode(void) { return TRUE; }
EXPORT void WINAPI CtfImmCoUninitialize(void) {}
EXPORT void WINAPI CtfImmHideToolbarWnd(void) {}
EXPORT void WINAPI CtfImmRestoreToolbarWnd(LPVOID cookie) { (void)cookie; }
EXPORT void WINAPI CtfImmLastEnabledWndDestroy(BOOL last) { (void)last; }
EXPORT HRESULT WINAPI CtfImmTIMActivate(HKL hkl) { (void)hkl; return E_NOTIMPL; }
EXPORT LRESULT WINAPI CtfImmDispatchDefImeMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) { (void)hwnd; (void)message; (void)wparam; (void)lparam; return 0; }

/* ---- threads ending ---------------------------------------------------------------------------------------------------------------------------------------------------- */
static void free_thread_contexts(DWORD tid) {
  for (;;) {
    HIMC found = NULL;
    lock_state();
    for (DWORD i = 0; i < g_client_count; ++i) if (g_clients[i] && g_clients[i]->tid == tid) { found = g_clients[i]->himc; break; }
    unlock_state();
    if (!found) return;
    context_free(found);
  }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_THREAD_DETACH) free_thread_contexts(GetCurrentThreadId());
  return TRUE;
}
