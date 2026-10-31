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

/* Security descriptors as text (SDDL) for advapi32.dll (included by advapi32.c): ConvertStringSecurityDescriptorToSecurityDescriptorW and A. The string is
 * [O:owner][G:group][D:flags(ace)...][S:flags(ace)...]; the SID of a name that needs the domain (DA, DU, ...) and the object ACEs are not known here. The descriptor is returned
 * self-relative, in memory from LocalAlloc. */

#define SDDL_MAX_ACL_ 2048
#define SDDL_MAX_SID_ 68

static BOOL sddl_is_digit(WCHAR c) { return c >= '0' && c <= '9'; }
static BOOL sddl_is_hex(WCHAR c) { return sddl_is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
static ULONGLONG sddl_number(const WCHAR **p, BOOL *ok) {
  ULONGLONG v = 0;
  int digits = 0;
  if ((*p)[0] == '0' && ((*p)[1] | 0x20) == 'x') {
    *p += 2;
    for (; sddl_is_hex(**p); ++*p, ++digits) { v = v * 16 + (ULONGLONG)(sddl_is_digit(**p) ? **p - '0' : (**p | 0x20) - 'a' + 10); if (digits > 16) *ok = FALSE; }
  } else {
    for (; sddl_is_digit(**p); ++*p, ++digits) { v = v * 10 + (ULONGLONG)(**p - '0'); if (digits > 19) *ok = FALSE; }
  }
  if (!digits) *ok = FALSE;
  return v;
}

/* The two letter names of well-known SIDs. */
static const struct { char name[3]; const WCHAR *sid; } sddl_aliases[] = {
  {"WD", L"S-1-1-0"}, {"CO", L"S-1-3-0"}, {"CG", L"S-1-3-1"}, {"OW", L"S-1-3-4"}, {"NU", L"S-1-5-2"}, {"IU", L"S-1-5-4"}, {"SU", L"S-1-5-6"}, {"AN", L"S-1-5-7"}, {"ED", L"S-1-5-9"},
  {"PS", L"S-1-5-10"}, {"AU", L"S-1-5-11"}, {"RC", L"S-1-5-12"}, {"IS", L"S-1-5-17"}, {"SY", L"S-1-5-18"}, {"LS", L"S-1-5-19"}, {"NS", L"S-1-5-20"}, {"BA", L"S-1-5-32-544"},
  {"BU", L"S-1-5-32-545"}, {"BG", L"S-1-5-32-546"}, {"PU", L"S-1-5-32-547"}, {"AO", L"S-1-5-32-548"}, {"SO", L"S-1-5-32-549"}, {"PO", L"S-1-5-32-550"}, {"BO", L"S-1-5-32-551"},
  {"RE", L"S-1-5-32-552"}, {"RU", L"S-1-5-32-554"}, {"RD", L"S-1-5-32-555"}, {"NO", L"S-1-5-32-556"}, {"MU", L"S-1-5-32-558"}, {"LU", L"S-1-5-32-559"}, {"CY", L"S-1-5-32-569"},
};

/* A SID at *p ("S-1-5-32-544" or an alias): written to out (68 bytes at most), its length returned, 0 if it is not one. *p moves behind it. */
static DWORD sddl_sid(const WCHAR **p, BYTE *out) {
  const WCHAR *c = *p;
  if (c[0] != 'S' || c[1] != '-') {
    for (unsigned i = 0; i < sizeof(sddl_aliases) / sizeof(sddl_aliases[0]); ++i) {
      if (c[0] != (WCHAR)(BYTE)sddl_aliases[i].name[0] || c[1] != (WCHAR)(BYTE)sddl_aliases[i].name[1]) continue;
      *p = c + 2;
      const WCHAR *text = sddl_aliases[i].sid;
      return sddl_sid(&text, out);
    }
    return 0;
  }
  c += 2;
  BOOL ok = TRUE;
  const ULONGLONG revision = sddl_number(&c, &ok);
  if (!ok || revision != 1 || *c != '-') return 0;
  ++c;
  const ULONGLONG authority = sddl_number(&c, &ok);
  if (!ok || authority > 0xFFFFFFFFFFFFull) return 0;
  DWORD subs[15];
  unsigned count = 0;
  while (*c == '-') {
    ++c;
    const ULONGLONG v = sddl_number(&c, &ok);
    if (!ok || v > 0xFFFFFFFFull || count == 15) return 0;
    subs[count++] = (DWORD)v;
  }
  out[0] = 1;
  out[1] = (BYTE)count;
  for (int i = 0; i < 6; ++i) out[2 + i] = (BYTE)(authority >> (8 * (5 - i)));
  for (unsigned i = 0; i < count; ++i) memcpy(out + 8 + 4 * i, &subs[i], 4);
  *p = c;
  return 8 + 4 * count;
}

static DWORD sddl_right_code(const WCHAR *c) {
  static const struct { char name[3]; DWORD mask; } codes[] = {
    {"GA", 0x10000000}, {"GR", 0x80000000u}, {"GW", 0x40000000}, {"GX", 0x20000000}, {"RC", 0x20000}, {"SD", 0x10000}, {"WD", 0x40000}, {"WO", 0x80000},
    {"RP", 0x1}, {"WP", 0x2}, {"CC", 0x1}, {"DC", 0x2}, {"LC", 0x4}, {"SW", 0x8}, {"LO", 0x10}, {"DT", 0x40}, {"CR", 0x100},
    {"FA", 0x1F01FF}, {"FR", 0x120089}, {"FW", 0x120116}, {"FX", 0x1200A0}, {"KA", 0xF003F}, {"KR", 0x20019}, {"KW", 0x20006}, {"KX", 0x20019},
  };
  for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) if (c[0] == (WCHAR)(BYTE)codes[i].name[0] && c[1] == (WCHAR)(BYTE)codes[i].name[1]) return codes[i].mask;
  return 0;
}

/* One ACL: the flags, then the ACEs in parentheses, from *p; into acl (SDDL_MAX_ACL_ bytes). 0: it is malformed. */
static BOOL sddl_acl(const WCHAR **p, BYTE *acl, WORD *control, WORD protect, WORD required, WORD inherited, WORD null_acl) {
  const WCHAR *c = *p;
  BOOL null_flag = FALSE;
  for (;;) {      /* the flags */
    if (c[0] == 'P') { *control |= protect; ++c; }
    else if (c[0] == 'A' && c[1] == 'R') { *control |= required; c += 2; }
    else if (c[0] == 'A' && c[1] == 'I') { *control |= inherited; c += 2; }
    else if (c[0] == 'N' && c[1] == 'O' && c[2] == '_') {      /* NO_ACCESS_CONTROL */
      static const char word[] = "NO_ACCESS_CONTROL";
      for (unsigned i = 0; word[i]; ++i) if (c[i] != (WCHAR)(BYTE)word[i]) return FALSE;
      c += 17;
      null_flag = TRUE;
    } else break;
  }
  (void)null_acl;
  memset(acl, 0, 8);
  acl[0] = 2;      /* ACL_REVISION */
  DWORD size = 8;
  WORD count = 0;
  BYTE revision = 2;
  while (*c == '(') {
    ++c;
    BYTE ace[4 + 4 + SDDL_MAX_SID_];
    memset(ace, 0, sizeof(ace));
    /* type */
    BYTE type;
    if (c[0] == 'A' && c[1] == ';') { type = 0; ++c; }
    else if (c[0] == 'D' && c[1] == ';') { type = 1; ++c; }
    else if (c[0] == 'A' && c[1] == 'U' && c[2] == ';') { type = 2; c += 2; }
    else if (c[0] == 'A' && c[1] == 'L' && c[2] == ';') { type = 3; c += 2; }
    else return FALSE;      /* (the object ACEs: OA, OD, OU, OL, and the conditional and mandatory ones) */
    if (*c++ != ';') return FALSE;
    /* flags */
    BYTE flags = 0;
    for (;;) {
      if (c[0] == 'C' && c[1] == 'I') flags |= 0x02;
      else if (c[0] == 'O' && c[1] == 'I') flags |= 0x01;
      else if (c[0] == 'N' && c[1] == 'P') flags |= 0x04;
      else if (c[0] == 'I' && c[1] == 'O') flags |= 0x08;
      else if (c[0] == 'I' && c[1] == 'D') flags |= 0x10;
      else if (c[0] == 'S' && c[1] == 'A') flags |= 0x40;
      else if (c[0] == 'F' && c[1] == 'A') flags |= 0x80;
      else break;
      c += 2;
    }
    if (*c++ != ';') return FALSE;
    /* rights */
    DWORD mask = 0;
    if (sddl_is_digit(c[0])) {
      BOOL ok = TRUE;
      const ULONGLONG v = sddl_number(&c, &ok);
      if (!ok || v > 0xFFFFFFFFull) return FALSE;
      mask = (DWORD)v;
    } else {
      while (c[0] && c[0] != ';') {
        const DWORD m = sddl_right_code(c);
        if (!m) return FALSE;
        mask |= m;
        c += 2;
      }
    }
    if (c[0] != ';' || c[1] != ';' || c[2] != ';') return FALSE;      /* (no object GUID, no inherited object GUID) */
    c += 3;
    BYTE sid[SDDL_MAX_SID_];
    const DWORD sid_length = sddl_sid(&c, sid);
    if (!sid_length || *c != ')') return FALSE;
    ++c;
    const DWORD ace_size = 8 + sid_length;
    if (size + ace_size > SDDL_MAX_ACL_) return FALSE;
    ace[0] = type; ace[1] = flags; ace[2] = (BYTE)ace_size; ace[3] = (BYTE)(ace_size >> 8);
    memcpy(ace + 4, &mask, 4);
    memcpy(ace + 8, sid, sid_length);
    memcpy(acl + size, ace, ace_size);
    size += ace_size;
    ++count;
  }
  acl[0] = revision;
  acl[2] = (BYTE)size; acl[3] = (BYTE)(size >> 8);
  acl[4] = (BYTE)count; acl[5] = (BYTE)(count >> 8);
  if (null_flag) { acl[0] = 0; acl[2] = 0; acl[3] = 0; }      /* (marked: there is no ACL) */
  *p = c;
  return TRUE;
}

EXPORT BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR text, DWORD revision, PVOID *descriptor, PULONG size) {
  if (!text || !descriptor || revision != 1) { SetLastError(revision != 1 ? 1305 /* ERROR_UNKNOWN_REVISION */ : ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE owner[SDDL_MAX_SID_], group[SDDL_MAX_SID_], dacl[SDDL_MAX_ACL_], sacl[SDDL_MAX_ACL_];
  DWORD owner_size = 0, group_size = 0;
  WORD control = 0x8000;      /* SE_SELF_RELATIVE */
  BOOL have_dacl = FALSE, have_sacl = FALSE, null_dacl = FALSE, null_sacl = FALSE;
  const WCHAR *c = text;
  while (*c) {
    const WCHAR kind = c[0];
    if (c[1] != ':') { SetLastError(1336 /* ERROR_INVALID_ACL */); return FALSE; }
    c += 2;
    if (kind == 'O' || kind == 'G') {
      BYTE *to = kind == 'O' ? owner : group;
      const DWORD n = sddl_sid(&c, to);
      if (!n || (kind == 'O' ? owner_size : group_size)) { SetLastError(1337 /* ERROR_INVALID_SID */); return FALSE; }
      if (kind == 'O') owner_size = n; else group_size = n;
    } else if (kind == 'D' || kind == 'S') {
      if (kind == 'D' ? have_dacl : have_sacl) { SetLastError(1336); return FALSE; }
      WORD flag_bits = 0;
      BYTE *to = kind == 'D' ? dacl : sacl;
      if (!sddl_acl(&c, to, &flag_bits, kind == 'D' ? 0x1000 : 0x2000, kind == 'D' ? 0x100 : 0x200, kind == 'D' ? 0x400 : 0x800, 0)) { SetLastError(1336); return FALSE; }
      control |= flag_bits | (kind == 'D' ? 0x0004 : 0x0010);
      const BOOL is_null = to[0] == 0;
      if (kind == 'D') { have_dacl = TRUE; null_dacl = is_null; } else { have_sacl = TRUE; null_sacl = is_null; }
    } else { SetLastError(1336); return FALSE; }
  }
  const DWORD dacl_size = have_dacl && !null_dacl ? dacl[2] | dacl[3] << 8 : 0, sacl_size = have_sacl && !null_sacl ? sacl[2] | sacl[3] << 8 : 0;
  const DWORD total = 20 + owner_size + group_size + dacl_size + sacl_size;
  BYTE *out = LocalAlloc(0, total);
  if (!out) { SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */); return FALSE; }
  memset(out, 0, total);
  DWORD at = 20, owner_at = 0, group_at = 0, dacl_at = 0, sacl_at = 0;
  if (sacl_size) { sacl_at = at; memcpy(out + at, sacl, sacl_size); at += sacl_size; }
  if (dacl_size) { dacl_at = at; memcpy(out + at, dacl, dacl_size); at += dacl_size; }
  if (owner_size) { owner_at = at; memcpy(out + at, owner, owner_size); at += owner_size; }
  if (group_size) { group_at = at; memcpy(out + at, group, group_size); at += group_size; }
  out[0] = 1;
  memcpy(out + 2, &control, 2);
  memcpy(out + 4, &owner_at, 4);
  memcpy(out + 8, &group_at, 4);
  memcpy(out + 12, &sacl_at, 4);
  memcpy(out + 16, &dacl_at, 4);
  *descriptor = out;
  if (size) *size = total;
  return TRUE;
}

EXPORT BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(LPCSTR text, DWORD revision, PVOID *descriptor, PULONG size) {
  if (!text) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const size_t_ n = strlen(text);
  WCHAR *wide = LocalAlloc(0, (n + 1) * sizeof(WCHAR));
  if (!wide) { SetLastError(8); return FALSE; }
  for (size_t_ i = 0; i <= n; ++i) wide[i] = (BYTE)text[i];      /* (SDDL is ASCII) */
  const BOOL ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(wide, revision, descriptor, size);
  LocalFree(wide);
  return ok;
}
