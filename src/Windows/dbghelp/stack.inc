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

/* Part of dbghelp.c: StackWalk64 for x64, with the unwind interpreter (UNWIND_INFO / .pdata) reading through the caller's memory routine. */

typedef struct {
  HANDLE process;
  PREAD_PROCESS_MEMORY_ROUTINE64 read;
  PFUNCTION_TABLE_ACCESS_ROUTINE64 table;
  PGET_MODULE_BASE_ROUTINE64 module_base;
} WALK;

static BOOL walk_read(WALK *w, DWORD64 address, void *buffer, DWORD n) {
  if (w->read) {
    DWORD got = 0;
    return w->read(w->process, address, buffer, n, &got) && got == n;
  }
  return target_read(w->process, address, buffer, n);
}

/* The unwind data of the function that holds an address; base is the module's. FALSE for a leaf function (no entry). */
static BOOL walk_function(WALK *w, DWORD64 pc, DWORD64 *base, RUNTIME_FUNCTION *entry, PVOID *entry_pointer) {
  *base = w->module_base ? w->module_base(w->process, pc) : SymGetModuleBase64(w->process, pc);
  if (!*base) return FALSE;
  PVOID f = w->table ? w->table(w->process, pc) : SymFunctionTableAccess64(w->process, pc);
  if (!f) return FALSE;
  *entry = *(RUNTIME_FUNCTION *)f;
  if (entry_pointer) *entry_pointer = f;
  return TRUE;
}

#define UWOP_PUSH_NONVOL 0
#define UWOP_ALLOC_LARGE 1
#define UWOP_ALLOC_SMALL 2
#define UWOP_SET_FPREG 3
#define UWOP_SAVE_NONVOL 4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_SAVE_XMM128 8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10
#define UNW_CHAININFO 4

/* One step: the context becomes the caller's (Rip is the return address). The prolog codes are undone from the last to the first; the codes whose instruction has not run
 * yet (the offset of the pc in the function is below the code's) are skipped. A pc inside an epilogue is not recognised: the walk starts at a call site or in a prolog or body. */
static BOOL unwind_with_info(WALK *w, DWORD64 base, const RUNTIME_FUNCTION *entry, CONTEXT *c) {
  DWORD64 *regs = &c->Rax;      /* Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8 .. R15 in this order */
  const DWORD64 offset = c->Rip - (base + entry->BeginAddress);
  RUNTIME_FUNCTION current = *entry;
  BOOL chained = FALSE;
  for (int depth = 0; depth < 32; depth++) {
    DWORD unwind = current.UnwindData;
    for (int hop = 0; (unwind & 1) && hop < 8; hop++) {      /* (indirect: the entry holds the RVA of another entry) */
      RUNTIME_FUNCTION other;
      if (!walk_read(w, base + (unwind & ~3u), &other, sizeof(other))) return FALSE;
      unwind = other.UnwindData;
    }
    BYTE header[4];
    WORD codes[256];
    if (!walk_read(w, base + unwind, header, 4)) return FALSE;
    const unsigned version = header[0] & 7, flags = header[0] >> 3, count = header[2], frame_register = header[3] & 0xF, frame_offset = header[3] >> 4;
    if (version != 1 && version != 2) return FALSE;
    if (count && !walk_read(w, base + unwind + 4, codes, count * 2)) return FALSE;
    for (unsigned i = 0; i < count;) {
      const unsigned code_offset = codes[i] & 0xFF, op = (codes[i] >> 8) & 0xF, info = codes[i] >> 12;
      unsigned slots = 1;
      const BOOL apply = chained || offset >= code_offset;
      switch (op) {
        case UWOP_PUSH_NONVOL:
          if (apply) { if (!walk_read(w, c->Rsp, &regs[info], 8)) return FALSE; c->Rsp += 8; }
          break;
        case UWOP_ALLOC_LARGE:
          if (info == 0) { slots = 2; if (i + 1 >= count) return FALSE; if (apply) c->Rsp += (DWORD64)codes[i + 1] * 8; }
          else { slots = 3; if (i + 2 >= count) return FALSE; if (apply) c->Rsp += (DWORD64)codes[i + 1] | ((DWORD64)codes[i + 2] << 16); }
          break;
        case UWOP_ALLOC_SMALL:
          if (apply) c->Rsp += (DWORD64)info * 8 + 8;
          break;
        case UWOP_SET_FPREG:
          if (apply) c->Rsp = regs[frame_register] - (DWORD64)frame_offset * 16;
          break;
        case UWOP_SAVE_NONVOL:
          slots = 2;
          if (i + 1 >= count) return FALSE;
          if (apply && !walk_read(w, c->Rsp + (DWORD64)codes[i + 1] * 8, &regs[info], 8)) return FALSE;
          break;
        case UWOP_SAVE_NONVOL_FAR:
          slots = 3;
          if (i + 2 >= count) return FALSE;
          if (apply && !walk_read(w, c->Rsp + ((DWORD64)codes[i + 1] | ((DWORD64)codes[i + 2] << 16)), &regs[info], 8)) return FALSE;
          break;
        case 6: slots = 2; break;      /* (UWOP_EPILOG of version 2, UWOP_SAVE_XMM of the first version) */
        case 7: slots = 3; break;
        case UWOP_SAVE_XMM128: slots = 2; break;      /* (the vector registers are not tracked) */
        case UWOP_SAVE_XMM128_FAR: slots = 3; break;
        case UWOP_PUSH_MACHFRAME: {
          if (!apply) break;
          if (info == 1) c->Rsp += 8;      /* (an error code) */
          DWORD64 frame[5];      /* Rip, CS, EFlags, Rsp, SS */
          if (!walk_read(w, c->Rsp, frame, sizeof(frame))) return FALSE;
          c->Rip = frame[0];
          c->Rsp = frame[3];
          return TRUE;
        }
        default: return FALSE;
      }
      i += slots;
    }
    if (!(flags & UNW_CHAININFO)) break;
    RUNTIME_FUNCTION parent;
    if (!walk_read(w, base + unwind + 4 + (DWORD64)((count + 1) & ~1u) * 2, &parent, sizeof(parent))) return FALSE;
    current = parent;
    chained = TRUE;
    if (depth == 31) return FALSE;
  }
  DWORD64 return_address;
  if (!walk_read(w, c->Rsp, &return_address, 8)) return FALSE;
  c->Rip = return_address;
  c->Rsp += 8;
  return TRUE;
}

/* One step for the pc of a context: by the function table, or as a leaf function (the return address is at the top of the stack). */
static BOOL unwind_step(WALK *w, CONTEXT *c, PVOID *entry_pointer) {
  DWORD64 base;
  RUNTIME_FUNCTION entry;
  if (entry_pointer) *entry_pointer = NULL;
  if (walk_function(w, c->Rip, &base, &entry, entry_pointer)) return unwind_with_info(w, base, &entry, c);
  DWORD64 return_address;
  if (!walk_read(w, c->Rsp, &return_address, 8)) return FALSE;
  c->Rip = return_address;
  c->Rsp += 8;
  return TRUE;
}

static void set_flat(ADDRESS64 *a, DWORD64 offset) {
  a->Offset = offset;
  a->Segment = 0;
  a->Mode = AddrModeFlat;
}

static BOOL stack_walk(DWORD machine, HANDLE process, STACKFRAME64 *f, void *context, PREAD_PROCESS_MEMORY_ROUTINE64 read, PFUNCTION_TABLE_ACCESS_ROUTINE64 table,
                       PGET_MODULE_BASE_ROUTINE64 module_base) {
  if (machine != IMAGE_FILE_MACHINE_AMD64 || !f || !context) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  CONTEXT *c = context;
  WALK w = {process, read, table, module_base};
  PVOID entry = NULL;
  if (f->Reserved[0] == 0) {
    /* the first call: the frame is the context itself */
    f->Reserved[0] = 1;
    set_flat(&f->AddrPC, c->Rip);
    set_flat(&f->AddrStack, c->Rsp);
    set_flat(&f->AddrFrame, c->Rbp);
  } else {
    CONTEXT next = *c;
    if (next.Rip == 0 || !unwind_step(&w, &next, NULL)) return FALSE;
    if (next.Rip == 0 || next.Rsp <= c->Rsp) return FALSE;
    *c = next;
    set_flat(&f->AddrPC, c->Rip);
    set_flat(&f->AddrStack, c->Rsp);
    set_flat(&f->AddrFrame, c->Rbp);
  }
  if (c->Rip == 0) return FALSE;
  /* the return address of this frame, and the function entry */
  CONTEXT caller = *c;
  if (unwind_step(&w, &caller, &entry)) set_flat(&f->AddrReturn, caller.Rip); else set_flat(&f->AddrReturn, 0);
  f->FuncTableEntry = entry;
  set_flat(&f->AddrBStore, 0);
  f->Far = FALSE;
  f->Virtual = FALSE;
  f->Params[0] = f->Params[1] = f->Params[2] = f->Params[3] = 0;
  return TRUE;
}

EXPORT BOOL WINAPI StackWalk64(DWORD machine, HANDLE process, HANDLE thread, LPSTACKFRAME64 frame, PVOID context, PREAD_PROCESS_MEMORY_ROUTINE64 read, PFUNCTION_TABLE_ACCESS_ROUTINE64 table,
                               PGET_MODULE_BASE_ROUTINE64 module_base, PTRANSLATE_ADDRESS_ROUTINE64 translate) {
  return stack_walk(machine, process, frame, context, read, table, module_base);
}

/* STACKFRAME_EX begins with a STACKFRAME64; the inline frame context is always "ignore" (there is no inline information). */
EXPORT BOOL WINAPI StackWalkEx(DWORD machine, HANDLE process, HANDLE thread, LPSTACKFRAME_EX frame, PVOID context, PREAD_PROCESS_MEMORY_ROUTINE64 read, PFUNCTION_TABLE_ACCESS_ROUTINE64 table,
                               PGET_MODULE_BASE_ROUTINE64 module_base, PTRANSLATE_ADDRESS_ROUTINE64 translate, DWORD flags) {
  if (!frame) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL r = stack_walk(machine, process, (STACKFRAME64 *)frame, context, read, table, module_base);
  if (r) frame->InlineFrameContext = INLINE_FRAME_CONTEXT_IGNORE;
  return r;
}
