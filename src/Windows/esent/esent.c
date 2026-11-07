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

/* esent.dll for TheoryCraftOS: the Extensible Storage Engine (JET). There is no database engine here: every call fails with JET_errFeatureNotAvailable and clears the
 * handles and counts it would give back, so a program reading an ESE database (Chrome imports Edge's and the old browsers' data from them) finds none. Built
 * freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

typedef LONG JET_ERR;
typedef ULONG_PTR JET_INSTANCE, JET_SESID, JET_TABLEID, JET_API_PTR;
typedef ULONG JET_DBID, JET_COLUMNID, JET_GRBIT;
#define JET_errFeatureNotAvailable ((JET_ERR)-1001)

EXPORT JET_ERR WINAPI JetCreateInstanceW(JET_INSTANCE *instance, const WCHAR *name) { (void)name; if (instance) *instance = 0; return JET_errFeatureNotAvailable; }
EXPORT JET_ERR WINAPI JetSetSystemParameterW(JET_INSTANCE *instance, JET_SESID session, ULONG parameter, JET_API_PTR value, const WCHAR *text) {
  (void)instance; (void)session; (void)parameter; (void)value; (void)text;
  return JET_errFeatureNotAvailable;
}
EXPORT JET_ERR WINAPI JetInit(JET_INSTANCE *instance) { (void)instance; return JET_errFeatureNotAvailable; }
EXPORT JET_ERR WINAPI JetTerm(JET_INSTANCE instance) { (void)instance; return JET_errFeatureNotAvailable; }
EXPORT JET_ERR WINAPI JetBeginSessionW(JET_INSTANCE instance, JET_SESID *session, const WCHAR *user, const WCHAR *password) {
  (void)instance; (void)user; (void)password;
  if (session) *session = 0;
  return JET_errFeatureNotAvailable;
}
EXPORT JET_ERR WINAPI JetAttachDatabase2W(JET_SESID session, const WCHAR *file, ULONG max_pages, JET_GRBIT flags) {
  (void)session; (void)file; (void)max_pages; (void)flags;
  return JET_errFeatureNotAvailable;
}
EXPORT JET_ERR WINAPI JetOpenDatabaseW(JET_SESID session, const WCHAR *file, const WCHAR *connect, JET_DBID *database, JET_GRBIT flags) {
  (void)session; (void)file; (void)connect; (void)flags;
  if (database) *database = 0;
  return JET_errFeatureNotAvailable;
}
EXPORT JET_ERR WINAPI JetOpenTableW(JET_SESID session, JET_DBID database, const WCHAR *name, const void *parameters, ULONG parameters_size, JET_GRBIT flags,
                                    JET_TABLEID *table) {
  (void)session; (void)database; (void)name; (void)parameters; (void)parameters_size; (void)flags;
  if (table) *table = 0;
  return JET_errFeatureNotAvailable;
}
EXPORT JET_ERR WINAPI JetCloseTable(JET_SESID session, JET_TABLEID table) { (void)session; (void)table; return JET_errFeatureNotAvailable; }
EXPORT JET_ERR WINAPI JetGetTableColumnInfoW(JET_SESID session, JET_TABLEID table, const WCHAR *column, void *result, ULONG result_size, ULONG level) {
  (void)session; (void)table; (void)column; (void)result; (void)result_size; (void)level;
  return JET_errFeatureNotAvailable;
}
EXPORT JET_ERR WINAPI JetMove(JET_SESID session, JET_TABLEID table, LONG rows, JET_GRBIT flags) { (void)session; (void)table; (void)rows; (void)flags; return JET_errFeatureNotAvailable; }
EXPORT JET_ERR WINAPI JetRetrieveColumn(JET_SESID session, JET_TABLEID table, JET_COLUMNID column, void *data, ULONG data_size, ULONG *actual, JET_GRBIT flags,
                                        void *info) {
  (void)session; (void)table; (void)column; (void)data; (void)data_size; (void)flags; (void)info;
  if (actual) *actual = 0;
  return JET_errFeatureNotAvailable;
}
