/* ole32: the table of the classes this library implements (CoGetClassObject finds them before it looks into the registry). Each module lists its own in BUILTIN_CLASSES_<MODULE>. */

typedef struct BuiltinClass { const GUID *clsid; CreateObjectFn create; } BuiltinClass;

#define BUILTIN_ROW(clsid, fn) {&(clsid), fn},
static const BuiltinClass g_builtin_classes[] = {
#ifdef BUILTIN_CLASSES_MARSHAL
  BUILTIN_CLASSES_MARSHAL(BUILTIN_ROW)
#endif
#ifdef BUILTIN_CLASSES_MONIKER
  BUILTIN_CLASSES_MONIKER(BUILTIN_ROW)
#endif
#ifdef BUILTIN_CLASSES_OLE
  BUILTIN_CLASSES_OLE(BUILTIN_ROW)
#endif
#ifdef BUILTIN_CLASSES_OLECREATE
  BUILTIN_CLASSES_OLECREATE(BUILTIN_ROW)
#endif
  {NULL, NULL}
};

static CreateObjectFn find_builtin_class(REFCLSID clsid) {
  for (const BuiltinClass *c = g_builtin_classes; c->clsid; c++) if (IsEqualGUID(c->clsid, clsid)) return c->create;
  return NULL;
}
