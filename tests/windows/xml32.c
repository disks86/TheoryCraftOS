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

/* MSXML's document object model (msxml3.dll, msxml6.dll: src/Windows/msxml3) as a 32-bit Windows program uses it, for tests/wow64_qemu.sh: every
 * DOMDocument class through CoCreateInstance, an update response of the kind Chromium's updater parses walked the way it walks one, entities, CDATA,
 * comments, namespaces, white space, serialization, simple paths, files, and documents that are not well formed - which must fail with a parse error
 * and leave the document usable. Ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -o xml32.exe tests/windows/xml32.c -lole32 -loleaut32 */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <ole2.h>
#include <msxml2.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "xml32: ok: %s\n" : "xml32: FAILED: %s\n", what);
  if (ok) ++g_passed; else ++g_failed;
}

/* (the identifiers, spelled out: the program is not linked with uuid) */
#define DOM_IID(low) {0x2933BF00 | (low), 0x7B36, 0x11D2, {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}}
static const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidDispatch = {0x00020400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIidNode = DOM_IID(0x80), kIidDocument = DOM_IID(0x81), kIidElement = DOM_IID(0x86), kIidText = DOM_IID(0x87), kIidDocument2 = DOM_IID(0x95);
static const struct { GUID clsid; const char *name; } kClasses[] = {
  {{0x2933BF90, 0x7B36, 0x11D2, {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}}, "DOMDocument"},
  {{0x2933BF91, 0x7B36, 0x11D2, {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}}, "FreeThreadedDOMDocument"},
  {{0xF6D90F11, 0x9C73, 0x11D3, {0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4}}, "DOMDocument (MSXML 2)"},
  {{0xF6D90F12, 0x9C73, 0x11D3, {0xB3, 0x2E, 0x00, 0xC0, 0x4F, 0x99, 0x0B, 0xB4}}, "FreeThreadedDOMDocument (MSXML 2)"},
  {{0xF5078F1B, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}}, "DOMDocument26"},
  {{0xF5078F1C, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}}, "FreeThreadedDOMDocument26"},
  {{0xF5078F32, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}}, "DOMDocument30"},
  {{0xF5078F33, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}}, "FreeThreadedDOMDocument30"},
  {{0x88D969C0, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}}, "DOMDocument40"},
  {{0x88D969C1, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}}, "FreeThreadedDOMDocument40"},
  {{0x88D96A05, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}}, "DOMDocument60"},
  {{0x88D96A06, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}}, "FreeThreadedDOMDocument60"},
};

/* ---- helpers: each returns what it read and releases what it used on the way ---------------------------------------------------------------------- */

static IXMLDOMDocument *new_document(void) {
  IXMLDOMDocument *d = NULL;
  if (FAILED(CoCreateInstance(&kClasses[0].clsid, NULL, CLSCTX_INPROC_SERVER, &kIidDocument, (void **)&d))) return NULL;
  return d;
}
/* loadXML: 1 if the document was taken (S_OK and VARIANT_TRUE), 0 if it was refused the proper way (S_FALSE and VARIANT_FALSE), -1 for anything else. */
static int load_n(IXMLDOMDocument *d, const wchar_t *xml, size_t length) {
  BSTR text = SysAllocStringLen(xml, (UINT)length);
  VARIANT_BOOL ok = 77;
  const HRESULT hr = IXMLDOMDocument_loadXML(d, text, &ok);
  SysFreeString(text);
  if (hr == S_OK && ok == VARIANT_TRUE) return 1;
  if (hr == S_FALSE && ok == VARIANT_FALSE) return 0;
  return -1;
}
static int load(IXMLDOMDocument *d, const wchar_t *xml) { return load_n(d, xml, wcslen(xml)); }
/* Is the string `expected`? (NULL: is there none?) Frees it. */
static int is(BSTR text, const wchar_t *expected) {
  const int same = expected ? text && SysStringLen(text) == wcslen(expected) && memcmp(text, expected, wcslen(expected) * sizeof(wchar_t)) == 0 : text == NULL;
  SysFreeString(text);
  return same;
}
static IXMLDOMNode *root_of(IXMLDOMDocument *d) {
  IXMLDOMElement *e = NULL;
  IXMLDOMNode *n = NULL;
  if (IXMLDOMDocument_get_documentElement(d, &e) != S_OK || !e) return NULL;
  IXMLDOMElement_QueryInterface(e, &kIidNode, (void **)&n);
  IXMLDOMElement_Release(e);
  return n;
}
static long count_of(IXMLDOMNode *n) {
  IXMLDOMNodeList *list = NULL;
  long count = -1;
  if (!n || IXMLDOMNode_get_childNodes(n, &list) != S_OK || !list) return -1;
  IXMLDOMNodeList_get_length(list, &count);
  IXMLDOMNodeList_Release(list);
  return count;
}
static IXMLDOMNode *child_of(IXMLDOMNode *n, long index) {
  IXMLDOMNodeList *list = NULL;
  IXMLDOMNode *child = NULL;
  if (!n || IXMLDOMNode_get_childNodes(n, &list) != S_OK || !list) return NULL;
  IXMLDOMNodeList_get_item(list, index, &child);
  IXMLDOMNodeList_Release(list);
  return child;
}
static BSTR name_of(IXMLDOMNode *n) { BSTR s = NULL; if (n) IXMLDOMNode_get_nodeName(n, &s); return s; }
static BSTR base_of(IXMLDOMNode *n) { BSTR s = NULL; if (n) IXMLDOMNode_get_baseName(n, &s); return s; }
static BSTR prefix_of(IXMLDOMNode *n) { BSTR s = NULL; if (n) IXMLDOMNode_get_prefix(n, &s); return s; }
static BSTR uri_of(IXMLDOMNode *n) { BSTR s = NULL; if (n) IXMLDOMNode_get_namespaceURI(n, &s); return s; }
static BSTR text_of(IXMLDOMNode *n) { BSTR s = NULL; if (n) IXMLDOMNode_get_text(n, &s); return s; }
static BSTR xml_of(IXMLDOMNode *n) { BSTR s = NULL; if (n) IXMLDOMNode_get_xml(n, &s); return s; }
static int type_of(IXMLDOMNode *n) { DOMNodeType t = 0; if (n) IXMLDOMNode_get_nodeType(n, &t); return (int)t; }
/* nodeValue as a string; NULL if it is not one (VT_NULL for an element). */
static BSTR value_of(IXMLDOMNode *n) {
  VARIANT v;
  VariantInit(&v);
  if (!n || FAILED(IXMLDOMNode_get_nodeValue(n, &v)) || V_VT(&v) != VT_BSTR) { VariantClear(&v); return NULL; }
  return V_BSTR(&v);
}
static IXMLDOMNode *attribute_node(IXMLDOMNode *n, const wchar_t *name) {
  IXMLDOMNamedNodeMap *map = NULL;
  IXMLDOMNode *a = NULL;
  BSTR b = SysAllocString(name);
  if (n && IXMLDOMNode_get_attributes(n, &map) == S_OK && map) {
    IXMLDOMNamedNodeMap_getNamedItem(map, b, &a);
    IXMLDOMNamedNodeMap_Release(map);
  }
  SysFreeString(b);
  return a;
}
/* The value of an attribute the way the updater reads one: attributes, getNamedItem, nodeValue. NULL if the element has none of that name. */
static BSTR attribute_of(IXMLDOMNode *n, const wchar_t *name) {
  IXMLDOMNode *a = attribute_node(n, name);
  BSTR value = value_of(a);
  if (a) IXMLDOMNode_Release(a);
  return value;
}
/* The first child element called `name`; with `release`, the node it was looked up in is released. */
static IXMLDOMNode *element_of(IXMLDOMNode *n, const wchar_t *name, int release) {
  IXMLDOMNode *c = NULL, *next = NULL;
  if (n) IXMLDOMNode_get_firstChild(n, &c);
  while (c) {
    if (type_of(c) == NODE_ELEMENT && is(name_of(c), name)) break;
    next = NULL;
    IXMLDOMNode_get_nextSibling(c, &next);
    IXMLDOMNode_Release(c);
    c = next;
  }
  if (n && release) IXMLDOMNode_Release(n);
  return c;
}
static void release(IXMLDOMNode *n) { if (n) IXMLDOMNode_Release(n); }
static IXMLDOMNode *select_one(IXMLDOMNode *n, const wchar_t *path) {
  IXMLDOMNode *found = NULL;
  BSTR b = SysAllocString(path);
  if (n) IXMLDOMNode_selectSingleNode(n, b, &found);
  SysFreeString(b);
  return found;
}
static long select_count(IXMLDOMNode *n, const wchar_t *path) {
  IXMLDOMNodeList *list = NULL;
  long count = -1;
  BSTR b = SysAllocString(path);
  if (n && SUCCEEDED(IXMLDOMNode_selectNodes(n, b, &list)) && list) {
    IXMLDOMNodeList_get_length(list, &count);
    IXMLDOMNodeList_Release(list);
  }
  SysFreeString(b);
  return count;
}
/* The parse error's code after a load (0: none). */
static long error_code(IXMLDOMDocument *d) {
  IXMLDOMParseError *e = NULL;
  long code = 0;
  if (FAILED(IXMLDOMDocument_get_parseError(d, &e)) || !e) return -1;
  IXMLDOMParseError_get_errorCode(e, &code);
  IXMLDOMParseError_Release(e);
  return code;
}
/* A document that is not well formed: refused the proper way, with an error code, no root element afterwards - and the next document loads. */
static int refused(IXMLDOMDocument *d, const wchar_t *xml, size_t length) {
  IXMLDOMElement *e = (IXMLDOMElement *)1;
  if (load_n(d, xml, length) != 0) return 0;
  if (error_code(d) == 0) return 0;
  if (IXMLDOMDocument_get_documentElement(d, &e) != S_FALSE || e) return 0;
  if (load(d, L"<again x='1'/>") != 1 || error_code(d) != 0) return 0;
  IXMLDOMNode *root = root_of(d);
  const int good = is(name_of(root), L"again") && is(attribute_of(root, L"x"), L"1");
  release(root);
  return good;
}

static const wchar_t kResponse[] =
  L"<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\"><systemrequirements platform=\"win\" arch=\"x64\" min_os_version=\"10.0.0\"/>"
  L"<app appid=\"{8A69D345-D564-463C-AFF1-A69D9E530F96}\" status=\"ok\"><updatecheck status=\"ok\"><urls><url codebase=\"http://dl.google.com/edgedl/chrome/install/\"/></urls>"
  L"<manifest version=\"158.0.8127.37\"><packages><package hash_sha256=\"abc\" name=\"158.0.8127.37_chrome_installer.exe\" required=\"true\" size=\"160657320\"/></packages>"
  L"<actions><action arguments=\"--verbose-logging --do-not-launch-chrome --channel=stable\" event=\"install\" needsadmin=\"true\" run=\"158.0.8127.37_chrome_installer.exe\"/>"
  L"<action event=\"postinstall\" onsuccess=\"exitsilentlyonlaunchcmd\"/></actions></manifest></updatecheck>"
  L"<data index=\"verboselogging\" name=\"install\" status=\"ok\">{\"distribution\":{\"verbose_logging\":true}}</data></app></response>";

static void test_classes(void) {
  char what[160];
  for (size_t i = 0; i < sizeof(kClasses) / sizeof(kClasses[0]); ++i) {
    IXMLDOMDocument *d = NULL;
    IXMLDOMDocument2 *d2 = NULL;
    IXMLDOMNode *n = NULL;
    IDispatch *dispatch = NULL;
    IUnknown *unknown = NULL, *unknown2 = NULL;
    const HRESULT hr = CoCreateInstance(&kClasses[i].clsid, NULL, CLSCTX_INPROC_SERVER, &kIidDocument, (void **)&d);
    int good = hr == S_OK && d != NULL;
    if (good) {
      good = IXMLDOMDocument_QueryInterface(d, &kIidDocument2, (void **)&d2) == S_OK && d2 && IXMLDOMDocument_QueryInterface(d, &kIidNode, (void **)&n) == S_OK && n &&
             IXMLDOMDocument_QueryInterface(d, &kIidDispatch, (void **)&dispatch) == S_OK && dispatch && IXMLDOMDocument_QueryInterface(d, &kIidUnknown, (void **)&unknown) == S_OK &&
             unknown && IXMLDOMDocument2_QueryInterface(d2, &kIidUnknown, (void **)&unknown2) == S_OK && unknown == unknown2 &&
             IXMLDOMDocument_QueryInterface(d, &kIidElement, (void **)&unknown2) == E_NOINTERFACE;
      good = good && load(d, L"<r a='1'>t</r>") == 1 && type_of(n) == NODE_DOCUMENT && is(name_of(n), L"#document");
      if (unknown) { IUnknown_Release(unknown); IUnknown_Release(unknown); }
      if (dispatch) IDispatch_Release(dispatch);
      if (n) IXMLDOMNode_Release(n);
      if (d2) IXMLDOMDocument2_Release(d2);
      IXMLDOMDocument_Release(d);
    }
    snprintf(what, sizeof(what), "CoCreateInstance(CLSID_%s): a document with IXMLDOMDocument, IXMLDOMDocument2, IXMLDOMNode, IDispatch (0x%08lX)", kClasses[i].name, (unsigned long)hr);
    check(good, what);
  }
  /* The libraries by name, as a program that loads them itself finds them. */
  static const char *const libraries[] = {"msxml3.dll", "msxml6.dll"};
  for (int i = 0; i < 2; ++i) {
    HMODULE module = LoadLibraryA(libraries[i]);
    typedef HRESULT (WINAPI *GetClassObject)(REFCLSID, REFIID, void **);
    GetClassObject entry = module ? (GetClassObject)(void *)GetProcAddress(module, "DllGetClassObject") : NULL;
    IClassFactory *factory = NULL;
    IXMLDOMDocument *d = NULL;
    int good = entry && entry(&kClasses[i == 0 ? 6 : 10].clsid, &kIidClassFactory, (void **)&factory) == S_OK && factory;
    good = good && IClassFactory_CreateInstance(factory, NULL, &kIidDocument, (void **)&d) == S_OK && d && load(d, L"<x/>") == 1;
    if (d) IXMLDOMDocument_Release(d);
    if (factory) IClassFactory_Release(factory);
    snprintf(what, sizeof(what), "LoadLibrary(%s), DllGetClassObject, IClassFactory::CreateInstance", libraries[i]);
    check(good, what);
  }
}

static void test_settings(IXMLDOMDocument *d) {
  IXMLDOMDocument2 *d2 = NULL;
  VARIANT_BOOL b = 5;
  VARIANT v;
  IXMLDOMElement *e = (IXMLDOMElement *)1;
  check(IXMLDOMDocument_get_documentElement(d, &e) == S_FALSE && e == NULL && count_of((IXMLDOMNode *)d) == 0, "a new document is empty: no documentElement (S_FALSE), no children");
  check(error_code(d) == 0, "and its parse error says no error");
  check(IXMLDOMDocument_put_async(d, VARIANT_FALSE) == S_OK && IXMLDOMDocument_get_async(d, &b) == S_OK && b == VARIANT_FALSE, "put_async(VARIANT_FALSE)");
  check(IXMLDOMDocument_put_resolveExternals(d, VARIANT_FALSE) == S_OK && IXMLDOMDocument_get_resolveExternals(d, &b) == S_OK && b == VARIANT_FALSE, "put_resolveExternals(VARIANT_FALSE)");
  check(IXMLDOMDocument_put_validateOnParse(d, VARIANT_FALSE) == S_OK && IXMLDOMDocument_get_validateOnParse(d, &b) == S_OK && b == VARIANT_FALSE, "put_validateOnParse(VARIANT_FALSE)");
  check(IXMLDOMDocument_put_preserveWhiteSpace(d, VARIANT_FALSE) == S_OK && IXMLDOMDocument_get_preserveWhiteSpace(d, &b) == S_OK && b == VARIANT_FALSE, "put_preserveWhiteSpace(VARIANT_FALSE)");
  IXMLDOMDocument_QueryInterface(d, &kIidDocument2, (void **)&d2);
  BSTR name = SysAllocString(L"SelectionLanguage");
  V_VT(&v) = VT_BSTR;
  V_BSTR(&v) = SysAllocString(L"XPath");
  check(d2 && IXMLDOMDocument2_setProperty(d2, name, v) == S_OK, "IXMLDOMDocument2::setProperty(SelectionLanguage, XPath)");
  VariantClear(&v);
  SysFreeString(name);
  if (d2) IXMLDOMDocument2_Release(d2);
}

/* The update response, read as chrome/updater/win/protocol_parser_xml.cc reads one. */
static void test_response(IXMLDOMDocument *d) {
  check(load(d, kResponse) == 1 && error_code(d) == 0, "loadXML takes an update response (S_OK, VARIANT_TRUE, no parse error)");
  IXMLDOMNode *response = root_of(d);
  check(response && is(base_of(response), L"response") && is(name_of(response), L"response") && type_of(response) == NODE_ELEMENT, "documentElement: an element with baseName response");
  check(is(attribute_of(response, L"protocol"), L"3.0"), "its protocol attribute, through attributes, getNamedItem and nodeValue");
  check(is(attribute_of(response, L"nothing"), NULL), "an attribute it does not have: none");
  {
    IXMLDOMNamedNodeMap *map = NULL;
    IXMLDOMNode *a = (IXMLDOMNode *)1;
    BSTR b = SysAllocString(L"nothing");
    IXMLDOMNode_get_attributes(response, &map);
    check(map && IXMLDOMNamedNodeMap_getNamedItem(map, b, &a) == S_FALSE && a == NULL, "getNamedItem of a missing attribute is S_FALSE and NULL");
    if (map) IXMLDOMNamedNodeMap_Release(map);
    SysFreeString(b);
  }
  VARIANT v;
  VariantInit(&v);
  check(IXMLDOMNode_get_nodeValue(response, &v) == S_FALSE && V_VT(&v) == VT_NULL, "an element's nodeValue is VT_NULL (S_FALSE)");
  check(count_of(response) == 2, "response has two children");

  /* (the children by index, each an element: the loop of the updater) */
  IXMLDOMNodeList *children = NULL;
  long length = 0;
  int elements = 0;
  IXMLDOMNode_get_childNodes(response, &children);
  if (children) IXMLDOMNodeList_get_length(children, &length);
  for (long i = 0; children && i < length; ++i) {
    IXMLDOMNode *c = NULL;
    if (IXMLDOMNodeList_get_item(children, i, &c) == S_OK && c && type_of(c) == NODE_ELEMENT && is(base_of(c), i == 0 ? L"systemrequirements" : L"app")) ++elements;
    release(c);
  }
  IXMLDOMNode *past = (IXMLDOMNode *)1, *next = NULL;
  check(elements == 2 && children && IXMLDOMNodeList_get_item(children, 2, &past) == S_FALSE && past == NULL, "childNodes: get_length, get_item by index (systemrequirements, app), nothing past the end");
  int walked = 0;
  if (children) {
    while (IXMLDOMNodeList_nextNode(children, &next) == S_OK && next) { ++walked; IXMLDOMNode_Release(next); }
    IXMLDOMNodeList_reset(children);
    if (IXMLDOMNodeList_nextNode(children, &next) == S_OK && next) { walked += is(name_of(next), L"systemrequirements") ? 10 : 0; IXMLDOMNode_Release(next); }
    IXMLDOMNodeList_Release(children);
  }
  check(walked == 12, "childNodes: nextNode to the end, reset, nextNode again");

  IXMLDOMNode *requirements = child_of(response, 0);
  check(is(attribute_of(requirements, L"platform"), L"win") && is(attribute_of(requirements, L"arch"), L"x64") && is(attribute_of(requirements, L"min_os_version"), L"10.0.0") &&
        count_of(requirements) == 0, "systemrequirements: platform, arch, min_os_version; no children");
  VARIANT_BOOL has = 5;
  check(IXMLDOMNode_hasChildNodes(requirements, &has) == S_FALSE && has == VARIANT_FALSE && IXMLDOMNode_hasChildNodes(response, &has) == S_OK && has == VARIANT_TRUE, "hasChildNodes");
  IXMLDOMNode *app = child_of(response, 1);
  check(is(attribute_of(app, L"appid"), L"{8A69D345-D564-463C-AFF1-A69D9E530F96}") && is(attribute_of(app, L"status"), L"ok"), "app: appid, status");
  IXMLDOMNode *updatecheck = element_of(app, L"updatecheck", 0);
  check(is(attribute_of(updatecheck, L"status"), L"ok"), "updatecheck: status");
  IXMLDOMNode *url = element_of(element_of(updatecheck, L"urls", 0), L"url", 1);
  check(is(attribute_of(url, L"codebase"), L"http://dl.google.com/edgedl/chrome/install/"), "urls/url: codebase");
  IXMLDOMNode *manifest = element_of(updatecheck, L"manifest", 0);
  check(is(attribute_of(manifest, L"version"), L"158.0.8127.37"), "manifest: version");
  IXMLDOMNode *package = element_of(element_of(manifest, L"packages", 0), L"package", 1);
  check(is(attribute_of(package, L"name"), L"158.0.8127.37_chrome_installer.exe") && is(attribute_of(package, L"hash_sha256"), L"abc") && is(attribute_of(package, L"required"), L"true") &&
        is(attribute_of(package, L"size"), L"160657320"), "packages/package: name, hash_sha256, required, size");
  IXMLDOMNode *actions = element_of(manifest, L"actions", 0);
  IXMLDOMNode *install = child_of(actions, 0), *postinstall = child_of(actions, 1);
  check(count_of(actions) == 2 && is(attribute_of(install, L"event"), L"install") && is(attribute_of(install, L"arguments"), L"--verbose-logging --do-not-launch-chrome --channel=stable") &&
        is(attribute_of(install, L"needsadmin"), L"true") && is(attribute_of(install, L"run"), L"158.0.8127.37_chrome_installer.exe"), "actions: the install action's event, arguments, needsadmin, run");
  check(is(attribute_of(postinstall, L"event"), L"postinstall") && is(attribute_of(postinstall, L"onsuccess"), L"exitsilentlyonlaunchcmd") && is(attribute_of(postinstall, L"run"), NULL),
        "actions: the postinstall action's event and onsuccess, no run");

  IXMLDOMNode *data = element_of(app, L"data", 0), *text = NULL;
  if (data) IXMLDOMNode_get_firstChild(data, &text);
  check(is(attribute_of(data, L"index"), L"verboselogging") && is(attribute_of(data, L"name"), L"install") && is(attribute_of(data, L"status"), L"ok"), "data: index, name, status");
  check(text && type_of(text) == NODE_TEXT && is(value_of(text), L"{\"distribution\":{\"verbose_logging\":true}}") && is(name_of(text), L"#text"), "data: its first child is the text, nodeValue the JSON");
  check(is(text_of(data), L"{\"distribution\":{\"verbose_logging\":true}}"), "data: get_text is the JSON too");
  check(is(text_of(response), L"{\"distribution\":{\"verbose_logging\":true}}"), "get_text of the whole response is the text inside it");

  /* The attribute map by index and in order; the element's own methods. */
  IXMLDOMNamedNodeMap *map = NULL;
  long attributes = 0;
  IXMLDOMNode *first = NULL, *second = NULL;
  if (data) IXMLDOMNode_get_attributes(data, &map);
  if (map) { IXMLDOMNamedNodeMap_get_length(map, &attributes); IXMLDOMNamedNodeMap_get_item(map, 0, &first); IXMLDOMNamedNodeMap_nextNode(map, &second); }
  check(attributes == 3 && first && second && type_of(first) == NODE_ATTRIBUTE && is(name_of(first), L"index") && is(text_of(first), L"verboselogging") && is(name_of(second), L"index"),
        "the attribute map: get_length, get_item(0) and nextNode are the first attribute, an attribute node");
  release(first);
  release(second);
  if (map) {
    int more = 0;
    while (IXMLDOMNamedNodeMap_nextNode(map, &next) == S_OK && next) { ++more; IXMLDOMNode_Release(next); }
    IXMLDOMNamedNodeMap_reset(map);
    BSTR b = SysAllocString(L"status");
    next = NULL;
    check(more == 2 && IXMLDOMNamedNodeMap_getQualifiedItem(map, b, NULL, &next) == S_OK && next && is(value_of(next), L"ok"), "the attribute map: nextNode to the end; getQualifiedItem");
    release(next);
    SysFreeString(b);
    IXMLDOMNamedNodeMap_Release(map);
  }
  IXMLDOMElement *element = NULL;
  if (data) IXMLDOMNode_QueryInterface(data, &kIidElement, (void **)&element);
  if (element) {
    BSTR b = SysAllocString(L"name"), missing = SysAllocString(L"missing"), tag = NULL;
    IXMLDOMAttribute *a = NULL, *none = (IXMLDOMAttribute *)1;
    VariantInit(&v);
    check(IXMLDOMElement_getAttribute(element, b, &v) == S_OK && V_VT(&v) == VT_BSTR && is(V_BSTR(&v), L"install"), "IXMLDOMElement::getAttribute");
    VariantInit(&v);
    check(IXMLDOMElement_getAttribute(element, missing, &v) == S_FALSE && V_VT(&v) == VT_NULL, "getAttribute of a missing attribute: VT_NULL, S_FALSE");
    check(IXMLDOMElement_getAttributeNode(element, b, &a) == S_OK && a && IXMLDOMElement_getAttributeNode(element, missing, &none) == S_FALSE && none == NULL, "getAttributeNode");
    if (a) {
      BSTR attribute_name = NULL;
      VariantInit(&v);
      IXMLDOMAttribute_get_name(a, &attribute_name);
      check(is(attribute_name, L"name") && IXMLDOMAttribute_get_value(a, &v) == S_OK && V_VT(&v) == VT_BSTR && is(V_BSTR(&v), L"install"), "IXMLDOMAttribute: get_name, get_value");
      IXMLDOMAttribute_Release(a);
    }
    IXMLDOMElement_get_tagName(element, &tag);
    check(is(tag, L"data"), "get_tagName");
    SysFreeString(b);
    SysFreeString(missing);
    IXMLDOMElement_Release(element);
  } else check(0, "an element node has IXMLDOMElement");
  IXMLDOMText *as_text = NULL;
  if (text) IXMLDOMNode_QueryInterface(text, &kIidText, (void **)&as_text);
  if (as_text) {
    BSTR characters = NULL;
    long units = 0;
    IXMLDOMText_get_data(as_text, &characters);
    IXMLDOMText_get_length(as_text, &units);
    check(units == 41 && is(characters, L"{\"distribution\":{\"verbose_logging\":true}}"), "IXMLDOMText: get_data, get_length");
    IXMLDOMText_Release(as_text);
  } else check(0, "a text node has IXMLDOMText");

  /* Around the tree. */
  IXMLDOMNode *parent = NULL, *sibling = NULL, *previous = NULL, *last = NULL, *none = (IXMLDOMNode *)1, *top = NULL;
  IXMLDOMDocument *owner = NULL, *no_owner = (IXMLDOMDocument *)1;
  if (data) { IXMLDOMNode_get_parentNode(data, &parent); IXMLDOMNode_get_previousSibling(data, &previous); }
  if (updatecheck) IXMLDOMNode_get_nextSibling(updatecheck, &sibling);
  if (app) IXMLDOMNode_get_lastChild(app, &last);
  check(is(name_of(parent), L"app") && is(name_of(sibling), L"data") && is(name_of(previous), L"updatecheck") && is(name_of(last), L"data"), "parentNode, nextSibling, previousSibling, lastChild");
  check(data && IXMLDOMNode_get_nextSibling(data, &none) == S_FALSE && none == NULL, "the last child has no nextSibling (S_FALSE, NULL)");
  if (response) IXMLDOMNode_get_parentNode(response, &top);
  check(top && type_of(top) == NODE_DOCUMENT && IXMLDOMNode_get_parentNode(top, &none) == S_FALSE && none == NULL, "the root element's parent is the document, which has none");
  check(data && IXMLDOMNode_get_ownerDocument(data, &owner) == S_OK && owner && IXMLDOMDocument_get_ownerDocument(d, &no_owner) == S_FALSE && no_owner == NULL,
        "ownerDocument of a node; the document has none");
  if (owner) {
    IXMLDOMElement *again = NULL;
    BSTR tag = NULL;
    if (IXMLDOMDocument_get_documentElement(owner, &again) == S_OK && again) { IXMLDOMElement_get_tagName(again, &tag); IXMLDOMElement_Release(again); }
    check(is(tag, L"response"), "and it is the document the node was loaded into");
    IXMLDOMDocument_Release(owner);
  }
  release(parent); release(sibling); release(previous); release(last); release(top);

  /* By name, and by path. */
  IXMLDOMNodeList *found = NULL;
  BSTR b = SysAllocString(L"action");
  long count = 0;
  check(IXMLDOMDocument_getElementsByTagName(d, b, &found) == S_OK && found && IXMLDOMNodeList_get_length(found, &count) == S_OK && count == 2, "getElementsByTagName(action): two");
  if (found) IXMLDOMNodeList_Release(found);
  SysFreeString(b);
  IXMLDOMNode *by_path = select_one((IXMLDOMNode *)d, L"response/app/updatecheck/manifest"), *anywhere = select_one(response, L"//manifest"), *attribute = select_one(app, L"@status");
  check(is(attribute_of(by_path, L"version"), L"158.0.8127.37") && is(name_of(anywhere), L"manifest") && is(attribute_of(anywhere, L"version"), L"158.0.8127.37"),
        "selectSingleNode: response/app/updatecheck/manifest, //manifest");
  check(attribute && type_of(attribute) == NODE_ATTRIBUTE && is(value_of(attribute), L"ok"), "selectSingleNode(@status): the attribute");
  check(select_count(response, L"app/updatecheck/manifest/actions/action") == 2 && select_count(response, L"//action") == 2 && select_count(response, L"nothing") == 0 &&
        select_count(app, L".") == 1 && select_count(response, L"//action[@event='postinstall']") == 1, "selectNodes: a path, //action, no match, ., a test on an attribute");
  release(by_path); release(anywhere); release(attribute);

  release(text); release(data); release(postinstall); release(install); release(actions); release(package); release(manifest); release(url); release(updatecheck);
  release(app); release(requirements); release(response);
}

static void test_content(IXMLDOMDocument *d) {
  IXMLDOMNode *root, *n, *m;
  /* Entities and character references. */
  check(load(d, L"<a b=\"x&amp;y&lt;&#65;&#x42;&quot;&apos;&gt;\" c='\"'>1 &lt; 2 &amp;&amp; &#x20AC; &apos;q&apos; &quot;&gt;&#x1F600;</a>") == 1, "a document with entities and character references");
  root = root_of(d);
  check(is(attribute_of(root, L"b"), L"x&y<AB\"'>") && is(attribute_of(root, L"c"), L"\""), "in attribute values: &amp; &lt; &#65; &#x42; &quot; &apos; &gt;, and the other quote");
  check(is(text_of(root), L"1 < 2 && \x20AC 'q' \">\xD83D\xDE00") && count_of(root) == 1, "in text: one text node, with a character beyond U+FFFF as two units");
  release(root);

  /* CDATA. */
  check(load(d, L"<a>x<![CDATA[<raw> & ]]]>y</a>") == 1, "a document with a CDATA section");
  root = root_of(d);
  n = child_of(root, 1);
  check(count_of(root) == 3 && type_of(n) == NODE_CDATA_SECTION && is(value_of(n), L"<raw> & ]") && is(name_of(n), L"#cdata-section"), "the CDATA section is a node of its own, its characters as written");
  check(is(text_of(root), L"x<raw> & ]y"), "get_text joins text and CDATA");
  release(n);
  release(root);

  /* Comments and processing instructions; the document's own children. */
  check(load(d, L"<?xml version=\"1.0\"?><!-- top --><a><!--in--><?pi some data?></a><!--after-->") == 1, "a document with a declaration, comments and a processing instruction");
  n = child_of((IXMLDOMNode *)d, 0);
  check(count_of((IXMLDOMNode *)d) == 4 && type_of(n) == NODE_PROCESSING_INSTRUCTION && is(name_of(n), L"xml"), "the document has four children; the declaration is a processing instruction called xml");
  release(n);
  n = child_of((IXMLDOMNode *)d, 1);
  check(type_of(n) == NODE_COMMENT && is(value_of(n), L" top ") && is(name_of(n), L"#comment"), "a comment before the root element");
  release(n);
  root = root_of(d);
  n = child_of(root, 0);
  m = child_of(root, 1);
  check(is(name_of(root), L"a") && type_of(n) == NODE_COMMENT && is(value_of(n), L"in") && type_of(m) == NODE_PROCESSING_INSTRUCTION && is(name_of(m), L"pi") && is(value_of(m), L"some data"),
        "inside the root: a comment, a processing instruction with its target and data");
  check(is(text_of(root), L""), "comments and processing instructions are not text");
  release(n); release(m); release(root);
  check(load(d, L"<!DOCTYPE a [ <!ELEMENT a (#PCDATA)> <!-- > --> ]><a>t</a>") == 1, "a DOCTYPE with an internal subset is accepted");
  root = root_of(d);
  check(is(name_of(root), L"a") && is(text_of(root), L"t"), "and the root element follows it");
  release(root);

  /* Namespaces. */
  check(load(d, L"<r xmlns=\"urn:d\" xmlns:p=\"urn:p\"><p:c p:a=\"1\" b=\"2\"/><d xmlns=\"\"/><e/></r>") == 1, "a document with namespaces");
  root = root_of(d);
  check(is(name_of(root), L"r") && is(base_of(root), L"r") && is(uri_of(root), L"urn:d") && is(prefix_of(root), NULL), "the default namespace: namespaceURI, no prefix");
  n = child_of(root, 0);
  check(is(name_of(n), L"p:c") && is(base_of(n), L"c") && is(prefix_of(n), L"p") && is(uri_of(n), L"urn:p"), "a prefixed element: nodeName p:c, baseName c, prefix p, its namespaceURI");
  m = attribute_node(n, L"p:a");
  check(m && is(name_of(m), L"p:a") && is(base_of(m), L"a") && is(prefix_of(m), L"p") && is(uri_of(m), L"urn:p") && is(value_of(m), L"1"), "a prefixed attribute likewise");
  release(m);
  m = attribute_node(n, L"b");
  BSTR uri = uri_of(m);
  check(m && is(base_of(m), L"b") && (!uri || SysStringLen(uri) == 0), "an attribute without a prefix is in no namespace");
  SysFreeString(uri);
  release(m);
  {
    IXMLDOMNamedNodeMap *map = NULL;
    BSTR name = SysAllocString(L"a"), space = SysAllocString(L"urn:p"), other = SysAllocString(L"urn:other");
    IXMLDOMNode *found = NULL, *none = (IXMLDOMNode *)1;
    if (n) IXMLDOMNode_get_attributes(n, &map);
    check(map && IXMLDOMNamedNodeMap_getQualifiedItem(map, name, space, &found) == S_OK && found && is(value_of(found), L"1") &&
          IXMLDOMNamedNodeMap_getQualifiedItem(map, name, other, &none) == S_FALSE && none == NULL, "getQualifiedItem by base name and namespace");
    release(found);
    if (map) IXMLDOMNamedNodeMap_Release(map);
    SysFreeString(name); SysFreeString(space); SysFreeString(other);
  }
  release(n);
  n = child_of(root, 1);
  uri = uri_of(n);
  check(is(name_of(n), L"d") && (!uri || SysStringLen(uri) == 0), "xmlns=\"\" takes an element out of the default namespace");
  SysFreeString(uri);
  release(n);
  n = child_of(root, 2);
  check(is(uri_of(n), L"urn:d"), "and its sibling is in it");
  release(n);
  release(root);

  /* White space. */
  static const wchar_t spaced[] = L"<a>\n  <b> x </b>\r\n  <c>  </c>\n</a>";
  IXMLDOMDocument_put_preserveWhiteSpace(d, VARIANT_FALSE);
  check(load(d, spaced) == 1, "a document with white space between its elements");
  root = root_of(d);
  n = child_of(root, 0);
  m = child_of(root, 1);
  check(count_of(root) == 2 && count_of(m) == 0 && count_of(n) == 1, "without preserveWhiteSpace the white space between elements is no node, nor is white space alone in an element");
  release(m);
  m = child_of(n, 0);
  check(is(value_of(m), L"x") && is(text_of(n), L"x"), "and text is read without the white space around it");
  release(m); release(n); release(root);
  IXMLDOMDocument_put_preserveWhiteSpace(d, VARIANT_TRUE);
  check(load(d, spaced) == 1, "the same document with preserveWhiteSpace");
  root = root_of(d);
  n = child_of(root, 0);
  m = child_of(root, 2);
  check(count_of(root) == 5 && type_of(n) == NODE_TEXT && is(value_of(n), L"\n  ") && is(value_of(m), L"\n  "), "the white space is text nodes, a line end a line feed");
  check(is(text_of(root), L"\n   x \n    \n"), "and get_text has all of it");
  release(m); release(n);
  n = child_of(root, 1);
  m = child_of(n, 0);
  check(is(value_of(m), L" x "), "text keeps the white space around it");
  release(m); release(n); release(root);
  IXMLDOMDocument_put_preserveWhiteSpace(d, VARIANT_FALSE);

  /* Serialization. */
  static const wchar_t written[] = L"<a b=\"1\" c=\"&lt;&amp;&quot;\"><c/>t&amp;&lt;<d>x</d><!--n--><![CDATA[<y>]]><?p q?></a>";
  check(load(d, written) == 1, "a document to write back");
  root = root_of(d);
  check(is(xml_of(root), written), "get_xml of the root element is the document as it was written");
  n = child_of(root, 2);
  check(is(xml_of(n), L"<d>x</d>"), "get_xml of an element inside it");
  release(n);
  BSTR whole = xml_of((IXMLDOMNode *)d);
  check(whole && wcsstr(whole, written) != NULL, "get_xml of the document contains it");
  SysFreeString(whole);
  release(root);
}

/* Methods that change a document: not implemented here (E_NOTIMPL), implemented in MSXML. Either way the call must come back with the stack in order,
 * which a table entry with the wrong number of arguments would not on i386. */
static void test_unimplemented(IXMLDOMDocument *d) {
  load(d, L"<a b='1'><c/></a>");
  IXMLDOMNode *root = root_of(d), *out = NULL, *c = child_of(root, 0);
  IXMLDOMElement *element = NULL;
  IXMLDOMText *text = NULL;
  IXMLDOMComment *comment = NULL;
  BSTR name = SysAllocString(L"n");
  VARIANT v;
  volatile unsigned guard_before = 0x12345678, guard_after;
  int calls = 0;
  HRESULT hr;
  VariantInit(&v);
#define TRY(call) hr = (call); if (hr == E_NOTIMPL || SUCCEEDED(hr) || FAILED(hr)) ++calls
  TRY(IXMLDOMDocument_createElement(d, name, &element));
  TRY(IXMLDOMDocument_createTextNode(d, name, &text));
  TRY(IXMLDOMDocument_createComment(d, name, &comment));
  V_VT(&v) = VT_I4; V_I4(&v) = NODE_ELEMENT;
  TRY(IXMLDOMDocument_createNode(d, v, name, NULL, &out));
  if (out) { IXMLDOMNode_Release(out); out = NULL; }
  V_VT(&v) = VT_NULL;
  TRY(IXMLDOMNode_insertBefore(root, (IXMLDOMNode *)element, v, &out));
  if (out) { IXMLDOMNode_Release(out); out = NULL; }
  TRY(IXMLDOMNode_appendChild(root, (IXMLDOMNode *)text, &out));
  if (out) { IXMLDOMNode_Release(out); out = NULL; }
  TRY(IXMLDOMNode_removeChild(root, c, &out));
  if (out) { IXMLDOMNode_Release(out); out = NULL; }
  TRY(IXMLDOMNode_cloneNode(root, VARIANT_TRUE, &out));
  if (out) { IXMLDOMNode_Release(out); out = NULL; }
  TRY(IXMLDOMNode_put_text(root, name));
  V_VT(&v) = VT_BSTR; V_BSTR(&v) = name;
  TRY(IXMLDOMNode_put_nodeValue(root, v));
  IXMLDOMElement *as_element = NULL;
  IXMLDOMNode_QueryInterface(root, &kIidElement, (void **)&as_element);
  if (as_element) {
    TRY(IXMLDOMElement_setAttribute(as_element, name, v));
    TRY(IXMLDOMElement_removeAttribute(as_element, name));
    TRY(IXMLDOMElement_normalize(as_element));
    IXMLDOMElement_Release(as_element);
  }
  UINT count = 0;
  TRY(IXMLDOMDocument_GetTypeInfoCount(d, &count));
  guard_after = guard_before;
  if (element) IXMLDOMElement_Release(element);
  if (text) IXMLDOMText_Release(text);
  if (comment) IXMLDOMComment_Release(comment);
  SysFreeString(name);
  release(c);
  release(root);
  check(calls == 14 && guard_after == 0x12345678 && load(d, L"<still/>") == 1, "the methods that change a document return, with the stack in order, and the document still loads");
#undef TRY
}

static void test_malformed(IXMLDOMDocument *d) {
  static const wchar_t *const bad[] = {
    L"", L"   ", L"<", L"<a", L"<a>", L"<a></b>", L"<a><b></a></b>", L"</a>", L"<a/><b/>", L"<a b=1/>", L"<a b='1' b='2'/>", L"<a b='1'c='2'/>", L"<a b='<'/>", L"<a b='1/>",
    L"<a b/>", L"<a ='1'/>", L"<a>&bogus;</a>", L"<a>&amp</a>", L"<a>&#xZZ;</a>", L"<a>&#0;</a>", L"<a>&#x110000;</a>", L"<a>& </a>", L"<a b='&bogus;'/>", L"<a><!-- x -- y --></a>",
    L"<a><!-- unterminated</a>", L"<a><![CDATA[x</a>", L"<a><?pi</a>", L"text<a/>", L"<a/>text", L"<1a/>", L"<a><b/></a></a>", L"<a>]]></a>", L"<?xml version='1.0'?",
    L"<a/><?xml version='1.0'?>", L"<a>\x01</a>", L"<p:a/>", L"<a p:b='1'/>", L"<a><p:b xmlns:q='urn:q'/></a>", L"<a><![CDATA[x]]></a><![CDATA[y]]>", L"<a></a", L"<a><</a>",
    L"<!DOCTYPE a [ <!ELEMENT a ANY> <a/>", L"<a>\xD800</a>", L"<a\x00/>",
  };
  char what[200];
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    char shown[64];
    const size_t length = i + 1 == sizeof(bad) / sizeof(bad[0]) ? 5 : wcslen(bad[i]);      /* (the last has a NUL inside) */
    size_t k = 0;
    for (; k < length && k < sizeof(shown) - 1; ++k) shown[k] = bad[i][k] >= 0x20 && bad[i][k] < 0x7F ? (char)bad[i][k] : '?';
    shown[k] = 0;
    snprintf(what, sizeof(what), "not well formed, refused with a parse error, and the next document loads: \"%s\"", shown);
    check(refused(d, bad[i], length), what);
  }

  /* Where and why. */
  check(load(d, L"<a>\n  <b></c>\n</a>") == 0, "a mismatched end tag on the second line");
  IXMLDOMParseError *error = NULL;
  long code = 0, line = 0, column = 0;
  BSTR reason = NULL;
  IXMLDOMDocument_get_parseError(d, &error);
  if (error) {
    IXMLDOMParseError_get_errorCode(error, &code);
    IXMLDOMParseError_get_line(error, &line);
    IXMLDOMParseError_get_linepos(error, &column);
    IXMLDOMParseError_get_reason(error, &reason);
    IXMLDOMParseError_Release(error);
  }
  snprintf(what, sizeof(what), "parseError: an error code, a reason, line 2 and a column (0x%08lX, line %ld, column %ld)", (unsigned long)code, line, column);
  check(code != 0 && line == 2 && column > 0 && reason && SysStringLen(reason) > 0, what);
  SysFreeString(reason);
  check(load(d, L"<fine/>") == 1 && error_code(d) == 0, "after a good load the parse error is clear again");

  /* Cut off anywhere, a document is not a document. */
  static const wchar_t small[] = L"<?xml version=\"1.0\"?><r a=\"1\" b='&lt;'><!--c--><b>t&amp;&#65;<![CDATA[d]]></b><?p q?><e/></r>";
  const size_t small_length = wcslen(small);
  int all_refused = 1;
  for (size_t cut = 0; cut < small_length; ++cut) {
    if (load_n(d, small, cut) != 0 || error_code(d) == 0) { all_refused = 0; printf("xml32: (taken although cut off after %u characters)\n", (unsigned)cut); }
  }
  snprintf(what, sizeof(what), "a document of %u characters cut off at every length is refused each time", (unsigned)small_length);
  check(all_refused, what);
  check(load_n(d, small, small_length) == 1, "and whole it loads");

  /* One character of it replaced by another, at every place: taken or refused, and what was taken can be read. */
  static const wchar_t replacements[] = {'<', '>', '&', '"', '\'', '/', ']', '?', '!', ':', ' ', 'x', 0, 0xD800, 0xDC00, 0xFFFF};
  wchar_t changed[128];
  int all_answered = small_length < 128, taken = 0;
  for (size_t at = 0; all_answered && at < small_length; ++at) {
    for (size_t r = 0; r < sizeof(replacements) / sizeof(replacements[0]); ++r) {
      memcpy(changed, small, small_length * sizeof(wchar_t));
      changed[at] = replacements[r];
      const int result = load_n(d, changed, small_length);
      if (result == 1) {
        IXMLDOMNode *root = root_of(d);
        BSTR text = text_of(root), xml = xml_of((IXMLDOMNode *)d);
        if (!root || !text || !xml || select_count(root, L"//*") < 0) all_answered = 0;
        SysFreeString(text);
        SysFreeString(xml);
        release(root);
        ++taken;
      } else if (result != 0 || error_code(d) == 0) all_answered = 0;
    }
  }
  snprintf(what, sizeof(what), "the same with each character replaced by each of %u others: taken (%d) or refused with an error, never anything else",
           (unsigned)(sizeof(replacements) / sizeof(replacements[0])), taken);
  check(all_answered, what);

  /* Deep and large. */
  const size_t depth = 100000;
  wchar_t *deep = (wchar_t *)malloc((depth * 7 + 1) * sizeof(wchar_t));
  if (deep) {
    for (size_t i = 0; i < depth; ++i) memcpy(deep + i * 3, L"<a>", 3 * sizeof(wchar_t));
    check(refused(d, deep, depth * 3), "100000 elements opened inside each other and never closed: refused");
    for (size_t i = 0; i < depth; ++i) memcpy(deep + depth * 3 + i * 4, L"</a>", 4 * sizeof(wchar_t));
    const int result = load_n(d, deep, depth * 7);
    check(result == 0 || result == 1, "the same closed properly: refused or taken, but it comes back");
    if (result == 1) {      /* (a library that takes it must also be able to read it and to let go of it) */
      IXMLDOMNode *root = root_of(d);
      BSTR text = text_of(root);
      BSTR xml = xml_of(root);
      SysFreeString(text);
      SysFreeString(xml);
      release(root);
    }
    check(load(d, L"<after-deep/>") == 1, "and the document loads something else afterwards");
    free(deep);
  } else check(0, "memory for the deep document");
  const size_t huge = 1000000;
  wchar_t *large = (wchar_t *)malloc((huge + 32) * sizeof(wchar_t));
  if (large) {
    memcpy(large, L"<a b='", 6 * sizeof(wchar_t));
    for (size_t i = 0; i < huge; ++i) large[6 + i] = (wchar_t)('a' + i % 26);
    check(refused(d, large, 6 + huge), "an attribute value of a million characters that never ends: refused");
    memcpy(large + 6 + huge, L"'/>", 3 * sizeof(wchar_t));
    const int taken = load_n(d, large, 6 + huge + 3) == 1;
    IXMLDOMNode *root = root_of(d);
    BSTR value = attribute_of(root, L"b");
    check(taken && value && SysStringLen(value) == huge && value[0] == 'a' && value[huge - 1] == (wchar_t)('a' + (huge - 1) % 26), "and with its end it loads, the value whole");
    SysFreeString(value);
    release(root);
    free(large);
  } else check(0, "memory for the large document");
}

static int write_file(const wchar_t *path, const void *bytes, DWORD size) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD written = 0;
  if (file == INVALID_HANDLE_VALUE) return 0;
  const BOOL good = WriteFile(file, bytes, size, &written, NULL) && written == size;
  CloseHandle(file);
  return good;
}
/* load(path): 1 taken, 0 refused the proper way, -1 anything else. */
static int load_file(IXMLDOMDocument *d, const wchar_t *path) {
  VARIANT source;
  VARIANT_BOOL ok = 77;
  V_VT(&source) = VT_BSTR;
  V_BSTR(&source) = SysAllocString(path);
  const HRESULT hr = IXMLDOMDocument_load(d, source, &ok);
  VariantClear(&source);
  if (hr == S_OK && ok == VARIANT_TRUE) return 1;
  if (hr == S_FALSE && ok == VARIANT_FALSE) return 0;
  return -1;
}
static void test_files(IXMLDOMDocument *d) {
  wchar_t folder[MAX_PATH], path[MAX_PATH + 32];
  DWORD length = GetTempPathW(MAX_PATH, folder);
  if (length == 0 || length >= MAX_PATH) wcscpy(folder, L".\\");
  static const char utf8[] = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<r name=\"caf\xC3\xA9\">\xE2\x82\xAC 5</r>\r\n";
  static const char plain[] = "<r>plain \xC3\xBC</r>";
  static const wchar_t utf16[] = L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?><r>sixteen \x20AC</r>";
  static const char broken[] = "<r>\xC3</r>";
  IXMLDOMNode *root;

  wcscpy(path, folder); wcscat(path, L"xml32-utf8.xml");
  check(write_file(path, utf8, sizeof(utf8) - 1) && load_file(d, path) == 1, "load: a UTF-8 file with a byte order mark and a declaration");
  root = root_of(d);
  check(is(attribute_of(root, L"name"), L"caf\xE9") && is(text_of(root), L"\x20AC 5"), "its characters outside ASCII are right");
  release(root);
  DeleteFileW(path);

  wcscpy(path, folder); wcscat(path, L"xml32-plain.xml");
  check(write_file(path, plain, sizeof(plain) - 1) && load_file(d, path) == 1, "load: a file with neither is UTF-8");
  root = root_of(d);
  check(is(text_of(root), L"plain \xFC"), "and reads as that");
  release(root);
  DeleteFileW(path);

  wcscpy(path, folder); wcscat(path, L"xml32-utf16.xml");
  check(write_file(path, utf16, sizeof(utf16) - sizeof(wchar_t)) && load_file(d, path) == 1, "load: a UTF-16 file with a byte order mark");
  root = root_of(d);
  check(is(text_of(root), L"sixteen \x20AC"), "and reads as that");
  release(root);
  DeleteFileW(path);

  wcscpy(path, folder); wcscat(path, L"xml32-broken.xml");
  check(write_file(path, broken, sizeof(broken) - 1) && load_file(d, path) == 0 && error_code(d) != 0, "load: a file that is not valid UTF-8 is refused with a parse error");
  DeleteFileW(path);
  check(load_file(d, path) == 0 && error_code(d) != 0, "load: a file that is not there is refused with an error");
  check(load(d, L"<ok/>") == 1 && error_code(d) == 0, "and loadXML works after that");
}

/* A node outlives the document's next load, and the document itself. */
static void test_lifetime(void) {
  IXMLDOMDocument *d = new_document();
  if (!d) { check(0, "a document"); return; }
  load(d, L"<old a='kept'><child>text</child></old>");
  IXMLDOMNode *old = root_of(d), *child = child_of(old, 0);
  check(load(d, L"<new/>") == 1, "a second document loaded into the same object");
  IXMLDOMNode *now = root_of(d);
  check(is(name_of(now), L"new") && is(name_of(old), L"old") && is(attribute_of(old, L"a"), L"kept") && is(text_of(child), L"text"), "the nodes of the first are still what they were");
  release(now);
  check(load(d, L"<broken") == 0 && is(name_of(old), L"old"), "also after a load that failed");
  IXMLDOMDocument_Release(d);
  check(is(name_of(child), L"child") && is(text_of(old), L"text"), "and after the document was released");
  release(child);
  release(old);
}

int main(void) {
  printf("xml32: a 32-bit program that reads XML with MSXML is running\n");
  const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  check(SUCCEEDED(hr), "CoInitializeEx");
  test_classes();
  IXMLDOMDocument *d = new_document();
  check(d != NULL, "a document to work with");
  if (d) {
    test_settings(d);
    test_response(d);
    test_content(d);
    test_unimplemented(d);
    test_malformed(d);
    test_files(d);
    check(IXMLDOMDocument_Release(d) == 0, "the last Release of the document says so");
  }
  test_lifetime();
  CoUninitialize();
  if (g_failed == 0) printf("xml32: all %d checks passed\n", g_passed);
  else printf("xml32: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
