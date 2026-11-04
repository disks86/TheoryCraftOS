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

/* imm32.dll without an IME and without windows (user32 has no window functions yet): input contexts, their status, the HIMCC blocks, and the getters over a composition string,
 * candidate list and guide line that the test puts into a context itself. Prints "Test failed" per wrong answer (scripts/winetest.sh counts them), exit code = failures.
 * Build: tests/windows/build.sh. */
#include <windows.h>
#include <imm.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { ++failures; printf("imm32test.c:%d: Test failed: %s\n", __LINE__, #c); } } while (0)

typedef struct {
  HWND hWnd; BOOL fOpen; POINT ptStatusWndPos, ptSoftKbdPos; DWORD fdwConversion, fdwSentence; LOGFONTW lfFont; COMPOSITIONFORM cfCompForm; CANDIDATEFORM cfCandForm[4];
  HANDLE hCompStr, hCandInfo, hGuideLine, hPrivate; DWORD dwNumMsgBuf; HANDLE hMsgBuf; DWORD fdwInit, dwReserve[3];
} IC;
typedef struct { DWORD f[25]; } COMP;      /* COMPOSITIONSTRING: dwSize, then (len, offset) pairs */
enum { C_SIZE, C_RDATTR_LEN, C_RDATTR_OFF, C_RDCL_LEN, C_RDCL_OFF, C_RDSTR_LEN, C_RDSTR_OFF, C_ATTR_LEN, C_ATTR_OFF, C_CL_LEN, C_CL_OFF, C_STR_LEN, C_STR_OFF, C_CURSOR, C_DELTA };
typedef struct { DWORD dwSize, dwCount, dwOffset[32], dwPrivateSize, dwPrivateOffset; } CI;
typedef struct { DWORD dwSize, dwLevel, dwIndex, dwStrLen, dwStrOffset, dwPrivateSize, dwPrivateOffset; } GL;


/* The exports that mingw's imm.h and import library do not have: found in the DLL at run time. */
#define IMM_FUNCTIONS(F) \
  F(HIMCC, ImmCreateIMCC, (DWORD)) F(HIMCC, ImmDestroyIMCC, (HIMCC)) F(LPVOID, ImmLockIMCC, (HIMCC)) F(BOOL, ImmUnlockIMCC, (HIMCC)) F(DWORD, ImmGetIMCCSize, (HIMCC)) \
  F(DWORD, ImmGetIMCCLockCount, (HIMCC)) F(HIMCC, ImmReSizeIMCC, (HIMCC, DWORD)) F(LPVOID, ImmLockIMC, (HIMC)) F(BOOL, ImmUnlockIMC, (HIMC)) F(DWORD, ImmGetIMCLockCount, (HIMC)) \
  F(LPVOID, ImmLockClientImc, (HIMC)) F(void, ImmUnlockClientImc, (LPVOID)) F(BOOL, ImmGenerateMessage, (HIMC)) F(BOOL, ImmGetHotKey, (DWORD, LPUINT, LPUINT, HKL *)) \
  F(BOOL, ImmSetHotKey, (DWORD, UINT, UINT, HKL)) F(BOOL, ImmProcessKey, (HWND, HKL, UINT, LPARAM, DWORD)) F(BOOL, ImmTranslateMessage, (HWND, UINT, WPARAM, LPARAM)) \
  F(BOOL, ImmWINNLSEnableIME, (HWND, BOOL)) F(BOOL, ImmWINNLSGetEnableStatus, (HWND)) F(UINT, GetKeyboardLayoutCP, (WORD))
#define DECLARE(ret, name, args) static ret (WINAPI *name) args;
IMM_FUNCTIONS(DECLARE)
#undef DECLARE
static BOOL load_functions(void) {
  HMODULE module = LoadLibraryA("imm32.dll");
  if (!module) return FALSE;
#define LOAD(ret, name, args) *(void **)&name = (void *)GetProcAddress(module, #name); if (!name) { printf("imm32.dll has no " #name "\n"); return FALSE; }
  IMM_FUNCTIONS(LOAD)
#undef LOAD
  return TRUE;
}

static BOOL CALLBACK count_context(HIMC himc, LPARAM param) { (void)himc; ++*(int *)param; return TRUE; }
static BOOL CALLBACK stop_context(HIMC himc, LPARAM param) { (void)himc; ++*(int *)param; return FALSE; }

static void test_imcc(void) {
  HIMCC h = ImmCreateIMCC(10);
  CHECK(h != NULL && ImmGetIMCCSize(h) == 10 && ImmGetIMCCLockCount(h) == 0);
  BYTE *p = ImmLockIMCC(h);
  CHECK(p != NULL && ImmGetIMCCLockCount(h) == 1);
  p[9] = 0x5A;
  CHECK(ImmUnlockIMCC(h) == FALSE && ImmGetIMCCLockCount(h) == 0);
  h = ImmReSizeIMCC(h, 40);
  CHECK(h != NULL && ImmGetIMCCSize(h) == 40);
  p = ImmLockIMCC(h);
  CHECK(p && p[9] == 0x5A && p[39] == 0);      /* the contents stay, the new part is zero */
  ImmLockIMCC(h);
  CHECK(ImmGetIMCCLockCount(h) == 2 && ImmUnlockIMCC(h) == TRUE && ImmUnlockIMCC(h) == FALSE);
  CHECK(ImmDestroyIMCC(h) == NULL);
  CHECK(ImmGetIMCCSize(h) == 0 && ImmLockIMCC(h) == NULL && ImmDestroyIMCC(h) == h);      /* a handle that is gone */
  HIMCC tiny = ImmCreateIMCC(0);
  CHECK(tiny != NULL && ImmGetIMCCSize(tiny) == 4);      /* a block is at least a DWORD */
  ImmDestroyIMCC(tiny);
  CHECK(ImmLockIMCC((HIMCC)(ULONG_PTR)0x1234) == NULL);
}

static void test_status(HIMC himc) {
  DWORD conversion = 99, sentence = 99;
  CHECK(ImmGetOpenStatus(himc) == FALSE);
  CHECK(ImmGetConversionStatus(himc, &conversion, &sentence) && conversion == IME_CMODE_ALPHANUMERIC && sentence == IME_SMODE_NONE);
  CHECK(ImmSetOpenStatus(himc, TRUE) && ImmGetOpenStatus(himc) == TRUE);
  CHECK(ImmSetOpenStatus(himc, 5) && ImmGetOpenStatus(himc) == TRUE);
  CHECK(ImmSetConversionStatus(himc, IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, IME_SMODE_PHRASEPREDICT));
  CHECK(ImmGetConversionStatus(himc, &conversion, NULL) && conversion == (IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE));
  CHECK(ImmGetConversionStatus(himc, NULL, &sentence) && sentence == IME_SMODE_PHRASEPREDICT);
  CHECK(!ImmGetOpenStatus(NULL) && !ImmSetOpenStatus(NULL, TRUE) && !ImmGetConversionStatus(NULL, &conversion, &sentence));

  POINT pos = {5, 6}, got = {0, 0};
  CHECK(!ImmGetStatusWindowPos(himc, &got));      /* never set */
  CHECK(ImmSetStatusWindowPos(himc, &pos) && ImmGetStatusWindowPos(himc, &got) && got.x == 5 && got.y == 6);

  COMPOSITIONFORM cf = {CFS_POINT, {10, 20}, {1, 2, 3, 4}}, cf2;
  memset(&cf2, 0, sizeof(cf2));
  CHECK(!ImmGetCompositionWindow(himc, &cf2));
  CHECK(ImmSetCompositionWindow(himc, &cf) && ImmGetCompositionWindow(himc, &cf2) && cf2.dwStyle == CFS_POINT && cf2.ptCurrentPos.x == 10 && cf2.ptCurrentPos.y == 20);

  CANDIDATEFORM cand = {2, CFS_CANDIDATEPOS, {7, 8}, {0, 0, 0, 0}}, cand2;
  memset(&cand2, 0, sizeof(cand2));
  CHECK(!ImmGetCandidateWindow(himc, 2, &cand2));
  CHECK(ImmSetCandidateWindow(himc, &cand) && ImmGetCandidateWindow(himc, 2, &cand2) && cand2.dwIndex == 2 && cand2.ptCurrentPos.x == 7);
  CHECK(!ImmGetCandidateWindow(himc, 1, &cand2) && !ImmGetCandidateWindow(himc, 4, &cand2));
  cand.dwIndex = 4;
  CHECK(!ImmSetCandidateWindow(himc, &cand));

  LOGFONTW lw, lw2;
  LOGFONTA la, la2;
  memset(&lw, 0, sizeof(lw));
  memset(&lw2, 0, sizeof(lw2));
  memset(&la, 0, sizeof(la));
  memset(&la2, 0, sizeof(la2));
  CHECK(!ImmGetCompositionFontW(himc, &lw2));
  lw.lfHeight = -14;
  lw.lfCharSet = DEFAULT_CHARSET;
  memcpy(lw.lfFaceName, L"Tahoma", sizeof(L"Tahoma"));
  CHECK(ImmSetCompositionFontW(himc, &lw) && ImmGetCompositionFontW(himc, &lw2) && lw2.lfHeight == -14 && !wcscmp(lw2.lfFaceName, L"Tahoma"));
  CHECK(ImmGetCompositionFontA(himc, &la2) && la2.lfHeight == -14 && !strcmp(la2.lfFaceName, "Tahoma"));
  la.lfHeight = -20;
  strcpy(la.lfFaceName, "Arial");
  CHECK(ImmSetCompositionFontA(himc, &la) && ImmGetCompositionFontW(himc, &lw2) && lw2.lfHeight == -20 && !wcscmp(lw2.lfFaceName, L"Arial"));

  CHECK(!ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0));      /* no IME */
  CHECK(!ImmSetCompositionStringW(himc, SCS_SETSTR, L"x", 2, NULL, 0));
  CHECK(!ImmSetCompositionStringW(himc, 0x7777, L"x", 2, NULL, 0));
  CHECK(ImmGenerateMessage(himc));      /* nothing queued */
}

static void test_composition(HIMC himc) {
  IC *ic = ImmLockIMC(himc);
  CHECK(ic != NULL && ImmGetIMCLockCount(himc) == 1);
  if (!ic) return;
  CHECK(ic->hCompStr && ic->hCandInfo && ic->hGuideLine && ic->hPrivate && ic->hMsgBuf);
  CHECK(ic->fdwConversion == IME_CMODE_NATIVE + IME_CMODE_FULLSHAPE);      /* test_status set it */
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0) == 0 && ImmGetCompositionStringA(himc, GCS_RESULTSTR, NULL, 0) == 0);      /* empty */

  /* a composition "ab" with a result "xyz": 2 clauses {0, 2}, attributes {0, 1}, cursor at 1 */
  const DWORD header = sizeof(COMP);
  HIMCC resized = ImmReSizeIMCC(ic->hCompStr, header + 64);
  CHECK(resized == ic->hCompStr);
  COMP *comp = ImmLockIMCC(ic->hCompStr);
  CHECK(comp != NULL);
  if (comp) {
    BYTE *base = (BYTE *)comp;
    comp->f[C_SIZE] = header + 64;
    comp->f[C_STR_LEN] = 2; comp->f[C_STR_OFF] = header;
    memcpy(base + header, L"ab", 4);
    comp->f[C_ATTR_LEN] = 2; comp->f[C_ATTR_OFF] = header + 8;
    base[header + 8] = 0; base[header + 9] = 1;
    comp->f[C_CL_LEN] = 8; comp->f[C_CL_OFF] = header + 12;
    ((DWORD *)(base + header + 12))[0] = 0; ((DWORD *)(base + header + 12))[1] = 2;
    comp->f[C_CURSOR] = 1;
    comp->f[C_DELTA] = 1;
    comp->f[21] = 3; comp->f[22] = header + 24;      /* dwResultStrLen / Offset */
    memcpy(base + header + 24, L"xyz", 6);
    ImmUnlockIMCC(ic->hCompStr);
  }
  WCHAR wide[8];
  char narrow[8];
  BYTE attr[8];
  DWORD clause[4];
  memset(wide, 0, sizeof(wide));
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0) == 4);
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPSTR, wide, sizeof(wide)) == 4 && !wcscmp(wide, L"ab"));
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPSTR, wide, 2) == 2);      /* a short buffer gets what fits */
  CHECK(ImmGetCompositionStringW(himc, GCS_RESULTSTR, wide, sizeof(wide)) == 6 && !wcscmp(wide, L"xyz"));
  memset(narrow, 0, sizeof(narrow));
  CHECK(ImmGetCompositionStringA(himc, GCS_COMPSTR, NULL, 0) == 2);
  CHECK(ImmGetCompositionStringA(himc, GCS_COMPSTR, narrow, sizeof(narrow)) == 2 && !strcmp(narrow, "ab"));
  CHECK(ImmGetCompositionStringA(himc, GCS_RESULTSTR, narrow, sizeof(narrow)) == 3 && !strcmp(narrow, "xyz"));
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPATTR, attr, sizeof(attr)) == 2 && attr[0] == 0 && attr[1] == 1);
  CHECK(ImmGetCompositionStringA(himc, GCS_COMPATTR, attr, sizeof(attr)) == 2 && attr[1] == 1);
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPCLAUSE, clause, sizeof(clause)) == 8 && clause[0] == 0 && clause[1] == 2);
  CHECK(ImmGetCompositionStringA(himc, GCS_COMPCLAUSE, clause, sizeof(clause)) == 8 && clause[1] == 2);
  CHECK(ImmGetCompositionStringW(himc, GCS_CURSORPOS, NULL, 0) == 1 && ImmGetCompositionStringA(himc, GCS_CURSORPOS, NULL, 0) == 1);
  CHECK(ImmGetCompositionStringW(himc, GCS_DELTASTART, NULL, 0) == 1);
  CHECK(ImmGetCompositionStringW(himc, 0x4000, NULL, 0) == 0);
  CHECK(ImmGetCompositionStringW(NULL, GCS_COMPSTR, NULL, 0) == IMM_ERROR_GENERAL);
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 8) == IMM_ERROR_GENERAL);

  /* a block that claims more than it has */
  comp = ImmLockIMCC(ic->hCompStr);
  if (comp) { comp->f[C_STR_LEN] = 1000; ImmUnlockIMCC(ic->hCompStr); }
  CHECK(ImmGetCompositionStringW(himc, GCS_COMPSTR, wide, sizeof(wide)) == IMM_ERROR_NODATA);

  /* a candidate list: two strings */
  HIMCC resized_cand = ImmReSizeIMCC(ic->hCandInfo, sizeof(CI) + 64);
  CI *info = ImmLockIMCC(ic->hCandInfo);
  CHECK(resized_cand == ic->hCandInfo && info != NULL);
  DWORD count = 77;
  CHECK(ImmGetCandidateListCountW(himc, &count) == 0 && count == 0);
  if (info) {
    BYTE *base = (BYTE *)info;
    info->dwSize = sizeof(CI) + 64;
    info->dwCount = 1;
    info->dwOffset[0] = sizeof(CI);
    CANDIDATELIST *list = (CANDIDATELIST *)(base + sizeof(CI));
    const DWORD list_header = FIELD_OFFSET(CANDIDATELIST, dwOffset) + 2 * sizeof(DWORD);
    list->dwSize = list_header + 2 * (2 * sizeof(WCHAR));
    list->dwStyle = IME_CAND_READ;
    list->dwCount = 2;
    list->dwSelection = 1;
    list->dwPageStart = 0;
    list->dwPageSize = 9;
    list->dwOffset[0] = list_header;
    list->dwOffset[1] = list_header + 4;
    memcpy((BYTE *)list + list_header, L"a", 4);
    memcpy((BYTE *)list + list_header + 4, L"b", 4);
    ImmUnlockIMCC(ic->hCandInfo);
  }
  const DWORD wide_size = FIELD_OFFSET(CANDIDATELIST, dwOffset) + 2 * sizeof(DWORD) + 8;
  CHECK(ImmGetCandidateListCountW(himc, &count) == wide_size && count == 1);
  union { CANDIDATELIST l; BYTE raw[128]; } out;
  memset(&out, 0, sizeof(out));
  CHECK(ImmGetCandidateListW(himc, 0, NULL, 0) == wide_size);
  CHECK(ImmGetCandidateListW(himc, 0, &out.l, sizeof(out)) == wide_size && out.l.dwCount == 2 && out.l.dwSelection == 1 && !wcscmp((WCHAR *)(out.raw + out.l.dwOffset[1]), L"b"));
  CHECK(ImmGetCandidateListW(himc, 0, &out.l, wide_size - 1) == 0);      /* too small */
  CHECK(ImmGetCandidateListW(himc, 1, &out.l, sizeof(out)) == 0);      /* no second list */
  memset(&out, 0, sizeof(out));
  const DWORD ansi_size = (FIELD_OFFSET(CANDIDATELIST, dwOffset) + 2 * sizeof(DWORD) + 4 + 3) & ~3u;
  CHECK(ImmGetCandidateListA(himc, 0, NULL, 0) == ansi_size);
  CHECK(ImmGetCandidateListA(himc, 0, &out.l, sizeof(out)) == ansi_size && out.l.dwCount == 2 && !strcmp((char *)(out.raw + out.l.dwOffset[0]), "a") && !strcmp((char *)(out.raw + out.l.dwOffset[1]), "b"));
  CHECK(ImmGetCandidateListCountA(himc, &count) == ansi_size && count == 1);

  /* a guide line */
  HIMCC resized_guide = ImmReSizeIMCC(ic->hGuideLine, sizeof(GL) + 16);
  GL *guide = ImmLockIMCC(ic->hGuideLine);
  CHECK(resized_guide == ic->hGuideLine && guide != NULL);
  if (guide) {
    guide->dwSize = sizeof(GL) + 16;
    guide->dwLevel = GL_LEVEL_WARNING;
    guide->dwIndex = GL_ID_TYPINGERROR;
    guide->dwStrLen = 3;
    guide->dwStrOffset = sizeof(GL);
    memcpy((BYTE *)guide + sizeof(GL), L"err", 8);
    ImmUnlockIMCC(ic->hGuideLine);
  }
  CHECK(ImmGetGuideLineW(himc, GGL_LEVEL, NULL, 0) == GL_LEVEL_WARNING && ImmGetGuideLineW(himc, GGL_INDEX, NULL, 0) == GL_ID_TYPINGERROR);
  CHECK(ImmGetGuideLineW(himc, GGL_STRING, wide, sizeof(wide)) == 6 && !wcsncmp(wide, L"err", 3));
  CHECK(ImmGetGuideLineA(himc, GGL_STRING, narrow, sizeof(narrow)) == 3 && !strncmp(narrow, "err", 3));
  CHECK(ImmGetGuideLineW(himc, GGL_PRIVATE, NULL, 0) == 0 && ImmGetGuideLineW(himc, 99, NULL, 0) == 0);

  /* a message the IME queued goes out (there is no window: nothing receives it, the queue empties) */
  struct { UINT m; WPARAM w; LPARAM l; } *queue;
  ImmReSizeIMCC(ic->hMsgBuf, 2 * 24);
  queue = ImmLockIMCC(ic->hMsgBuf);
  if (queue) { queue[0].m = WM_IME_NOTIFY; queue[0].w = IMN_OPENCANDIDATE; queue[1].m = WM_IME_ENDCOMPOSITION; ImmUnlockIMCC(ic->hMsgBuf); }
  ic->dwNumMsgBuf = 2;
  CHECK(ImmGenerateMessage(himc) && ic->dwNumMsgBuf == 0);

  CHECK(ImmUnlockIMC(himc) == FALSE && ImmGetIMCLockCount(himc) == 0);
}

int main(void) {
  if (!load_functions()) { printf("Test failed: functions\n"); return 1; }
  test_imcc();

  CHECK(ImmGetContext(NULL) == NULL);
  CHECK(ImmCreateContext() != NULL);
  HIMC a = ImmCreateContext(), b = ImmCreateContext();
  CHECK(a != NULL && b != NULL && a != b);
  CHECK(ImmLockIMC((HIMC)(ULONG_PTR)0x4321) == NULL && ImmGetIMCLockCount((HIMC)(ULONG_PTR)0x4321) == 0);
  test_status(a);
  test_composition(a);

  int n = 0;
  CHECK(ImmEnumInputContext(0, count_context, (LPARAM)&n) && n >= 3);
  int stopped = 0;
  ImmEnumInputContext(0, stop_context, (LPARAM)&stopped);
  CHECK(stopped == 1);
  int others = 0;
  CHECK(ImmEnumInputContext(GetCurrentThreadId() + 1000, count_context, (LPARAM)&others) && others == 0);

  void *client = ImmLockClientImc(a);
  CHECK(client != NULL);
  ImmUnlockClientImc(client);
  CHECK(ImmLockClientImc((HIMC)(ULONG_PTR)0x4321) == NULL);

  CHECK(ImmDestroyContext(a) && !ImmDestroyContext(a));      /* once */
  CHECK(!ImmGetOpenStatus(a) && ImmLockIMC(a) == NULL);
  CHECK(!ImmDestroyContext(NULL));
  CHECK(ImmDestroyContext(b));

  /* a context made in a thread is gone with the thread */
  /* the layouts: the US layout has no IME */
  HKL hkl = (HKL)(ULONG_PTR)0x04090409;      /* (user32 has no GetKeyboardLayout yet) */
  char text[32];
  CHECK(!ImmIsIME(hkl) && ImmGetProperty(hkl, IGP_PROPERTY) == 0 && ImmGetDescriptionA(hkl, text, sizeof(text)) == 0 && ImmGetIMEFileNameA(hkl, text, sizeof(text)) == 0);
  CHECK(!ImmConfigureIMEA(hkl, NULL, IME_CONFIG_GENERAL, NULL) && ImmEscapeA(hkl, NULL, IME_ESC_IME_NAME, text) == 0 && ImmInstallIMEA("x.ime", "x") == NULL);
  CHECK(ImmGetDefaultIMEWnd(NULL) == NULL && ImmGetVirtualKey(NULL) == VK_PROCESSKEY);
  CHECK(GetKeyboardLayoutCP(0x0409) == 1252 || GetKeyboardLayoutCP(0x0409) == GetACP());

  /* hot keys live in win32k */
  UINT modifiers = 0, vk = 0;
  HKL hotkey_layout = NULL;
  CHECK(!ImmGetHotKey(0x30, &modifiers, &vk, &hotkey_layout));
  CHECK(ImmSetHotKey(0x30, MOD_ALT, 'K', hkl) && ImmGetHotKey(0x30, &modifiers, &vk, &hotkey_layout) && modifiers == MOD_ALT && vk == 'K' && hotkey_layout == hkl);
  CHECK(ImmSetHotKey(0x30, 0, 0, NULL) && ImmGetHotKey(0x30, &modifiers, &vk, &hotkey_layout));      /* (a zero hot key is stored, as Windows does) */

  /* messages the IME owns */
  CHECK(ImmIsUIMessageW(NULL, WM_IME_NOTIFY, 0, 0) && ImmIsUIMessageA(NULL, WM_IME_SETCONTEXT, 0, 0) && !ImmIsUIMessageW(NULL, WM_USER, 0, 0) && !ImmIsUIMessageW(NULL, WM_IME_CHAR, 0, 0));
  CHECK(!ImmProcessKey(NULL, hkl, 'A', 0, 0) && !ImmTranslateMessage(NULL, WM_KEYDOWN, 'A', 0) && !ImmSimulateHotKey(NULL, 0x30));
  CHECK(ImmWINNLSGetEnableStatus(NULL) == TRUE && ImmWINNLSEnableIME(NULL, FALSE) == TRUE && ImmWINNLSGetEnableStatus(NULL) == FALSE && ImmWINNLSEnableIME(NULL, TRUE) == FALSE);

  printf("imm32test: %d check(s) failed\n", failures);
  return failures;
}
