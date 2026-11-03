/* ole32: property sets over storages and streams. NOT IMPLEMENTED YET: IStorage answers QueryInterface(IPropertySetStorage) with E_NOINTERFACE, and the Stg*Prop* exports are stubs. */
static HRESULT propsetstg_create(IStorage *storage, IPropertySetStorage **out) { (void)storage; if (out) *out = NULL; return E_NOINTERFACE; }
