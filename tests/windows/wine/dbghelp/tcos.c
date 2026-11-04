/*
 * TheoryCraftOS's own checks of dbghelp.dll, in the style of Wine's tests (and run with them: scripts/winetest.sh dbghelp tcos). Wine's tests of this DLL lean on PDB and
 * DWARF symbols; the symbol handler of this OS reads the export tables and the COFF symbols of images, so these check that, the x64 stack walker, the image helpers, the
 * name decoder, the path functions and MiniDumpWriteDump. Written for this project (GNU AGPL).
 */
#include "windows.h"
#include "dbghelp.h"
#include "wine/test.h"

static void test_image_helpers(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *section;
    ULONG size = 0;
    void *dir;

    ok(ImageNtHeader(NULL) == NULL, "NULL image\n");
    nt = ImageNtHeader(k32);
    ok(nt != NULL && nt->Signature == IMAGE_NT_SIGNATURE, "ImageNtHeader %p\n", nt);
    ok(nt && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64, "machine %#x\n", nt ? nt->FileHeader.Machine : 0);
    ok(GetTimestampForLoadedLibrary(k32) == nt->FileHeader.TimeDateStamp, "timestamp\n");

    dir = ImageDirectoryEntryToData(k32, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &size);
    ok(dir != NULL && size >= sizeof(IMAGE_EXPORT_DIRECTORY), "export directory %p %lu\n", dir, size);
    ok((BYTE *)dir == (BYTE *)k32 + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress, "export directory address\n");
    section = NULL;
    dir = ImageDirectoryEntryToDataEx(k32, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &size, &section);
    ok(dir != NULL, "Ex\n");
    dir = ImageDirectoryEntryToData(k32, TRUE, IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR, &size);
    ok(dir == NULL && size == 0, "no COM descriptor: %p %lu\n", dir, size);
    dir = ImageDirectoryEntryToData(k32, TRUE, 40, &size);
    ok(dir == NULL, "directory beyond the table\n");

    section = ImageRvaToSection(nt, k32, nt->OptionalHeader.AddressOfEntryPoint);
    ok(section != NULL && !memcmp(section->Name, ".text", 5), "entry point section %p\n", section);
    ok(ImageRvaToSection(nt, k32, 0xFFFFFFF0) == NULL, "rva far outside\n");
    if (section)
    {
        void *va = ImageRvaToVa(nt, k32, section->VirtualAddress, &section);
        ok(va == (BYTE *)k32 + section->PointerToRawData, "ImageRvaToVa %p\n", va);
    }
    ok(ImagehlpApiVersion() != NULL && ImagehlpApiVersion()->MajorVersion >= 4, "api version\n");
}

static BOOL CALLBACK count_symbol(PSYMBOL_INFO info, ULONG size, PVOID context)
{
    (*(int *)context)++;
    return TRUE;
}

static BOOL CALLBACK find_module(PCSTR name, DWORD64 base, PVOID context)
{
    if (!lstrcmpiA(name, "kernel32")) *(DWORD64 *)context = base;
    return TRUE;
}

static void test_symbols(void)
{
    HANDLE process = GetCurrentProcess();
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    DWORD64 address = (DWORD64)(ULONG_PTR)GetProcAddress(k32, "CreateFileA"), displacement, found = 0, base;
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *info = (SYMBOL_INFO *)buffer;
    char path[MAX_PATH];
    IMAGEHLP_MODULE64 module;
    BOOL ret;
    int count;

    ok(SymGetOptions() & SYMOPT_UNDNAME, "default options %#lx\n", SymGetOptions());
    ok(!SymGetModuleBase64(process, address), "no module before SymInitialize\n");
    ret = SymInitialize(process, NULL, TRUE);
    ok(ret, "SymInitialize %lu\n", GetLastError());
    ret = SymInitialize(process, NULL, FALSE);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "second SymInitialize %d %lu\n", ret, GetLastError());

    base = SymGetModuleBase64(process, address);
    ok(base == (DWORD64)(ULONG_PTR)k32, "module base %I64x vs %p\n", base, k32);
    SymEnumerateModules64(process, find_module, &found);
    ok(found == (DWORD64)(ULONG_PTR)k32, "kernel32 in the module list: %I64x\n", found);

    memset(&module, 0, sizeof(module));
    module.SizeOfStruct = sizeof(module);
    ret = SymGetModuleInfo64(process, address, &module);
    ok(ret, "SymGetModuleInfo64 %lu\n", GetLastError());
    ok(!lstrcmpiA(module.ModuleName, "kernel32"), "module name '%s'\n", module.ModuleName);
    ok(module.BaseOfImage == base && module.ImageSize > 0 && module.NumSyms > 0, "module info %I64x %lu %lu\n", module.BaseOfImage, module.ImageSize, module.NumSyms);
    ok(module.SymType == SymExport || module.SymType == SymCoff, "symbol type %u\n", module.SymType);
    module.SizeOfStruct = 4;
    ok(!SymGetModuleInfo64(process, address, &module), "a structure that is too small\n");

    memset(buffer, 0, sizeof(buffer));
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 256;
    displacement = 0xdead;
    ret = SymFromAddr(process, address, &displacement, info);
    ok(ret, "SymFromAddr %lu\n", GetLastError());
    ok(displacement == 0 && info->Address == address && info->ModBase == base, "symbol %I64x +%I64x base %I64x\n", info->Address, displacement, info->ModBase);
    ok(info->NameLen == strlen(info->Name) && info->NameLen > 0, "name length %lu '%s'\n", info->NameLen, info->Name);
    ret = SymFromAddr(process, address + 1, &displacement, info);
    ok(ret && displacement == 1 + (address - info->Address), "inside a function: %d +%I64x\n", ret, displacement);

    memset(buffer, 0, sizeof(buffer));
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 256;
    ret = SymFromName(process, "kernel32!CreateFileA", info);
    ok(ret && info->Address == address, "SymFromName %d %I64x vs %I64x\n", ret, info->Address, address);
    ok(!strcmp(info->Name, "CreateFileA"), "name '%s'\n", info->Name);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    ret = SymFromName(process, "CreateFileA", info);
    ok(ret && info->Address == address, "SymFromName without a module\n");
    ret = SymFromName(process, "kernel32!NoSuchSymbolAnywhere", info);
    ok(!ret, "unknown name\n");
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 4;      /* a name that does not fit is cut */
    ret = SymFromName(process, "kernel32!CreateFileA", info);
    ok(ret && strlen(info->Name) == 3 && info->NameLen == 3, "short buffer: '%s' %lu\n", info->Name, info->NameLen);

    count = 0;
    ret = SymEnumSymbols(process, base, "Create*A", count_symbol, &count);
    ok(ret && count > 1, "SymEnumSymbols Create*A: %d %d\n", ret, count);
    count = 0;
    ret = SymEnumSymbols(process, 0, "kernel32!CreateFile?", count_symbol, &count);
    ok(ret && count >= 1, "SymEnumSymbols with a module: %d\n", count);
    count = 0;
    SymEnumSymbols(process, 0, "nosuchmodule!*", count_symbol, &count);
    ok(count == 0, "no such module\n");

    ret = SymGetSearchPath(process, path, sizeof(path));
    ok(ret && path[0], "search path '%s'\n", path);
    ok(SymSetSearchPath(process, "C:\\tcos"), "SymSetSearchPath\n");
    SymGetSearchPath(process, path, sizeof(path));
    ok(!strcmp(path, "C:\\tcos"), "search path '%s'\n", path);

    /* a module of our own: virtual, with symbols added */
    base = SymLoadModuleEx(process, NULL, "virtual.dll", "virt", 0x10000000, 0x10000, NULL, SLMFLAG_VIRTUAL);
    ok(base == 0x10000000, "SymLoadModuleEx virtual %I64x %lu\n", base, GetLastError());
    ok(SymAddSymbol(process, 0x10000000, "first", 0x10000100, 0x20, 0), "SymAddSymbol\n");
    ok(SymAddSymbol(process, 0x10000000, "second", 0x10000200, 0x20, 0), "SymAddSymbol 2\n");
    memset(buffer, 0, sizeof(buffer));
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 256;
    ret = SymFromAddr(process, 0x10000210, &displacement, info);
    ok(ret && !strcmp(info->Name, "second") && displacement == 0x10, "virtual symbol: %d '%s' +%I64x\n", ret, info->Name, displacement);
    ok(SymLoadModuleEx(process, NULL, NULL, "virt", 0x10000000, 0x10000, NULL, SLMFLAG_VIRTUAL) == 0 && GetLastError() == ERROR_SUCCESS, "loading a module twice\n");
    ok(SymDeleteSymbol(process, 0x10000000, "first", 0, 0), "SymDeleteSymbol\n");
    ret = SymFromName(process, "virt!first", info);
    ok(!ret, "deleted symbol\n");
    ok(SymUnloadModule64(process, 0x10000000), "SymUnloadModule64\n");
    ok(!SymUnloadModule64(process, 0x10000000), "unloading twice\n");

    ok(SymCleanup(process), "SymCleanup\n");
    ok(!SymCleanup(process), "SymCleanup twice\n");
    ok(!SymFromAddr(process, address, &displacement, info), "no symbols after SymCleanup\n");
}

static int __attribute__((noinline)) own_function(int x)
{
    volatile int y = x * 3;
    return y + 1;
}

static void test_coff_symbols(void)
{
    HANDLE process = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *info = (SYMBOL_INFO *)buffer;
    IMAGEHLP_MODULE64 module;
    DWORD64 displacement;
    char name[MAX_PATH];

    GetModuleFileNameA(NULL, name, sizeof(name));
    ok(SymInitialize(process, NULL, FALSE), "SymInitialize\n");
    ok(SymLoadModuleEx(process, NULL, name, "test", (DWORD64)(ULONG_PTR)GetModuleHandleA(NULL), 0, NULL, 0) != 0, "SymLoadModuleEx of the program: %lu\n", GetLastError());
    memset(&module, 0, sizeof(module));
    module.SizeOfStruct = sizeof(module);
    ok(SymGetModuleInfo64(process, (DWORD64)(ULONG_PTR)own_function, &module), "module info\n");
    ok(!lstrcmpiA(module.ModuleName, "test"), "module name '%s'\n", module.ModuleName);
    if (module.SymType != SymCoff)
    {
        skip("the test program has no COFF symbol table (type %u)\n", module.SymType);
    }
    else
    {
        memset(buffer, 0, sizeof(buffer));
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 256;
        ok(SymFromAddr(process, (DWORD64)(ULONG_PTR)own_function, &displacement, info), "SymFromAddr\n");
        ok(strstr(info->Name, "own_function") != NULL && displacement == 0, "COFF symbol '%s' +%I64x\n", info->Name, displacement);
        ok(info->Size > 0, "size %lu\n", info->Size);
        ok(info->Flags & SYMFLAG_FUNCTION, "flags %#lx\n", info->Flags);
        ok(SymFromAddr(process, (DWORD64)(ULONG_PTR)own_function + 4, &displacement, info) && displacement == 4, "inside the function\n");
    }
    ok(SymFunctionTableAccess64(process, (DWORD64)(ULONG_PTR)own_function) != NULL, "function table entry of own_function\n");
    ok(SymCleanup(process), "SymCleanup\n");
}

static DWORD64 return_in_walker;

static int __attribute__((noinline)) walk_leaf(DWORD64 *return_address, DWORD64 *frame_pcs, DWORD64 *frame_stacks, int max)
{
    HANDLE process = GetCurrentProcess();
    CONTEXT context;
    STACKFRAME64 frame;
    int n = 0;

    *return_address = (DWORD64)(ULONG_PTR)__builtin_return_address(0);
    RtlCaptureContext(&context);
    memset(&frame, 0, sizeof(frame));
    while (n < max && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
    {
        frame_pcs[n] = frame.AddrPC.Offset;
        frame_stacks[n] = frame.AddrStack.Offset;
        n++;
    }
    return n;
}

static int __attribute__((noinline)) walk_middle(DWORD64 *return_address, DWORD64 *pcs, DWORD64 *stacks, int max)
{
    volatile char pad[200];
    int n;
    pad[0] = 1;
    n = walk_leaf(return_address, pcs, stacks, max);
    return n + pad[0] - 1;
}

static void test_stack_walk(void)
{
    HANDLE process = GetCurrentProcess();
    DWORD64 pcs[64], stacks[64], return_of_leaf = 0;
    int n, i;

    ok(SymInitialize(process, NULL, TRUE), "SymInitialize\n");
    n = walk_middle(&return_of_leaf, pcs, stacks, 64);
    ok(n >= 3, "at least three frames, got %d\n", n);
    if (n >= 3)
    {
        ok(pcs[0] >= (DWORD64)(ULONG_PTR)walk_leaf && pcs[0] < (DWORD64)(ULONG_PTR)walk_leaf + 0x200, "first frame %I64x is in walk_leaf %p\n", pcs[0], walk_leaf);
        ok(pcs[1] == return_of_leaf, "second frame %I64x is the return address of walk_leaf %I64x\n", pcs[1], return_of_leaf);
        ok(pcs[1] >= (DWORD64)(ULONG_PTR)walk_middle && pcs[1] < (DWORD64)(ULONG_PTR)walk_middle + 0x200, "second frame is in walk_middle\n");
        for (i = 1; i < n; i++) ok(stacks[i] > stacks[i - 1], "the stack grows: frame %d %I64x after %I64x\n", i, stacks[i], stacks[i - 1]);
        ok(stacks[2] - stacks[1] >= 200, "walk_middle's frame holds the 200 byte pad: %I64u\n", stacks[2] - stacks[1]);
    }
    ok(SymCleanup(process), "SymCleanup\n");

    SetLastError(0);
    {
        STACKFRAME64 frame;
        CONTEXT context;
        memset(&frame, 0, sizeof(frame));
        memset(&context, 0, sizeof(context));
        ok(!StackWalk64(IMAGE_FILE_MACHINE_I386, process, GetCurrentThread(), &frame, &context, NULL, NULL, NULL, NULL), "only x64 is walked\n");
        ok(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, NULL, NULL, NULL, NULL), "a context without a pc\n");
    }
}

static void test_undecorate(void)
{
    static const struct { const char *in; DWORD flags; const char *out; } tests[] = {
        {"?foo@@YAXXZ", UNDNAME_COMPLETE, "void __cdecl foo(void)"},
        {"?foo@@YAXXZ", UNDNAME_NAME_ONLY, "foo"},
        {"?add@@YAHHH@Z", UNDNAME_COMPLETE, "int __cdecl add(int, int)"},
        {"?add@@YAHHH@Z", UNDNAME_NO_MS_KEYWORDS, "int add(int, int)"},
        {"?name@@YAPEBDPEBD@Z", UNDNAME_COMPLETE, "char const * __ptr64 __cdecl name(char const * __ptr64)"},
        {"?bar@Foo@@QEAAHH@Z", UNDNAME_COMPLETE, "public: int __cdecl Foo::bar(int) __ptr64"},
        {"?bar@Foo@@QEAAHH@Z", UNDNAME_NAME_ONLY, "Foo::bar"},
        {"?get@Foo@@QEBAHXZ", UNDNAME_COMPLETE, "public: int __cdecl Foo::get(void) const __ptr64"},
        {"?make@Foo@@SAXXZ", UNDNAME_COMPLETE, "public: static void __cdecl Foo::make(void)"},
        {"??0Foo@@QEAA@XZ", UNDNAME_COMPLETE, "public: __cdecl Foo::Foo(void) __ptr64"},
        {"??1Foo@@QEAA@XZ", UNDNAME_COMPLETE, "public: __cdecl Foo::~Foo(void) __ptr64"},
        {"?inner@Outer@Ns@@QEAAXXZ", UNDNAME_NAME_ONLY, "Ns::Outer::inner"},
        {"?g@@3HA", UNDNAME_COMPLETE, "int g"},
        {"?use@@YAXPEAVFoo@@@Z", UNDNAME_COMPLETE, "void __cdecl use(class Foo * __ptr64)"},
        {"?f@@YAHHZZ", UNDNAME_COMPLETE, "int __cdecl f(int, ...)"},
    };
    char out[256];
    DWORD len;
    int i;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        len = UnDecorateSymbolName(tests[i].in, out, sizeof(out), tests[i].flags);
        ok(len == strlen(tests[i].out) && !strcmp(out, tests[i].out), "%s: got %lu '%s' expected '%s'\n", tests[i].in, len, out, tests[i].out);
    }
    len = UnDecorateSymbolName("plain", out, sizeof(out), UNDNAME_COMPLETE);
    ok(len == 5 && !strcmp(out, "plain"), "a C name: %lu '%s'\n", len, out);
    len = UnDecorateSymbolName("?foo@@YAXXZ", out, 5, UNDNAME_COMPLETE);
    ok(len == 4 && !strcmp(out, "void"), "a buffer that is too small: %lu '%s'\n", len, out);
    ok(UnDecorateSymbolName(NULL, out, sizeof(out), 0) == 0, "NULL\n");
}

static void test_paths(void)
{
    char temp[MAX_PATH], dir[MAX_PATH], file[MAX_PATH], found[MAX_PATH];
    HANDLE h;
    DWORD attributes;

    GetTempPathA(sizeof(temp), temp);
    wsprintfA(dir, "%stcosdbg\\one\\two\\", temp);
    ok(MakeSureDirectoryPathExists(dir), "MakeSureDirectoryPathExists %lu\n", GetLastError());
    attributes = GetFileAttributesA(dir);
    ok(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY), "directory made: %#lx\n", attributes);
    ok(MakeSureDirectoryPathExists(dir), "directory already there\n");
    wsprintfA(file, "%stcosdbg\\three\\leaf.txt", temp);
    ok(MakeSureDirectoryPathExists(file), "path of a file\n");
    attributes = GetFileAttributesA(file);
    ok(attributes == INVALID_FILE_ATTRIBUTES, "the file name is not made a directory\n");
    wsprintfA(file, "%stcosdbg\\three", temp);
    ok((GetFileAttributesA(file) & FILE_ATTRIBUTE_DIRECTORY) != 0, "the directory above the file\n");

    wsprintfA(file, "%stcosdbg\\one\\two\\marker.dat", temp);
    h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(h != INVALID_HANDLE_VALUE, "create marker %lu\n", GetLastError());
    CloseHandle(h);
    wsprintfA(dir, "%stcosdbg", temp);
    memset(found, 0, sizeof(found));
    ok(SearchTreeForFile(dir, "marker.dat", found), "SearchTreeForFile\n");
    ok(!lstrcmpiA(found, file), "found '%s' expected '%s'\n", found, file);
    ok(!SearchTreeForFile(dir, "absent.dat", found), "absent file\n");

    wsprintfA(found, "%stcosdbg\\one\\two", temp);
    {
        char out[MAX_PATH];
        ok(FindFileInPath(GetCurrentProcess(), found, "marker.dat", NULL, 0, 0, 0, out, NULL, NULL), "FindFileInPath\n");
        ok(!lstrcmpiA(out, file), "found '%s'\n", out);
        ok(!FindFileInPath(GetCurrentProcess(), temp, "marker.dat", NULL, 0, 0, 0, out, NULL, NULL), "not in that directory\n");
    }

    DeleteFileA(file);
    wsprintfA(file, "%stcosdbg\\one\\two", temp); RemoveDirectoryA(file);
    wsprintfA(file, "%stcosdbg\\one", temp); RemoveDirectoryA(file);
    wsprintfA(file, "%stcosdbg\\three", temp); RemoveDirectoryA(file);
    wsprintfA(file, "%stcosdbg", temp); RemoveDirectoryA(file);

    ok(SymMatchString("CreateFileA", "create*a", FALSE), "wildcard, ignoring case\n");
    ok(!SymMatchString("CreateFileA", "create*a", TRUE), "wildcard, case sensitive\n");
    ok(SymMatchString("abc", "a?c", TRUE), "question mark\n");
    ok(!SymMatchString("abcd", "a?c", TRUE), "question mark, too long\n");
    ok(SymMatchString("anything", "*", TRUE), "star\n");
}

static void test_minidump(void)
{
    char temp[MAX_PATH], path[MAX_PATH];
    HANDLE file, map;
    BYTE *view;
    MINIDUMP_HEADER *header;
    MINIDUMP_DIRECTORY *dir;
    void *stream;
    ULONG size, i;
    BOOL found_threads = FALSE, found_modules = FALSE, found_system = FALSE, found_misc = FALSE, found_user = FALSE;
    DWORD file_size;
    static const char user_data[] = "tcos user stream";
    MINIDUMP_USER_STREAM user = {0x1234abcd, sizeof(user_data), (void *)user_data};
    MINIDUMP_USER_STREAM_INFORMATION user_info = {1, &user};
    int type;

    GetTempPathA(sizeof(temp), temp);
    for (type = 0; type < 2; type++)
    {
        wsprintfA(path, "%stcos%d.dmp", temp, type);
        file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok(file != INVALID_HANDLE_VALUE, "create dump file %lu\n", GetLastError());
        ok(!MiniDumpWriteDump(NULL, 0, file, MiniDumpNormal, NULL, NULL, NULL), "NULL process\n");
        ok(MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type ? MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo : MiniDumpNormal, NULL, &user_info, NULL),
           "MiniDumpWriteDump %lu\n", GetLastError());
        file_size = GetFileSize(file, NULL);
        ok(file_size > sizeof(MINIDUMP_HEADER), "dump size %lu\n", file_size);
        map = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
        view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
        ok(view != NULL, "map the dump %lu\n", GetLastError());
        if (view)
        {
            header = (MINIDUMP_HEADER *)view;
            ok(header->Signature == MINIDUMP_SIGNATURE && (header->Version & 0xffff) == (MINIDUMP_VERSION & 0xffff), "header %#lx %#lx\n", header->Signature, header->Version);
            ok(header->NumberOfStreams >= 5, "streams %lu\n", header->NumberOfStreams);
            for (i = 0; i < header->NumberOfStreams; i++)
            {
                dir = (MINIDUMP_DIRECTORY *)(view + header->StreamDirectoryRva) + i;
                ok(MiniDumpReadDumpStream(view, dir->StreamType, &dir, &stream, &size), "MiniDumpReadDumpStream %lu\n", i);
                ok(dir->Location.Rva + (ULONGLONG)size <= file_size, "stream %lu inside the file\n", i);
                switch (dir->StreamType)
                {
                case ThreadListStream:
                {
                    MINIDUMP_THREAD_LIST *list = stream;
                    ULONG t;
                    found_threads = TRUE;
                    ok(list->NumberOfThreads >= 1, "threads\n");
                    for (t = 0; t < list->NumberOfThreads; t++)
                        if (list->Threads[t].ThreadId == GetCurrentThreadId())
                        {
                            CONTEXT *ctx = (CONTEXT *)(view + list->Threads[t].ThreadContext.Rva);
                            ok(list->Threads[t].ThreadContext.DataSize == sizeof(CONTEXT), "context size\n");
                            ok(ctx->Rsp != 0 && ctx->Rip != 0, "context of the calling thread: rip %I64x rsp %I64x\n", ctx->Rip, ctx->Rsp);
                            ok(list->Threads[t].Stack.StartOfMemoryRange <= ctx->Rsp && list->Threads[t].Stack.Memory.DataSize > 0, "stack of the calling thread\n");
                            break;
                        }
                    ok(t < list->NumberOfThreads, "the calling thread is in the list\n");
                    break;
                }
                case ModuleListStream:
                {
                    MINIDUMP_MODULE_LIST *list = stream;
                    ULONG m;
                    BOOL k32 = FALSE;
                    found_modules = TRUE;
                    ok(list->NumberOfModules >= 2, "modules %lu\n", list->NumberOfModules);
                    for (m = 0; m < list->NumberOfModules; m++)
                    {
                        MINIDUMP_STRING *name = (MINIDUMP_STRING *)(view + list->Modules[m].ModuleNameRva);
                        if (list->Modules[m].BaseOfImage == (DWORD64)(ULONG_PTR)GetModuleHandleA("kernel32.dll")) k32 = TRUE;
                        ok(name->Length > 0 && name->Buffer[name->Length / 2] == 0, "module name\n");
                    }
                    ok(k32, "kernel32 in the module list\n");
                    break;
                }
                case SystemInfoStream:
                    found_system = TRUE;
                    ok(((MINIDUMP_SYSTEM_INFO *)stream)->ProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "architecture\n");
                    break;
                case MiscInfoStream:
                    found_misc = TRUE;
                    ok(((MINIDUMP_MISC_INFO *)stream)->ProcessId == GetCurrentProcessId(), "process id\n");
                    break;
                case 0x1234abcd:
                    found_user = TRUE;
                    ok(size == sizeof(user_data) && !memcmp(stream, user_data, size), "user stream\n");
                    break;
                case Memory64ListStream:
                {
                    MINIDUMP_MEMORY64_LIST *list = stream;
                    ULONG64 total = 0, r;
                    ok(type == 1, "memory list only in a full dump\n");
                    for (r = 0; r < list->NumberOfMemoryRanges; r++) total += list->MemoryRanges[r].DataSize;
                    ok(list->NumberOfMemoryRanges > 0 && list->BaseRva + total == file_size, "memory data ends at the end of the file: %I64u + %I64u vs %lu\n", list->BaseRva, total, file_size);
                    break;
                }
                }
            }
            ok(found_threads && found_modules && found_system && found_misc && found_user, "streams %d %d %d %d %d\n", found_threads, found_modules, found_system, found_misc, found_user);
            ok(!MiniDumpReadDumpStream(view, 0x7777, &dir, &stream, &size), "a stream type that is not there\n");
            UnmapViewOfFile(view);
        }
        if (map) CloseHandle(map);
        CloseHandle(file);
        DeleteFileA(path);
        found_threads = found_modules = found_system = found_misc = found_user = FALSE;
    }
}

START_TEST(tcos)
{
    test_image_helpers();
    test_symbols();
    test_coff_symbols();
    test_stack_walk();
    test_undecorate();
    test_paths();
    test_minidump();
}
