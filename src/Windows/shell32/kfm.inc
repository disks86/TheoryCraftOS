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

/* The known-folder manager (IKnownFolderManager) and the folders it hands out (IKnownFolder), over the table in folders.inc. Folders cannot be registered or removed.
 * Included by shell32.c after link.inc. */

typedef struct { const IKnownFolderVtbl *vtbl; LONG ref; const KnownFolder *kf; } KnownFolderObject;
typedef struct { const IKnownFolderManagerVtbl *vtbl; LONG ref; } KnownFolderManagerObject;

static DWORD kf_category(const KnownFolder *kf) { return (kf->kind & KF_VIRTUAL) ? KF_CATEGORY_VIRTUAL : (kf->kind & KF_PERUSER) ? KF_CATEGORY_PERUSER : (kf->kind & KF_COMMON) ? KF_CATEGORY_COMMON : KF_CATEGORY_FIXED; }

static HRESULT STDMETHODCALLTYPE kfo_query(IKnownFolder *self, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IKnownFolder)) { *out = self; InterlockedIncrement(&((KnownFolderObject *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE kfo_addref(IKnownFolder *self) { return (ULONG)InterlockedIncrement(&((KnownFolderObject *)self)->ref); }
static ULONG STDMETHODCALLTYPE kfo_release(IKnownFolder *self) {
  KnownFolderObject *o = (KnownFolderObject *)self;
  const LONG n = InterlockedDecrement(&o->ref);
  if (!n) { mem_free(o); objects_remove(); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE kfo_id(IKnownFolder *self, KNOWNFOLDERID *id) { if (!id) return E_INVALIDARG; *id = *((KnownFolderObject *)self)->kf->id; return S_OK; }
static HRESULT STDMETHODCALLTYPE kfo_category(IKnownFolder *self, KF_CATEGORY *category) { if (!category) return E_INVALIDARG; *category = kf_category(((KnownFolderObject *)self)->kf); return S_OK; }
static HRESULT STDMETHODCALLTYPE kfo_shell_item(IKnownFolder *self, DWORD flags, REFIID riid, void **out) { return SHGetKnownFolderItem(((KnownFolderObject *)self)->kf->id, flags, NULL, riid, out); }
static HRESULT STDMETHODCALLTYPE kfo_path(IKnownFolder *self, DWORD flags, LPWSTR *path) { return SHGetKnownFolderPath(((KnownFolderObject *)self)->kf->id, flags, NULL, path); }
static HRESULT STDMETHODCALLTYPE kfo_set_path(IKnownFolder *self, DWORD flags, LPCWSTR path) { return SHSetKnownFolderPath(((KnownFolderObject *)self)->kf->id, flags, NULL, path); }
static HRESULT STDMETHODCALLTYPE kfo_idlist(IKnownFolder *self, DWORD flags, PITEMID_CHILD *pidl) { return SHGetKnownFolderIDList(((KnownFolderObject *)self)->kf->id, flags, NULL, pidl); }
static HRESULT STDMETHODCALLTYPE kfo_type(IKnownFolder *self, FOLDERTYPEID *type) { (void)self; if (type) *type = kGuidNull; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE kfo_redirection(IKnownFolder *self, KF_REDIRECTION_CAPABILITIES *caps) {
  if (!caps) return E_INVALIDARG;
  *caps = (((KnownFolderObject *)self)->kf->kind & KF_PERUSER) ? KF_REDIRECTION_CAPABILITIES_REDIRECTABLE : 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE kfo_definition(IKnownFolder *self, KNOWNFOLDER_DEFINITION *d) {
  const KnownFolder *kf = ((KnownFolderObject *)self)->kf;
  if (!d) return E_INVALIDARG;
  ZeroMemory(d, sizeof(*d));
  d->category = kf_category(kf);
  d->pszName = w_task_dup(kf->name);
  d->pszDescription = w_task_dup(L"");
  if (kf->parent) d->fidParent = *kf->parent;
  d->pszRelativePath = kf->relative ? w_task_dup(kf->relative) : NULL;
  d->pszParsingName = kf->clsid ? w_task_dup(kf->clsid) : NULL;
  d->dwAttributes = kf->attributes;
  return S_OK;
}
static const IKnownFolderVtbl g_known_folder_vtbl = {kfo_query, kfo_addref, kfo_release, kfo_id, kfo_category, kfo_shell_item, kfo_path, kfo_set_path, kfo_idlist, kfo_type, kfo_redirection, kfo_definition};

static HRESULT known_folder_object(const KnownFolder *kf, IKnownFolder **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!kf) return E_INVALIDARG;
  KnownFolderObject *o = mem_alloc(sizeof(*o));
  if (!o) return E_OUTOFMEMORY;
  o->vtbl = &g_known_folder_vtbl;
  o->ref = 1;
  o->kf = kf;
  objects_add();
  *out = (IKnownFolder *)o;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE kfm_query(IKnownFolderManager *self, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IKnownFolderManager)) { *out = self; InterlockedIncrement(&((KnownFolderManagerObject *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE kfm_addref(IKnownFolderManager *self) { return (ULONG)InterlockedIncrement(&((KnownFolderManagerObject *)self)->ref); }
static ULONG STDMETHODCALLTYPE kfm_release(IKnownFolderManager *self) {
  KnownFolderManagerObject *m = (KnownFolderManagerObject *)self;
  const LONG n = InterlockedDecrement(&m->ref);
  if (!n) { mem_free(m); objects_remove(); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE kfm_from_csidl(IKnownFolderManager *self, int csidl, KNOWNFOLDERID *id) {
  (void)self;
  const KnownFolder *kf = id ? kf_for_csidl(csidl) : NULL;
  if (!kf) return E_INVALIDARG;
  *id = *kf->id;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE kfm_to_csidl(IKnownFolderManager *self, REFKNOWNFOLDERID id, int *csidl) {
  (void)self;
  const KnownFolder *kf = kf_find(id);
  if (!csidl) return E_INVALIDARG;
  if (!kf) return E_INVALIDARG;
  *csidl = kf_csidl(kf);
  return *csidl < 0 ? E_INVALIDARG : S_OK;
}
static HRESULT STDMETHODCALLTYPE kfm_get_ids(IKnownFolderManager *self, KNOWNFOLDERID **ids, UINT *count) {
  (void)self;
  if (!ids || !count) return E_INVALIDARG;
  *count = (UINT)ARRAY_LEN(g_known_folders);
  *ids = SHAlloc(sizeof(KNOWNFOLDERID) * ARRAY_LEN(g_known_folders));
  if (!*ids) return E_OUTOFMEMORY;
  for (size_t i = 0; i < ARRAY_LEN(g_known_folders); ++i) (*ids)[i] = *g_known_folders[i].id;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE kfm_get_folder(IKnownFolderManager *self, REFKNOWNFOLDERID id, IKnownFolder **out) { (void)self; if (out) *out = NULL; const KnownFolder *kf = kf_find(id); return kf ? known_folder_object(kf, out) : E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE kfm_get_by_name(IKnownFolderManager *self, LPCWSTR name, IKnownFolder **out) {
  (void)self;
  if (out) *out = NULL;
  if (!name || !out) return E_INVALIDARG;
  for (size_t i = 0; i < ARRAY_LEN(g_known_folders); ++i) if (!wicmp(g_known_folders[i].name, name) && name[0] != '{') return known_folder_object(&g_known_folders[i], out);
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE kfm_register(IKnownFolderManager *self, REFKNOWNFOLDERID id, const KNOWNFOLDER_DEFINITION *d) { (void)self; (void)id; (void)d; return E_ACCESSDENIED; }
static HRESULT STDMETHODCALLTYPE kfm_unregister(IKnownFolderManager *self, REFKNOWNFOLDERID id) { (void)self; (void)id; return E_ACCESSDENIED; }
static HRESULT STDMETHODCALLTYPE kfm_find_path(IKnownFolderManager *self, LPCWSTR path, FFFP_MODE mode, IKnownFolder **out) {
  (void)self;
  if (out) *out = NULL;
  if (!path || !out) return E_INVALIDARG;
  const KnownFolder *best = NULL;
  size_t best_len = 0;
  for (size_t i = 0; i < ARRAY_LEN(g_known_folders); ++i) {
    WCHAR folder[MAX_PATH_EX];
    if (!kf_path(&g_known_folders[i], TRUE, folder, ARRAY_LEN(folder), 0)) continue;
    if (!wicmp(folder, path)) { best = &g_known_folders[i]; best_len = wlen(folder); break; }
    if (mode == FFFP_EXACTMATCH) continue;
    const size_t n = wlen(folder);
    if (n > best_len && wnicmp(path, folder, n) == 0 && is_sep(path[n])) { best = &g_known_folders[i]; best_len = n; }
  }
  if (!best) return S_FALSE;
  return known_folder_object(best, out);
}
static HRESULT STDMETHODCALLTYPE kfm_find_idlist(IKnownFolderManager *self, PCIDLIST_ABSOLUTE pidl, IKnownFolder **out) {
  WCHAR path[MAX_PATH_EX];
  if (out) *out = NULL;
  if (!pidl || !out) return E_INVALIDARG;
  if (!pidl_to_path(pidl, path, ARRAY_LEN(path))) return S_FALSE;
  return kfm_find_path(self, path, FFFP_EXACTMATCH, out);
}
static HRESULT STDMETHODCALLTYPE kfm_redirect(IKnownFolderManager *self, REFKNOWNFOLDERID id, HWND owner, KF_REDIRECT_FLAGS flags, LPCWSTR target, UINT exclusion_count, const KNOWNFOLDERID *exclusions, LPWSTR *error) {
  (void)owner; (void)flags; (void)exclusion_count; (void)exclusions;
  if (error) *error = NULL;
  (void)self;
  return SHSetKnownFolderPath(id, 0, NULL, target);
}
static const IKnownFolderManagerVtbl g_kfm_vtbl = {kfm_query, kfm_addref, kfm_release, kfm_from_csidl, kfm_to_csidl, kfm_get_ids, kfm_get_folder, kfm_get_by_name, kfm_register,
                                                   kfm_unregister, kfm_find_path, kfm_find_idlist, kfm_redirect};
static HRESULT known_folder_manager_create(REFIID riid, void **out) {
  KnownFolderManagerObject *m = mem_alloc(sizeof(*m));
  if (!m) return E_OUTOFMEMORY;
  m->vtbl = &g_kfm_vtbl;
  m->ref = 1;
  objects_add();
  const HRESULT hr = kfm_query((IKnownFolderManager *)m, riid, out);
  kfm_release((IKnownFolderManager *)m);
  return hr;
}
