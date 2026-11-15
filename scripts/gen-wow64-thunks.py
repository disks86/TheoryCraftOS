#!/usr/bin/env python3
"""Generates src/Windows/ntdll/wow64_services.inc: the Nt* functions of the 32-bit ntdll.dll (docs/Wow64.md).

A 32-bit program's ntdll.dll reaches the kernel's services - which are written for 64-bit programs - through INT 2E, with every argument widened to
64 bits and the structures that depend on the pointer size converted. This script writes one C function per service from its prototype (the
`NTSTATUS NTAPI NtX(types);` lines of src/Windows), by the type of each parameter:

  a handle                         sign-extended (so the pseudo handles -1 and -2 stay what they are)
  LONG, NTSTATUS, int              sign-extended
  another integer, a pointer       zero-extended (what it points at is used as it is: bytes, 32- and 64-bit integers, LARGE_INTEGER,
                                   IO_STATUS_BLOCK - which the kernel writes in the caller's layout)
  PHANDLE, PVOID *, PSIZE_T ...    a pointer to a pointer-sized value: a 64-bit copy is passed and its low half stored back
  POBJECT_ATTRIBUTES               a 64-bit OBJECT_ATTRIBUTES (and UNICODE_STRING) is passed
  PUNICODE_STRING                  a 64-bit UNICODE_STRING is passed (it is read, not written)
  PCLIENT_ID                       a 64-bit CLIENT_ID is passed

The services whose buffers have a layout of their own (NtQueryVirtualMemory, the handle array of NtWaitForMultipleObjects, ...) are written by hand in
src/Windows/ntdll/wow64.inc and listed in SPECIAL here. Only the services ntdll.dll has real stubs for are generated (NT_SERVICE and NT_SERVICE_RAW
outside services.inc); the others are not exported by the 32-bit ntdll.dll.

Usage: scripts/gen-wow64-thunks.py   (from the repository's root; writes the file)"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWS = os.path.join(ROOT, "src", "Windows")
OUT = os.path.join(WINDOWS, "ntdll", "wow64_services.inc")

# Written by hand in wow64.inc (the generated file leaves them out).
SPECIAL = {"NtWaitForMultipleObjects", "NtQueryVirtualMemory", "NtQueryInformationProcess", "NtQueryInformationThread", "NtCreateUserProcess",
           "NtGetContextThread", "NtSetContextThread", "NtContinue", "NtRaiseException", "NtQuerySystemInformation", "NtQueryObject"}

HANDLES = {"HANDLE", "HMODULE", "HINSTANCE", "HKEY"}
SIGNED = {"LONG", "NTSTATUS", "INT", "int", "BOOL"}
WIDE = {"ULONG64", "LONG64", "LONGLONG", "ULONGLONG", "LARGE_INTEGER", "ULARGE_INTEGER", "DWORD64", "UINT64", "INT64"}
POINTER_SIZED_OUT = {"PHANDLE", "HANDLE *", "PVOID *", "PSIZE_T", "SIZE_T *", "PULONG_PTR", "ULONG_PTR *", "HMODULE *", "LPVOID *", "void **", "HKEY *", "PHKEY"}

def kind(type_text):
    t = " ".join(type_text.replace("const ", "").split())
    if t in POINTER_SIZED_OUT:
        return "H"
    if t == "POBJECT_ATTRIBUTES" or t == "OBJECT_ATTRIBUTES *":
        return "O"
    if t in ("PUNICODE_STRING", "UNICODE_STRING *", "PCUNICODE_STRING"):
        return "U"
    if t in ("PCLIENT_ID", "CLIENT_ID *"):
        return "C"
    if t in HANDLES:
        return "h"
    if t in SIGNED:
        return "s"
    if t in WIDE:
        return "q"
    return "u"

def parameters(text):
    text = text.strip()
    if text in ("", "void"):
        return []
    out, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(text[start:i].strip())
            start = i + 1
    out.append(text[start:].strip())
    return out

def strip_name(parameter):
    """'HANDLE process' -> 'HANDLE' (a prototype may name its parameters; the types here are one word, or end in '*')."""
    m = re.match(r"^(.*[\s\*])([A-Za-z_][A-Za-z0-9_]*)$", parameter)
    if m and m.group(1).strip() not in ("const", "unsigned", "struct") and not parameter.rstrip().endswith("*"):
        head = m.group(1).strip()
        if head:
            return head
    return parameter

def main():
    sources = sorted(glob.glob(os.path.join(WINDOWS, "ntdll", "*.c")) + glob.glob(os.path.join(WINDOWS, "ntdll", "*.inc")))
    stubs, raw = [], set()
    for path in sources:
        if os.path.basename(path) in ("services.inc", "zw.inc", "wow64_services.inc", "wow64.inc"):
            continue
        for m in re.finditer(r"^NT_SERVICE(_RAW)?\((Nt[A-Za-z0-9]+)\);", open(path).read(), re.M):
            if m.group(2) not in stubs:
                stubs.append(m.group(2))
            if m.group(1):
                raw.add(m.group(2))
    prototypes = {}
    for path in [os.path.join(WINDOWS, "tcos_win.h"), os.path.join(WINDOWS, "kernel32.h"), os.path.join(WINDOWS, "ntsec.h")] + sources:
        if not os.path.exists(path):
            continue
        for m in re.finditer(r"NTSTATUS\s+NTAPI\s+(Nt[A-Za-z0-9]+)\(([^;{]*?)\)\s*;", open(path).read()):
            prototypes.setdefault(m.group(1), [strip_name(p) for p in parameters(m.group(2))])
    lines = ["/* The Nt* functions of the 32-bit ntdll.dll: generated by scripts/gen-wow64-thunks.py from the services' prototypes. Do not edit; see that script",
             " * for what each kind of parameter becomes, and wow64.inc for the services written by hand. */"]
    missing = []
    for name in sorted(stubs):
        if name in SPECIAL:
            continue
        if name not in prototypes:
            missing.append(name)
            continue
        types = prototypes[name]
        kinds = [kind(t) for t in types]
        symbol = ("Raw" if name in raw else "") + name
        head = ("static " if name in raw else "EXPORT ") + "NTSTATUS NTAPI " + symbol + "(" + (", ".join(f"{t} a{i}" for i, t in enumerate(types)) or "void") + ") {"
        body, post = [], []
        count = max(len(types), 4)
        body.append(f"  ULONG64 v[{count}] = {{0}};")
        for i, (t, k) in enumerate(zip(types, kinds)):
            if k == "h":
                body.append(f"  v[{i}] = WOW_HANDLE(a{i});")
            elif k == "s":
                body.append(f"  v[{i}] = (ULONG64)(LONG64)a{i};")
            elif k == "q":
                body.append(f"  v[{i}] = (ULONG64)a{i}.QuadPart;" if "LARGE_INTEGER" in t else f"  v[{i}] = (ULONG64)a{i};")
            elif k == "u":
                body.append(f"  v[{i}] = (ULONG64)(ULONG_PTR)a{i};")
            elif k == "H":
                handle = "HANDLE" in t or "HKEY" in t or "HMODULE" in t
                body.append(f"  ULONG64 w{i} = a{i} ? {'WOW_HANDLE(*a' + str(i) + ')' if handle else '(ULONG64)(ULONG_PTR)*a' + str(i)} : 0;")
                body.append(f"  v[{i}] = a{i} ? WOW_POINTER(&w{i}) : 0;")
                post.append(f"  if (a{i}) *a{i} = (__typeof__(*a{i}))(ULONG_PTR)w{i};")
            elif k == "O":
                body.append(f"  WOW_OBJECT_ATTRIBUTES o{i};")
                body.append(f"  v[{i}] = wow_object_attributes(&o{i}, a{i});")
            elif k == "U":
                body.append(f"  UNICODE_STRING64 s{i};")
                body.append(f"  v[{i}] = wow_unicode_string(&s{i}, a{i});")
            elif k == "C":
                body.append(f"  ULONG64 c{i}[2] = {{a{i} ? WOW_HANDLE(a{i}->UniqueProcess) : 0, a{i} ? WOW_HANDLE(a{i}->UniqueThread) : 0}};")
                body.append(f"  v[{i}] = a{i} ? WOW_POINTER(c{i}) : 0;")
        body.append(f"  const NTSTATUS status = wow_service(NT_SERVICE_{name}, v);")
        lines.append(head)
        lines += body + post
        lines.append("  return status;")
        lines.append("}")
        if name not in raw:      # the Zw name is the same call
            arguments = ", ".join(f"a{i}" for i in range(len(types)))
            lines.append("EXPORT NTSTATUS NTAPI Zw" + name[2:] + "(" + (", ".join(f"{t} a{i}" for i, t in enumerate(types)) or "void") + f") {{ return {name}({arguments}); }}")
    open(OUT, "w").write("\n".join(lines) + "\n")
    print(f"{OUT}: {len(stubs) - len(missing) - len(SPECIAL & set(stubs))} services generated, {len(SPECIAL & set(stubs))} by hand, no prototype for: {' '.join(missing) or '(none)'}")

if __name__ == "__main__":
    main()
