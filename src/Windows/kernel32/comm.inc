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

/* kernel32.dll: serial port device control strings. BuildCommDCB* turn "9600,n,8,1" or "baud=9600 parity=n data=8 stop=1 xon=on" into a DCB (and COMMTIMEOUTS for
 * "to="). The OS has no serial hardware, so the calls that need a port (GetCommState, GetDefaultCommConfig, ...) stay stubs that fail. Ported from Wine's comm.c.
 * Included by kernel32.c. */

typedef struct {
  DWORD DCBlength; DWORD BaudRate;
  DWORD fBinary : 1, fParity : 1, fOutxCtsFlow : 1, fOutxDsrFlow : 1, fDtrControl : 2, fDsrSensitivity : 1, fTXContinueOnXoff : 1, fOutX : 1, fInX : 1, fErrorChar : 1,
        fNull : 1, fRtsControl : 2, fAbortOnError : 1, fDummy2 : 17;
  WORD wReserved, XonLim, XoffLim; BYTE ByteSize, Parity, StopBits; char XonChar, XoffChar, ErrorChar, EofChar, EvtChar; WORD wReserved1;
} DCB, *LPDCB;
typedef struct { DWORD ReadIntervalTimeout, ReadTotalTimeoutMultiplier, ReadTotalTimeoutConstant, WriteTotalTimeoutMultiplier, WriteTotalTimeoutConstant; } COMMTIMEOUTS, *LPCOMMTIMEOUTS;
enum { NOPARITY = 0, ODDPARITY = 1, EVENPARITY = 2, MARKPARITY = 3, SPACEPARITY = 4 };
enum { ONESTOPBIT = 0, ONE5STOPBITS = 1, TWOSTOPBITS = 2 };
enum { DTR_CONTROL_DISABLE = 0, DTR_CONTROL_ENABLE = 1, DTR_CONTROL_HANDSHAKE = 2 };
enum { RTS_CONTROL_DISABLE = 0, RTS_CONTROL_ENABLE = 1, RTS_CONTROL_HANDSHAKE = 2 };

static BOOL comm_prefix_ci(const WCHAR *s, const char *prefix, size_t_ n) {
  for (size_t_ i = 0; i < n; i++) if (upper(s[i]) != upper((WCHAR)prefix[i])) return FALSE;
  return TRUE;
}

static const WCHAR *comm_parse_start(const WCHAR *p) {
  if (comm_prefix_ci(p, "COM", 3)) {      /* the string may start with "COMx" and an optional ':' (any port above 0, like Windows 9x) */
    p += 3;
    if (*p < '1' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') p++;
    if (*p != ':' && *p != ' ') return NULL;
    while (*p == ' ') p++;
    if (*p == ':') { p++; while (*p == ' ') p++; }
  } else if (*p == ' ') return NULL;
  return p;
}

static const WCHAR *comm_parse_number(const WCHAR *p, DWORD *number) {
  if (*p < '0' || *p > '9') return NULL;
  DWORD v = 0;
  for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (DWORD)(*p - '0');      /* wraps like wcstoul's truncation does not matter for valid strings */
  *number = v;
  return p;
}

static const WCHAR *comm_parse_parity(const WCHAR *p, BYTE *parity) {
  switch (*p++) {
    case 'e': case 'E': *parity = EVENPARITY; break;
    case 'm': case 'M': *parity = MARKPARITY; break;
    case 'n': case 'N': *parity = NOPARITY; break;
    case 'o': case 'O': *parity = ODDPARITY; break;
    case 's': case 'S': *parity = SPACEPARITY; break;
    default: return NULL;
  }
  return p;
}

static const WCHAR *comm_parse_byte_size(const WCHAR *p, BYTE *size) {
  DWORD v;
  if (!(p = comm_parse_number(p, &v)) || v < 5 || v > 8) return NULL;
  *size = (BYTE)v;
  return p;
}

static const WCHAR *comm_parse_stop_bits(const WCHAR *p, BYTE *stop) {
  DWORD v;
  if (p[0] == '1' && p[1] == '.' && p[2] == '5') { *stop = ONE5STOPBITS; return p + 3; }
  if (!(p = comm_parse_number(p, &v))) return NULL;
  if (v == 1) *stop = ONESTOPBIT; else if (v == 2) *stop = TWOSTOPBITS; else return NULL;
  return p;
}

static const WCHAR *comm_parse_on_off(const WCHAR *p, DWORD *onoff) {
  if (comm_prefix_ci(p, "on", 2)) { *onoff = 1; return p + 2; }
  if (comm_prefix_ci(p, "off", 3)) { *onoff = 0; return p + 3; }
  return NULL;
}

static void comm_skip_spaces(const WCHAR **p) { while (**p == ' ') (*p)++; }

/* The old style: "96,n,8,1" with an optional flow control letter. */
static BOOL comm_build_old(const WCHAR *d, DCB *dcb) {
  WCHAR last = 0;
  DWORD baud;
  if (!(d = comm_parse_number(d, &baud))) return FALSE;
  switch (baud) {
    case 11: case 30: case 60: baud *= 10; break;
    case 12: case 24: case 48: case 96: baud *= 100; break;
    case 19: baud = 19200; break;
  }
  dcb->BaudRate = baud;
  comm_skip_spaces(&d); if (*d++ != ',') return FALSE; comm_skip_spaces(&d);
  if (!(d = comm_parse_parity(d, &dcb->Parity))) return FALSE;
  comm_skip_spaces(&d); if (*d++ != ',') return FALSE; comm_skip_spaces(&d);
  if (!(d = comm_parse_byte_size(d, &dcb->ByteSize))) return FALSE;
  comm_skip_spaces(&d); if (*d++ != ',') return FALSE; comm_skip_spaces(&d);
  if (!(d = comm_parse_stop_bits(d, &dcb->StopBits))) return FALSE;
  comm_skip_spaces(&d);
  if (*d == ',') {      /* the flow control letter is optional */
    d++; comm_skip_spaces(&d);
    if (*d) last = *d++;
    comm_skip_spaces(&d);
  }
  switch (last) {      /* Windows NT sets the flow control members from the last parameter (or the lack of it) */
    case 0:
      dcb->fInX = FALSE; dcb->fOutX = FALSE; dcb->fOutxCtsFlow = FALSE; dcb->fOutxDsrFlow = FALSE;
      dcb->fDtrControl = DTR_CONTROL_ENABLE; dcb->fRtsControl = RTS_CONTROL_ENABLE; break;
    case 'x': case 'X':
      dcb->fInX = TRUE; dcb->fOutX = TRUE; dcb->fOutxCtsFlow = FALSE; dcb->fOutxDsrFlow = FALSE;
      dcb->fDtrControl = DTR_CONTROL_ENABLE; dcb->fRtsControl = RTS_CONTROL_ENABLE; break;
    case 'p': case 'P':
      dcb->fInX = FALSE; dcb->fOutX = FALSE; dcb->fOutxCtsFlow = TRUE; dcb->fOutxDsrFlow = TRUE;
      dcb->fDtrControl = DTR_CONTROL_HANDSHAKE; dcb->fRtsControl = RTS_CONTROL_HANDSHAKE; break;
    default: return FALSE;
  }
  return *d == 0;
}

/* The new style: "baud=9600 parity=n data=8 stop=1 xon=on to=on". */
static BOOL comm_build_new(const WCHAR *d, DCB *dcb, COMMTIMEOUTS *timeouts) {
  DWORD t;
  BOOL baud = FALSE, stop = FALSE;
  while (*d) {
    comm_skip_spaces(&d);
    if (comm_prefix_ci(d, "baud=", 5)) { baud = TRUE; if (!(d = comm_parse_number(d + 5, &dcb->BaudRate))) return FALSE; }
    else if (comm_prefix_ci(d, "parity=", 7)) { if (!(d = comm_parse_parity(d + 7, &dcb->Parity))) return FALSE; }
    else if (comm_prefix_ci(d, "data=", 5)) { if (!(d = comm_parse_byte_size(d + 5, &dcb->ByteSize))) return FALSE; }
    else if (comm_prefix_ci(d, "stop=", 5)) { stop = TRUE; if (!(d = comm_parse_stop_bits(d + 5, &dcb->StopBits))) return FALSE; }
    else if (comm_prefix_ci(d, "to=", 3)) {
      if (!(d = comm_parse_on_off(d + 3, &t))) return FALSE;
      timeouts->ReadIntervalTimeout = 0; timeouts->ReadTotalTimeoutMultiplier = 0; timeouts->ReadTotalTimeoutConstant = 0;
      timeouts->WriteTotalTimeoutMultiplier = 0; timeouts->WriteTotalTimeoutConstant = t ? 60000 : 0;
    }
    else if (comm_prefix_ci(d, "xon=", 4)) { if (!(d = comm_parse_on_off(d + 4, &t))) return FALSE; dcb->fOutX = t; dcb->fInX = t; }
    else if (comm_prefix_ci(d, "odsr=", 5)) { if (!(d = comm_parse_on_off(d + 5, &t))) return FALSE; dcb->fOutxDsrFlow = t; }
    else if (comm_prefix_ci(d, "octs=", 5)) { if (!(d = comm_parse_on_off(d + 5, &t))) return FALSE; dcb->fOutxCtsFlow = t; }
    else if (comm_prefix_ci(d, "dtr=", 4)) { if (!(d = comm_parse_on_off(d + 4, &t))) return FALSE; dcb->fDtrControl = t; }
    else if (comm_prefix_ci(d, "rts=", 4)) { if (!(d = comm_parse_on_off(d + 4, &t))) return FALSE; dcb->fRtsControl = t; }
    else if (comm_prefix_ci(d, "idsr=", 5)) { if (!(d = comm_parse_on_off(d + 5, &t))) return FALSE; dcb->fDsrSensitivity = t; }
    else return FALSE;
    if (*d && *d != ' ') return FALSE;
  }
  if (!stop) dcb->StopBits = (baud && dcb->BaudRate == 110) ? TWOSTOPBITS : ONESTOPBIT;      /* a default is always supplied */
  return TRUE;
}

EXPORT BOOL WINAPI BuildCommDCBAndTimeoutsW(LPCWSTR device, LPDCB out, LPCOMMTIMEOUTS out_timeouts) {
  DCB dcb;
  COMMTIMEOUTS timeouts;
  memset(&timeouts, 0, sizeof(timeouts));
  out->DCBlength = sizeof(DCB);
  dcb = *out;      /* work on copies: a malformed string leaves the originals as they were */
  if (out_timeouts) timeouts = *out_timeouts;
  const WCHAR *p = device ? comm_parse_start(device) : NULL;
  BOOL ok;
  if (!p) ok = FALSE;
  else {
    const WCHAR *c = p;
    while (*c && *c != ',') c++;
    ok = *c == ',' ? comm_build_old(p, &dcb) : comm_build_new(p, &dcb, &timeouts);
  }
  if (!ok) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *out = dcb;
  if (out_timeouts) *out_timeouts = timeouts;
  return TRUE;
}

EXPORT BOOL WINAPI BuildCommDCBW(LPCWSTR device, LPDCB dcb) { return BuildCommDCBAndTimeoutsW(device, dcb, NULL); }

EXPORT BOOL WINAPI BuildCommDCBAndTimeoutsA(LPCSTR device, LPDCB dcb, LPCOMMTIMEOUTS timeouts) {
  if (!device) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const int n = MultiByteToWideChar(CP_ACP, 0, device, -1, NULL, 0);
  WCHAR *w = HeapAlloc(GetProcessHeap(), 0, (size_t_)n * sizeof(WCHAR));
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  MultiByteToWideChar(CP_ACP, 0, device, -1, w, n);
  const BOOL r = BuildCommDCBAndTimeoutsW(w, dcb, timeouts);
  const DWORD e = TEB_LAST_ERROR(NtCurrentTeb());
  HeapFree(GetProcessHeap(), 0, w);
  set_error(e);
  return r;
}

EXPORT BOOL WINAPI BuildCommDCBA(LPCSTR device, LPDCB dcb) { return BuildCommDCBAndTimeoutsA(device, dcb, NULL); }

/* No handle is a serial port, so every call that takes a port fails the way Windows does for a handle of another kind (a file, the console, a pipe). The default
 * configuration of a port that does not exist is an invalid parameter. */
static BOOL comm_not_a_port(void) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
#define COMM_NO_PORT(name, params) EXPORT BOOL WINAPI name params { return comm_not_a_port(); }
COMM_NO_PORT(ClearCommBreak, (HANDLE h))
COMM_NO_PORT(ClearCommError, (HANDLE h, LPDWORD errors, PVOID stat))
COMM_NO_PORT(EscapeCommFunction, (HANDLE h, DWORD func))
COMM_NO_PORT(GetCommConfig, (HANDLE h, PVOID config, LPDWORD size))
COMM_NO_PORT(GetCommMask, (HANDLE h, LPDWORD mask))
COMM_NO_PORT(GetCommModemStatus, (HANDLE h, LPDWORD status))
COMM_NO_PORT(GetCommProperties, (HANDLE h, PVOID props))
COMM_NO_PORT(GetCommState, (HANDLE h, LPDCB dcb))
COMM_NO_PORT(GetCommTimeouts, (HANDLE h, LPCOMMTIMEOUTS timeouts))
COMM_NO_PORT(PurgeComm, (HANDLE h, DWORD flags))
COMM_NO_PORT(SetCommBreak, (HANDLE h))
COMM_NO_PORT(SetCommConfig, (HANDLE h, PVOID config, DWORD size))
COMM_NO_PORT(SetCommMask, (HANDLE h, DWORD mask))
COMM_NO_PORT(SetCommState, (HANDLE h, LPDCB dcb))
COMM_NO_PORT(SetCommTimeouts, (HANDLE h, LPCOMMTIMEOUTS timeouts))
COMM_NO_PORT(SetupComm, (HANDLE h, DWORD in, DWORD out))
COMM_NO_PORT(TransmitCommChar, (HANDLE h, char c))
COMM_NO_PORT(WaitCommEvent, (HANDLE h, LPDWORD mask, LPOVERLAPPED overlapped))

EXPORT BOOL WINAPI GetDefaultCommConfigW(LPCWSTR name, PVOID config, LPDWORD size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI GetDefaultCommConfigA(LPCSTR name, PVOID config, LPDWORD size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI SetDefaultCommConfigW(LPCWSTR name, PVOID config, DWORD size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI SetDefaultCommConfigA(LPCSTR name, PVOID config, DWORD size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI CommConfigDialogW(LPCWSTR name, HANDLE wnd, PVOID config) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI CommConfigDialogA(LPCSTR name, HANDLE wnd, PVOID config) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
