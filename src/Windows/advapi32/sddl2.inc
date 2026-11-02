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
/* Security descriptors as text (SDDL), both ways. Included by kbsec.c. The string is [O:owner][G:group][D:flags(ace)...][S:flags(ace)...]; an ACE is
 * (type;flags;rights;object guid;inherited object guid;account sid). */

#define SDDL_MAX_ACL 0xFFF8

/* ---- the words of SDDL ----------------------------------------------------------------------------------------------------------------------------------- */

static const struct { char name[3]; DWORD value; } sddl_rights[] = {
    {"CC", 0x1}, {"DC", 0x2}, {"LC", 0x4}, {"SW", 0x8}, {"RP", 0x10}, {"WP", 0x20}, {"DT", 0x40}, {"LO", 0x80}, {"CR", 0x100},
    {"SD", 0x10000}, {"RC", 0x20000}, {"WD", 0x40000}, {"WO", 0x80000},
    {"GA", 0x10000000}, {"GX", 0x20000000}, {"GW", 0x40000000}, {"GR", 0x80000000},
    {"NR", 0x2}, {"NW", 0x1}, {"NX", 0x4},
    {"FA", 0x1f01ff}, {"FR", 0x120089}, {"FW", 0x120116}, {"FX", 0x1200a0},
    {"KA", 0xf003f}, {"KR", 0x20019}, {"KW", 0x20006}, {"KX", 0x20019},
};
#define SDDL_BIT_RIGHTS 17      /* the first entries: one right each (the mandatory label ones follow) */
static const struct { char name[3]; BYTE value; } sddl_ace_flags[] = {
    {"OI", OBJECT_INHERIT_ACE}, {"CI", CONTAINER_INHERIT_ACE}, {"NP", NO_PROPAGATE_INHERIT_ACE}, {"IO", INHERIT_ONLY_ACE}, {"ID", INHERITED_ACE}, {"SA", SUCCESSFUL_ACCESS_ACE_FLAG}, {"FA", FAILED_ACCESS_ACE_FLAG},
    {"TP", 0x40}, {"CR", 0x80},
};
static const struct { char name[3]; BYTE value; BOOL object; } sddl_ace_types[] = {
    {"A", ACCESS_ALLOWED_ACE_TYPE, 0}, {"D", ACCESS_DENIED_ACE_TYPE, 0}, {"AU", SYSTEM_AUDIT_ACE_TYPE, 0}, {"AL", SYSTEM_ALARM_ACE_TYPE, 0},
    {"OA", ACCESS_ALLOWED_OBJECT_ACE_TYPE, 1}, {"OD", ACCESS_DENIED_OBJECT_ACE_TYPE, 1}, {"OU", SYSTEM_AUDIT_OBJECT_ACE_TYPE, 1}, {"OL", SYSTEM_ALARM_OBJECT_ACE_TYPE, 1},
    {"ML", SYSTEM_MANDATORY_LABEL_ACE_TYPE, 0},
};

static BOOL sddl_ws(WCHAR c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static const WCHAR *sddl_skip(const WCHAR *p)
{
    while (sddl_ws(*p)) ++p;
    return p;
}
static BOOL sddl_alpha(WCHAR c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* The SID at p: "S-1-..." or a two letter alias; stops at ')' ';' or the end, for SIDs of owner and group at the next letter followed by ':'. */
static const WCHAR *sddl_parse_sid(const WCHAR *p, BYTE *out, BOOL *ok)
{
    SID *sid = (SID *)out;
    *ok = FALSE;
    if ((p[0] == 'S' || p[0] == 's') && p[1] == '-') {
        const WCHAR *end = parse_text_sid(p, sid);
        if (!end) return p;
        *ok = TRUE;
        return end;
    }
    if (alias_to_sid(p, sid)) { *ok = TRUE; return p + 2; }
    return p;
}

static BOOL sddl_parse_guid(const WCHAR **pp, GUID *guid, BOOL *present)
{
    const WCHAR *p = sddl_skip(*pp);
    *present = FALSE;
    if (*p == ';' || *p == ')') { *pp = p; return TRUE; }
    if (*p == '{') ++p;
    DWORD parts[11];
    static const int widths[11] = {8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2};
    for (int i = 0; i < 11; ++i) {
        DWORD v = 0;
        if (i == 3 && *p == '-') ++p;
        else if (i > 0 && i < 3 && *p == '-') ++p;
        else if (i == 1 || i == 2) return FALSE;
        for (int d = 0; d < widths[i]; ++d, ++p) {
            int x;
            if (*p >= '0' && *p <= '9') x = *p - '0';
            else if (*p >= 'a' && *p <= 'f') x = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') x = *p - 'A' + 10;
            else return FALSE;
            v = v * 16 + (DWORD)x;
        }
        parts[i] = v;
        if (i == 3 && *p == '-') {}
    }
    if (*pp[0] == '{') { if (*p != '}') return FALSE; ++p; }
    guid->Data1 = parts[0];
    guid->Data2 = (WORD)parts[1];
    guid->Data3 = (WORD)parts[2];
    for (int i = 0; i < 8; ++i) guid->Data4[i] = (BYTE)parts[3 + i];
    *pp = p;
    *present = TRUE;
    return TRUE;
}

/* The ACL of "D:" or "S:" at *pp: the flags then the ACEs. acl gets room for SDDL_MAX_ACL bytes. */
static DWORD sddl_parse_acl(const WCHAR **pp, ACL *acl, WORD *control, WORD protect, WORD req, WORD inherited, WORD null_acl, BOOL *is_null)
{
    const WCHAR *p = *pp;
    BYTE *free_at;
    *is_null = FALSE;
    acl->AclRevision = ACL_REVISION;
    acl->Sbz1 = 0;
    acl->AceCount = 0;
    acl->Sbz2 = 0;
    acl->AclSize = sizeof(ACL);
    free_at = (BYTE *)(acl + 1);
    for (;;) {
        p = sddl_skip(p);
        if (*p == 'P') { *control |= protect; ++p; }
        else if (p[0] == 'A' && p[1] == 'R') { *control |= req; p += 2; }
        else if (p[0] == 'A' && p[1] == 'I') { *control |= inherited; p += 2; }
        else if (!wcsncmp(p, L"NO_ACCESS_CONTROL", 17)) { *is_null = TRUE; *control |= null_acl; p += 17; }
        else break;
    }
    for (p = sddl_skip(p); *p == '('; p = sddl_skip(p)) {
        BYTE ace_type = 0, ace_flags = 0;
        DWORD rights = 0;
        GUID object = {0}, inherit = {0};
        BOOL has_object = FALSE, has_inherit = FALSE, object_ace = FALSE, ok;
        BYTE sid[sizeof(SID) + 16 * sizeof(DWORD)];
        int i;
        ++p;
        p = sddl_skip(p);
        for (i = 0; i < (int)ARRAY_SIZE(sddl_ace_types); ++i) {
            const char *n = sddl_ace_types[i].name;
            if (p[0] == (WCHAR)n[0] && (n[1] ? p[1] == (WCHAR)n[1] : !sddl_alpha(p[1]))) { p += n[1] ? 2 : 1; break; }
        }
        if (i == (int)ARRAY_SIZE(sddl_ace_types)) return ERROR_INVALID_ACL;
        ace_type = sddl_ace_types[i].value;
        object_ace = sddl_ace_types[i].object;
        if (*p++ != ';') return ERROR_INVALID_ACL;
        for (;;) {      /* flags */
            p = sddl_skip(p);
            for (i = 0; i < (int)ARRAY_SIZE(sddl_ace_flags); ++i)
                if (p[0] == (WCHAR)sddl_ace_flags[i].name[0] && p[1] == (WCHAR)sddl_ace_flags[i].name[1]) break;
            if (i == (int)ARRAY_SIZE(sddl_ace_flags)) break;
            ace_flags |= sddl_ace_flags[i].value;
            p += 2;
        }
        if (*p++ != ';') return ERROR_INVALID_ACL;
        p = sddl_skip(p);
        for (;;) {      /* the rights: numbers (hexadecimal 0x, octal 0, decimal) and two letter names, together */
            if (*p >= '0' && *p <= '9') {
                ULONGLONG v = 0;
                int digits = 0;
                if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                    p += 2;
                    for (;; ++p, ++digits) {
                        int x;
                        if (*p >= '0' && *p <= '9') x = *p - '0';
                        else if (*p >= 'a' && *p <= 'f') x = *p - 'a' + 10;
                        else if (*p >= 'A' && *p <= 'F') x = *p - 'A' + 10;
                        else break;
                        v = v * 16 + (ULONGLONG)x;
                        if (v >> 32) return ERROR_INVALID_ACL;
                    }
                    if (!digits) return ERROR_INVALID_ACL;
                } else if (p[0] == '0') {
                    for (; *p >= '0' && *p <= '7'; ++p) { v = v * 8 + (ULONGLONG)(*p - '0'); if (v >> 32) return ERROR_INVALID_ACL; }
                } else {
                    for (; *p >= '0' && *p <= '9'; ++p) { v = v * 10 + (ULONGLONG)(*p - '0'); if (v >> 32) return ERROR_INVALID_ACL; }
                }
                rights |= (DWORD)v;
                continue;
            }
            for (i = 0; i < (int)ARRAY_SIZE(sddl_rights); ++i)
                if (p[0] == (WCHAR)sddl_rights[i].name[0] && p[1] == (WCHAR)sddl_rights[i].name[1]) break;
            if (i == (int)ARRAY_SIZE(sddl_rights)) break;
            rights |= sddl_rights[i].value;
            p += 2;
        }
        if (*p++ != ';') return ERROR_INVALID_ACL;
        if (!sddl_parse_guid(&p, &object, &has_object)) return ERROR_INVALID_ACL;
        if (*p++ != ';') return ERROR_INVALID_ACL;
        if (!sddl_parse_guid(&p, &inherit, &has_inherit)) return ERROR_INVALID_ACL;
        if (*p++ != ';') return ERROR_INVALID_ACL;
        p = sddl_skip(p);
        p = sddl_parse_sid(p, sid, &ok);
        if (!ok) return ERROR_INVALID_ACL;
        p = sddl_skip(p);
        if (*p == ';') {      /* a resource attribute: not kept */
            while (*p && *p != ')') ++p;
        }
        if (*p++ != ')') return ERROR_INVALID_ACL;
        if (has_object || has_inherit) object_ace = TRUE;
        if (!object_ace && (ace_type == ACCESS_ALLOWED_OBJECT_ACE_TYPE)) object_ace = TRUE;
        {
            DWORD sid_length = RtlLengthSid(sid), size = 8 + sid_length, flags = 0;
            BYTE *ace = free_at;
            if (object_ace) {
                size += 4 + (has_object ? 16 : 0) + (has_inherit ? 16 : 0);
                flags = (has_object ? ACE_OBJECT_TYPE_PRESENT : 0) | (has_inherit ? ACE_INHERITED_OBJECT_TYPE_PRESENT : 0);
                acl->AclRevision = ACL_REVISION_DS;
                if (ace_type < ACCESS_ALLOWED_OBJECT_ACE_TYPE && ace_type == ACCESS_ALLOWED_ACE_TYPE) ace_type = ACCESS_ALLOWED_OBJECT_ACE_TYPE;
            } else if (ace_type == SYSTEM_MANDATORY_LABEL_ACE_TYPE) acl->AclRevision = ACL_REVISION_DS;
            if ((DWORD)acl->AclSize + size > SDDL_MAX_ACL) return ERROR_INVALID_ACL;
            memset(ace, 0, size);
            ace[0] = ace_type;
            ace[1] = ace_flags;
            *(WORD *)(ace + 2) = (WORD)size;
            *(DWORD *)(ace + 4) = rights;
            BYTE *at = ace + 8;
            if (object_ace) {
                *(DWORD *)at = flags;
                at += 4;
                if (has_object) { memcpy(at, &object, 16); at += 16; }
                if (has_inherit) { memcpy(at, &inherit, 16); at += 16; }
            }
            memcpy(at, sid, sid_length);
            free_at += size;
            acl->AclSize = (WORD)(acl->AclSize + size);
            ++acl->AceCount;
        }
    }
    *pp = p;
    return ERROR_SUCCESS;
}

EXP BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR text, DWORD revision, PSECURITY_DESCRIPTOR *descriptor, PULONG size)
{
    BYTE owner[sizeof(SID) + 16 * sizeof(DWORD)], group[sizeof(SID) + 16 * sizeof(DWORD)];
    BOOL have_owner = FALSE, have_group = FALSE, have_dacl = FALSE, have_sacl = FALSE, dacl_null = FALSE, sacl_null = FALSE;
    WORD control = 0;
    ACL *dacl = NULL, *sacl = NULL;
    DWORD error = ERROR_SUCCESS;
    const WCHAR *p;
    SECURITY_DESCRIPTOR_RELATIVE *out = NULL;
    if (!text || !descriptor) return win_fail(ERROR_INVALID_PARAMETER);
    if (revision != SDDL_REVISION_1) return win_fail(ERROR_UNKNOWN_REVISION);
    dacl = malloc(SDDL_MAX_ACL);
    sacl = malloc(SDDL_MAX_ACL);
    if (!dacl || !sacl) { free(dacl); free(sacl); return win_fail(ERROR_NOT_ENOUGH_MEMORY); }
    p = text;
    while (error == ERROR_SUCCESS) {
        p = sddl_skip(p);
        if (!*p) break;
        if (p[1] != ':') { error = ERROR_INVALID_PARAMETER; break; }
        const WCHAR part = p[0];
        p += 2;
        if (part == 'O' || part == 'G') {
            BOOL ok;
            BYTE *dest = part == 'O' ? owner : group;
            if (part == 'O' ? have_owner : have_group) { error = ERROR_INVALID_PARAMETER; break; }
            p = sddl_skip(p);
            p = sddl_parse_sid(p, dest, &ok);
            if (!ok) { error = ERROR_INVALID_PARAMETER; break; }
            if (part == 'O') have_owner = TRUE; else have_group = TRUE;
        } else if (part == 'D') {
            if (have_dacl) { error = ERROR_INVALID_PARAMETER; break; }
            have_dacl = TRUE;
            error = sddl_parse_acl(&p, dacl, &control, SE_DACL_PROTECTED, SE_DACL_AUTO_INHERIT_REQ, SE_DACL_AUTO_INHERITED, 0, &dacl_null);
            if (error) { error = ERROR_INVALID_ACL; break; }
        } else if (part == 'S') {
            if (have_sacl) { error = ERROR_INVALID_PARAMETER; break; }
            have_sacl = TRUE;
            error = sddl_parse_acl(&p, sacl, &control, SE_SACL_PROTECTED, SE_SACL_AUTO_INHERIT_REQ, SE_SACL_AUTO_INHERITED, 0, &sacl_null);
            if (error) { error = ERROR_INVALID_ACL; break; }
        } else { error = ERROR_INVALID_PARAMETER; break; }
    }
    if (!error) {
        DWORD length = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
        if (have_owner) length += RtlLengthSid(owner);
        if (have_group) length += RtlLengthSid(group);
        if (have_dacl && !dacl_null) length += dacl->AclSize;
        if (have_sacl && !sacl_null) length += sacl->AclSize;
        out = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, length);
        if (!out) error = ERROR_NOT_ENOUGH_MEMORY;
        else {
            DWORD at = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
            out->Revision = SECURITY_DESCRIPTOR_REVISION;
            out->Control = control | SE_SELF_RELATIVE;
            if (have_owner) { DWORD n = RtlLengthSid(owner); memcpy((BYTE *)out + at, owner, n); out->Owner = at; at += n; }
            if (have_group) { DWORD n = RtlLengthSid(group); memcpy((BYTE *)out + at, group, n); out->Group = at; at += n; }
            if (have_dacl) {
                out->Control |= SE_DACL_PRESENT;
                if (!dacl_null) { memcpy((BYTE *)out + at, dacl, dacl->AclSize); out->Dacl = at; at += dacl->AclSize; }
            }
            if (have_sacl) {
                out->Control |= SE_SACL_PRESENT;
                if (!sacl_null) { memcpy((BYTE *)out + at, sacl, sacl->AclSize); out->Sacl = at; at += sacl->AclSize; }
            }
            *descriptor = out;
            if (size) *size = length;
        }
    }
    free(dacl);
    free(sacl);
    return error ? win_fail(error) : TRUE;
}
/* ---- as text ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { WCHAR *text; DWORD length, capacity; BOOL failed; } sddl_out;
static void sddl_put(sddl_out *o, const WCHAR *s, DWORD n)
{
    if (o->failed) return;
    if (o->length + n + 1 > o->capacity) {
        DWORD capacity = o->capacity ? o->capacity * 2 : 256;
        while (capacity < o->length + n + 1) capacity *= 2;
        WCHAR *grown = malloc(capacity * sizeof(WCHAR));
        if (!grown) { o->failed = TRUE; return; }
        if (o->text) { memcpy(grown, o->text, o->length * sizeof(WCHAR)); free(o->text); }
        o->text = grown;
        o->capacity = capacity;
    }
    memcpy(o->text + o->length, s, n * sizeof(WCHAR));
    o->length += n;
    o->text[o->length] = 0;
}
static void sddl_puts(sddl_out *o, const char *s)
{
    WCHAR w[2];
    for (; *s; ++s) { w[0] = (WCHAR)*s; sddl_put(o, w, 1); }
}
static void sddl_put_hex(sddl_out *o, DWORD v, int digits)
{
    WCHAR w[8];
    for (int i = 0; i < digits; ++i) w[i] = (WCHAR)"0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15];
    sddl_put(o, w, (DWORD)digits);
}
static void sddl_put_sid(sddl_out *o, PSID sid)
{
    const char *name;
    if (!RtlValidSid(sid)) { o->failed = TRUE; return; }
    if ((name = sddl_sid_name(sid))) { sddl_puts(o, name); return; }
    UNICODE_STRING text;
    if (RtlConvertSidToUnicodeString(&text, sid, TRUE)) { o->failed = TRUE; return; }
    sddl_put(o, text.Buffer, text.Length / sizeof(WCHAR));
    RtlFreeUnicodeString(&text);
}
static void sddl_put_rights(sddl_out *o, DWORD mask, BOOL label)
{
    WCHAR hex[16];
    if (mask == 0) return;
    if (label) {
        for (int i = 17; i < 20; ++i) if (mask & sddl_rights[i].value) { sddl_puts(o, sddl_rights[i].name); mask &= ~sddl_rights[i].value; }
        if (mask) { sddl_puts(o, "0x"); sddl_put_hex(o, mask, 1); }
        return;
    }
    for (int i = 0; i < (int)ARRAY_SIZE(sddl_rights); ++i) {
        if (i >= 17 && i < 20) continue;
        if (sddl_rights[i].value == mask) { sddl_puts(o, sddl_rights[i].name); return; }
    }
    DWORD named = 0;
    for (int i = 0; i < SDDL_BIT_RIGHTS; ++i) named |= sddl_rights[i].value;
    if (mask & ~named) {
        int n = 0;
        DWORD v = mask;
        int digits = 1;
        while (digits < 8 && (v >> (4 * digits))) ++digits;
        hex[n++] = '0';
        hex[n++] = 'x';
        sddl_put(o, hex, (DWORD)n);
        sddl_put_hex(o, mask, digits);
        return;
    }
    for (int i = 0; i < SDDL_BIT_RIGHTS; ++i)
        if (mask & sddl_rights[i].value) sddl_puts(o, sddl_rights[i].name);
}
static void sddl_put_guid(sddl_out *o, const GUID *g)
{
    sddl_put_hex(o, g->Data1, 8);
    sddl_puts(o, "-");
    sddl_put_hex(o, g->Data2, 4);
    sddl_puts(o, "-");
    sddl_put_hex(o, g->Data3, 4);
    sddl_puts(o, "-");
    sddl_put_hex(o, ((DWORD)g->Data4[0] << 8) | g->Data4[1], 4);
    sddl_puts(o, "-");
    for (int i = 2; i < 8; ++i) sddl_put_hex(o, g->Data4[i], 2);
}
static BOOL sddl_put_acl(sddl_out *o, const ACL *acl, WORD control, WORD protect, WORD req, WORD inherited)
{
    if (control & protect) sddl_puts(o, "P");
    if (control & req) sddl_puts(o, "AR");
    if (control & inherited) sddl_puts(o, "AI");
    if (!acl) { sddl_puts(o, "NO_ACCESS_CONTROL"); return TRUE; }
    if (!RtlValidAcl((ACL *)acl)) return FALSE;
    const ACE_HEADER *ace = (const ACE_HEADER *)(acl + 1);
    for (int n = 0; n < acl->AceCount; ++n, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize)) {
        int i;
        const BYTE *body = (const BYTE *)ace + 8;
        const GUID *object = NULL, *inherit = NULL;
        PSID sid;
        for (i = 0; i < (int)ARRAY_SIZE(sddl_ace_types); ++i) if (sddl_ace_types[i].value == ace->AceType) break;
        if (i == (int)ARRAY_SIZE(sddl_ace_types)) return FALSE;
        sddl_puts(o, "(");
        sddl_puts(o, sddl_ace_types[i].name);
        sddl_puts(o, ";");
        for (int f = 0; f < 7; ++f) if (ace->AceFlags & sddl_ace_flags[f].value) sddl_puts(o, sddl_ace_flags[f].name);
        sddl_puts(o, ";");
        sddl_put_rights(o, *(const DWORD *)((const BYTE *)ace + 4), ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE);
        sddl_puts(o, ";");
        if (sddl_ace_types[i].object) {
            DWORD flags = *(const DWORD *)body;
            body += 4;
            if (flags & ACE_OBJECT_TYPE_PRESENT) { object = (const GUID *)body; body += 16; }
            if (flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) { inherit = (const GUID *)body; body += 16; }
        }
        if (object) sddl_put_guid(o, object);
        sddl_puts(o, ";");
        if (inherit) sddl_put_guid(o, inherit);
        sddl_puts(o, ";");
        sid = (PSID)body;
        sddl_put_sid(o, sid);
        sddl_puts(o, ")");
    }
    return TRUE;
}

EXP BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorW(PSECURITY_DESCRIPTOR sd, DWORD revision, SECURITY_INFORMATION info, LPWSTR *string, PULONG length)
{
    sddl_out o = {0};
    PSID owner, group;
    PACL dacl, sacl;
    BOOLEAN present, defaulted;
    SECURITY_DESCRIPTOR_CONTROL control;
    DWORD rev;
    if (!sd || !string) return win_fail(ERROR_INVALID_PARAMETER);
    if (revision != SDDL_REVISION_1) return win_fail(ERROR_UNKNOWN_REVISION);
    if (!RtlValidSecurityDescriptor(sd)) return win_fail(ERROR_INVALID_SECURITY_DESCR);
    RtlGetControlSecurityDescriptor(sd, &control, &rev);
    if (info & OWNER_SECURITY_INFORMATION) {
        RtlGetOwnerSecurityDescriptor(sd, &owner, &defaulted);
        if (owner) { sddl_puts(&o, "O:"); sddl_put_sid(&o, owner); }
    }
    if (info & GROUP_SECURITY_INFORMATION) {
        RtlGetGroupSecurityDescriptor(sd, &group, &defaulted);
        if (group) { sddl_puts(&o, "G:"); sddl_put_sid(&o, group); }
    }
    if (info & DACL_SECURITY_INFORMATION) {
        RtlGetDaclSecurityDescriptor(sd, &present, &dacl, &defaulted);
        if (present) {
            sddl_puts(&o, "D:");
            if (!sddl_put_acl(&o, dacl, control, SE_DACL_PROTECTED, SE_DACL_AUTO_INHERIT_REQ, SE_DACL_AUTO_INHERITED)) { free(o.text); return win_fail(ERROR_INVALID_ACL); }
        }
    }
    if (info & SACL_SECURITY_INFORMATION) {
        RtlGetSaclSecurityDescriptor(sd, &present, &sacl, &defaulted);
        if (present) {
            sddl_puts(&o, "S:");
            if (!sddl_put_acl(&o, sacl, control, SE_SACL_PROTECTED, SE_SACL_AUTO_INHERIT_REQ, SE_SACL_AUTO_INHERITED)) { free(o.text); return win_fail(ERROR_INVALID_ACL); }
        }
    }
    if (o.failed) { free(o.text); return win_fail(ERROR_NOT_ENOUGH_MEMORY); }
    if (!o.text) sddl_put(&o, L"", 0);
    if (o.failed || !o.text) { free(o.text); o.text = NULL; }
    WCHAR *out = LocalAlloc(LMEM_FIXED, (o.length + 1) * sizeof(WCHAR));
    if (!out) { free(o.text); return win_fail(ERROR_NOT_ENOUGH_MEMORY); }
    if (o.text) memcpy(out, o.text, o.length * sizeof(WCHAR));
    out[o.length] = 0;
    free(o.text);
    *string = out;
    if (length) *length = o.length + 1;
    return TRUE;
}
