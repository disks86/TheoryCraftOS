#!/usr/bin/env python3
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Generates the two halves of the OpenGL entry points from Khronos' registry (src/OpenGl/khronos/gl.xml):
#   src/OpenGl/generated/GlApiDecl.inc   the GlApi methods the kernel has (one per `GlApi::glName(` definition found in src/OpenGl/*.cpp)
#   src/OpenGl/generated/GlDecode.inc    the switch that turns a stream command back into a method call
#   src/OpenGl/generated/GlOpNames.inc   opcode -> name, for messages and the coverage report
#   src/OpenGl/generated/GlEntries.inc   the user-mode side: one C function per command and alias, that writes the command into the stream
#   src/OpenGl/generated/GlProcs.inc     sorted name -> function table behind wglGetProcAddress
#   src/Windows/stubs/opengl32.inc       the exports of opengl32.dll that the above does not cover
# Run it after adding a method to GlApi. `--report` prints coverage per GL version instead.
#
# How a command travels: scalars and arrays of a size fixed by the registry go inline in the stream and the call returns at once. A command with any other pointer, or a result,
# carries the raw address, is sent at once (with everything queued before it) and the kernel reads or writes the program's memory while the call waits. See src/OpenGl/GlStream.hpp.
import os, re, sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "src/OpenGl/generated")

WORD_TYPES = {"GLenum": "E", "GLboolean": "B", "GLbitfield": "U", "GLbyte": "I", "GLshort": "I", "GLint": "I", "GLclampx": "I", "GLubyte": "U", "GLushort": "U", "GLuint": "U", "GLsizei": "I",
              "GLfloat": "F", "GLclampf": "F", "GLchar": "I", "GLcharARB": "I", "GLhalf": "U", "GLhalfARB": "U", "GLhalfNV": "U", "GLfixed": "I", "GLhandleARB": "U", "GLvdpauSurfaceNV": "I"}
LONG_TYPES = {"GLdouble": "D", "GLclampd": "D", "GLint64": "L", "GLint64EXT": "L", "GLuint64": "L", "GLuint64EXT": "L", "GLintptr": "L", "GLintptrARB": "L", "GLsizeiptr": "L", "GLsizeiptrARB": "L",
              "GLsync": "P", "GLeglImageOES": "P", "GLeglClientBufferEXT": "P"}
CALLBACKS = {"GLDEBUGPROC", "GLDEBUGPROCARB", "GLDEBUGPROCKHR", "GLDEBUGPROCAMD", "GLVULKANPROCNV"}
ELEMENT_BYTES = {"GLbyte": 1, "GLubyte": 1, "GLboolean": 1, "GLchar": 1, "GLshort": 2, "GLushort": 2, "GLhalf": 2, "GLhalfNV": 2, "GLint": 4, "GLuint": 4, "GLenum": 4, "GLsizei": 4, "GLfloat": 4,
                 "GLclampf": 4, "GLfixed": 4, "GLdouble": 8, "GLclampd": 8, "GLint64": 8, "GLuint64": 8}
# These keep the pointer rather than read through it, so they can wait in the stream like a scalar.
POINTER_STORED = {"glVertexPointer", "glNormalPointer", "glColorPointer", "glTexCoordPointer", "glIndexPointer", "glEdgeFlagPointer", "glSecondaryColorPointer", "glFogCoordPointer", "glVertexAttribPointer",
                  "glVertexAttribIPointer", "glVertexAttribLPointer", "glFeedbackBuffer", "glSelectBuffer"}
# The last argument to these is sent at once because they can read client arrays: an array may change the moment the call returns.
ALWAYS_SYNC = {"glDrawArrays", "glDrawArraysInstanced", "glDrawArraysInstancedBaseInstance", "glArrayElement", "glMultiDrawArrays", "glDrawTransformFeedback", "glFlush", "glFinish", "glEndList", "glCallList",
               "glUnmapBuffer", "glUnmapNamedBuffer"}

class Param:
    def __init__(self, node, index):
        self.name = node.find("name").text
        self.c_name = "a%d" % index
        ptype = node.find("ptype")
        self.base = ptype.text if ptype is not None else None
        text = "".join(node.itertext())
        self.decl = text[:text.rindex(self.name)].strip()
        self.len = node.get("len")
        self.stars = self.decl.count("*")
        self.const = "const" in self.decl
        array = re.search(r"\[(\d+)\]", text)
        if array:
            self.stars += 1
            self.len = array.group(1)
            self.decl = self.decl + " *"
    def is_pointer(self):
        return self.stars > 0 or self.base in CALLBACKS or (self.base is None and "void" in self.decl and "*" in self.decl)
    def inline_count(self):
        if self.stars == 1 and self.const and self.base in ELEMENT_BYTES and self.len and self.len.isdigit():
            return int(self.len)
        return 0

class Command:
    def __init__(self, node):
        proto = node.find("proto")
        self.name = proto.find("name").text
        text = "".join(proto.itertext())
        self.ret = text[:text.rindex(self.name)].strip()
        self.params = [Param(p, i) for i, p in enumerate(node.findall("param"))]
        alias = node.find("alias")
        self.alias = alias.get("name") if alias is not None else None
    def is_void(self):
        return self.ret == "void"
    def sync(self):
        if not self.is_void() or self.name in ALWAYS_SYNC:
            return True
        return any(p.is_pointer() and not p.inline_count() for p in self.params) and self.name not in POINTER_STORED

def load():
    root = ET.parse(os.path.join(ROOT, "src/OpenGl/khronos/gl.xml")).getroot()
    commands = {}
    for node in root.find("commands").findall("command"):
        c = Command(node)
        commands[c.name] = c
    versions = {}
    for f in root.findall("feature"):
        if f.get("api") != "gl":
            continue
        version = f.get("number")
        names = set()
        for req in f.findall("require"):
            for c in req.findall("command"):
                names.add(c.get("name"))
        versions[version] = names
    extensions = {}
    for e in root.find("extensions").findall("extension"):
        if "gl" not in (e.get("supported") or "").split("|"):
            continue
        names = set()
        for req in e.findall("require"):
            if req.get("api") in (None, "gl"):
                for c in req.findall("command"):
                    names.add(c.get("name"))
        extensions[e.get("name")] = names
    removed = set()
    for f in root.findall("feature"):
        if f.get("api") == "gl":
            for rem in f.findall("remove"):
                if rem.get("profile") == "core":
                    for c in rem.findall("command"):
                        removed.add(c.get("name"))
    return commands, versions, extensions, removed

def canonical(commands, name):
    seen = 0
    while commands[name].alias and commands[name].alias in commands and seen < 8:
        name = commands[name].alias
        seen += 1
    return name

def implemented_names():
    found = set()
    for d in (os.path.join(ROOT, "src/OpenGl"), GEN):
        for f in sorted(os.listdir(d)):
            if f.endswith(".cpp"):
                found |= set(re.findall(r"\bGlApi::(gl[A-Z0-9]\w*)\s*\(", open(os.path.join(d, f), encoding="utf-8").read()))
    return found

# --- The big regular families of commands (glVertex3fv ...): one line each, generated rather than typed.
INT_NORMALIZE = {"b": ("static_cast<float>(2 * {0} + 1) / 255.0f"), "ub": "static_cast<float>({0}) / 255.0f", "s": "static_cast<float>(2 * {0} + 1) / 65535.0f", "us": "static_cast<float>({0}) / 65535.0f",
                 "i": "static_cast<float>((2.0 * {0} + 1.0) / 4294967295.0)", "ui": "static_cast<float>({0} / 4294967295.0)", "f": "{0}", "d": "static_cast<float>({0})"}
ATTRIB_NORMALIZE = {"b": "static_cast<float>({0}) / 127.0f", "ub": "static_cast<float>({0}) / 255.0f", "s": "static_cast<float>({0}) / 32767.0f", "us": "static_cast<float>({0}) / 65535.0f",
                    "i": "static_cast<float>({0} / 2147483647.0)", "ui": "static_cast<float>({0} / 4294967295.0)"}

def values_of(c, skip=0):
    # The values a command carries: its scalar arguments, or the elements of its one array.
    params = c.params[skip:]
    if len(params) == 1 and params[0].inline_count():
        return ["%s[%d]" % (params[0].name, i) for i in range(params[0].inline_count())]
    return [p.name for p in params]

def as_float(v):
    return "static_cast<float>(%s)" % v

def method(c, body):
    args = ", ".join("%s %s" % (kernel_type(p), p.name) for p in c.params)
    ret = "" if c.is_void() else "return "
    return "%s GlApi::%s(%s) { %s%s; }\n" % (kernel_ret(c), c.name, args, ret, body)

def variant_body(c):
    n = c.name
    m = re.match(r"^glVertex([234])(d|f|i|s)v?$", n)
    if m and all(p.is_pointer() is False or p.inline_count() for p in c.params):
        v = [as_float(x) for x in values_of(c)] + ["0.0f", "1.0f"][int(m.group(1)) - 2:]
        return "imm_vertex(%s)" % ", ".join(v)
    m = re.match(r"^glColor([34])(b|d|f|i|s|ub|ui|us)v?$", n)
    if m:
        v = [INT_NORMALIZE[m.group(2)].format(x) for x in values_of(c)]
        if len(v) == 3: v.append("1.0f")
        return "set_color(%s)" % ", ".join(v)
    m = re.match(r"^glSecondaryColor3(b|d|f|i|s|ub|ui|us)v?$", n)
    if m:
        return "set_secondary(%s)" % ", ".join(INT_NORMALIZE[m.group(1)].format(x) for x in values_of(c))
    m = re.match(r"^glNormal3(b|d|f|i|s)v?$", n)
    if m:
        return "set_normal(%s)" % ", ".join(INT_NORMALIZE[m.group(1)].format(x) for x in values_of(c))
    m = re.match(r"^glTexCoord([1234])(d|f|i|s)v?$", n)
    if m:
        v = [as_float(x) for x in values_of(c)]
        v += ["0.0f", "0.0f", "1.0f"][len(v) - 1:]
        return "set_texcoord(0, %s)" % ", ".join(v)
    m = re.match(r"^glMultiTexCoord([1234])(d|f|i|s)v?$", n)
    if m:
        target = c.params[0].name
        v = [as_float(x) for x in values_of(c, 1)]
        v += ["0.0f", "0.0f", "1.0f"][len(v) - 1:]
        return "set_texcoord(%s - 0x84C0u, %s)" % (target, ", ".join(v))
    m = re.match(r"^glFogCoord(f|d)v?$", n)
    if m:
        return "set_fog_coord(%s)" % as_float(values_of(c)[0])
    m = re.match(r"^glIndex(d|f|i|s|ub)v?$", n)
    if m:
        return "set_index(%s)" % as_float(values_of(c)[0])
    if n in ("glEdgeFlag", "glEdgeFlagv"):
        return "set_edge(%s != 0)" % values_of(c)[0]
    m = re.match(r"^glRasterPos([234])(d|f|i|s)v?$", n)
    if m:
        v = [as_float(x) for x in values_of(c)] + ["0.0f", "1.0f"][int(m.group(1)) - 2:]
        return "raster_pos(%s)" % ", ".join(v)
    m = re.match(r"^glWindowPos([23])(d|f|i|s)v?$", n)
    if m:
        v = [as_float(x) for x in values_of(c)] + ["0.0f"][int(m.group(1)) - 2:]
        return "window_pos(%s)" % ", ".join(v)
    m = re.match(r"^glRect(d|f|i|s)$", n)
    if m:
        return "rect(%s)" % ", ".join(as_float(x) for x in values_of(c))
    m = re.match(r"^glRect(d|f|i|s)v$", n)
    if m:
        a, b = c.params[0].name, c.params[1].name
        return "rect(%s, %s, %s, %s)" % (as_float(a + "[0]"), as_float(a + "[1]"), as_float(b + "[0]"), as_float(b + "[1]"))
    m = re.match(r"^glEvalCoord1(d|f)v?$", n)
    if m:
        return "eval_coord1(%s)" % as_float(values_of(c)[0])
    m = re.match(r"^glEvalCoord2(d|f)v?$", n)
    if m:
        return "eval_coord2(%s)" % ", ".join(as_float(x) for x in values_of(c))
    m = re.match(r"^glVertexAttrib([1234])(d|f|s)v?$", n)
    if m:
        v = [as_float(x) for x in values_of(c, 1)]
        v += ["0.0f", "0.0f", "1.0f"][len(v) - 1:]
        return "set_attrib(%s, %s)" % (c.params[0].name, ", ".join(v))
    m = re.match(r"^glVertexAttrib4(b|i|ub|ui|us)v$", n)
    if m:
        return "set_attrib(%s, %s)" % (c.params[0].name, ", ".join(as_float(x) for x in values_of(c, 1)))
    m = re.match(r"^glVertexAttrib4N(b|i|s|ub|ui|us)v?$", n) or re.match(r"^glVertexAttrib4N(ub)$", n)
    if m:
        return "set_attrib(%s, %s)" % (c.params[0].name, ", ".join(ATTRIB_NORMALIZE[m.group(1)].format(x) for x in values_of(c, 1)))
    m = re.match(r"^glVertexAttribI([1234])(i|ui)v?$", n)
    if m:
        v = values_of(c, 1)
        v = ["static_cast<int32_t>(%s)" % x for x in v] + ["0", "0", "1"][len(v) - 1:]
        return "set_attrib_int(%s, %s, %s)" % (c.params[0].name, ", ".join(v), "true" if m.group(2) == "ui" else "false")
    m = re.match(r"^glVertexAttribI4(b|s|ub|us)v$", n)
    if m:
        v = ["static_cast<int32_t>(%s)" % x for x in values_of(c, 1)]
        return "set_attrib_int(%s, %s, %s)" % (c.params[0].name, ", ".join(v), "true" if m.group(1).startswith("u") else "false")
    m = re.match(r"^glVertexAttribL([1234])dv?$", n)
    if m:
        v = values_of(c, 1)
        v = v + ["0.0", "0.0", "1.0"][len(v) - 1:]
        return "set_attrib_double(%s, %s)" % (c.params[0].name, ", ".join(v))
    m = re.match(r"^glUniform([1234])(f|i|ui)$", n)
    if m:
        k = {"f": "'f'", "i": "'i'", "ui": "'u'"}[m.group(2)]
        vals = c.params[1:]
        lanes = ", ".join(("__builtin_bit_cast(uint32_t, %s)" % p.name) if m.group(2) == "f" else ("static_cast<uint32_t>(%s)" % p.name) for p in vals)
        return "const uint32_t lanes[%d] = {%s}; uniform_scalars(location, %s, %d, lanes)" % (len(vals), lanes, k, len(vals))
    m = re.match(r"^glUniform([1234])(f|i|ui|d)v$", n)
    if m:
        k = {"f": "'f'", "i": "'i'", "ui": "'u'", "d": "'f'"}[m.group(2)]
        return "uniform_values(location, %s, 1, %s, count, value, false, %s)" % (k, m.group(1), "true" if m.group(2) == "d" else "false")
    m = re.match(r"^glUniformMatrix([234])(?:x([234]))?(f|d)v$", n)
    if m:
        cols = m.group(1)
        rows = m.group(2) or cols
        return "uniform_values(location, 'f', %s, %s, count, value, transpose != 0, %s)" % (cols, rows, "true" if m.group(3) == "d" else "false")
    m = re.match(r"^glProgramUniform([1234])(f|i|ui)$", n)
    if m:
        k = {"f": "'f'", "i": "'i'", "ui": "'u'"}[m.group(2)]
        vals = c.params[2:]
        lanes = ", ".join(("__builtin_bit_cast(uint32_t, %s)" % p.name) if m.group(2) == "f" else ("static_cast<uint32_t>(%s)" % p.name) for p in vals)
        return "const uint32_t lanes[%d] = {%s}; const uint32_t saved = begin_program_uniform(program); uniform_scalars(location, %s, %d, lanes); end_program_uniform(saved)" % (len(vals), lanes, k, len(vals))
    m = re.match(r"^glProgramUniform([1234])(f|i|ui|d)v$", n)
    if m:
        k = {"f": "'f'", "i": "'i'", "ui": "'u'", "d": "'f'"}[m.group(2)]
        return "const uint32_t saved = begin_program_uniform(program); uniform_values(location, %s, 1, %s, count, value, false, %s); end_program_uniform(saved)" % (k, m.group(1), "true" if m.group(2) == "d" else "false")
    m = re.match(r"^glProgramUniformMatrix([234])(?:x([234]))?(f|d)v$", n)
    if m:
        cols = m.group(1)
        rows = m.group(2) or cols
        return "const uint32_t saved = begin_program_uniform(program); uniform_values(location, 'f', %s, %s, count, value, transpose != 0, %s); end_program_uniform(saved)" % (cols, rows, "true" if m.group(3) == "d" else "false")
    return None

def generate_variants(commands, versions):
    wanted = set()
    for names in versions.values():
        wanted |= names
    out = [LICENSE, '#include "OpenGl/GlApi.hpp"\n\nnamespace opengl {\n\n']
    for name in sorted(wanted):
        c = commands[name]
        body = variant_body(c)
        if body:
            out.append(method(c, body))
    out.append("\n}  // namespace opengl\n")
    write("src/OpenGl/generated/GlApiVariants.cpp", "".join(out))

def kernel_type(p):
    if p.inline_count():
        return "ArrayView<const %s>" % p.base
    if p.is_pointer():
        return "UserPtr"
    return p.base

def kernel_ret(c):
    if c.is_void():
        return "void"
    return "uint64_t"

def words_of(p):
    n = p.inline_count()
    if n:
        return (n * ELEMENT_BYTES[p.base] + 3) // 4
    if p.is_pointer():
        return 2
    return 2 if p.base in LONG_TYPES else 1

def read_expr(p):
    if p.is_pointer():
        return "UserPtr{rd.u64()}"
    if p.base in LONG_TYPES:
        k = LONG_TYPES[p.base]
        if k == "D":
            return "rd.f64()"
        return "static_cast<%s>(rd.u64())" % p.base
    k = WORD_TYPES.get(p.base)
    if k == "F":
        return "rd.f32()"
    if k is None:
        raise Exception("unknown type " + str(p.base))
    return "static_cast<%s>(rd.u32())" % p.base

LICENSE = "// Generated by scripts/gen-opengl.py from src/OpenGl/khronos/gl.xml. Do not edit.\n"

def write(path, text):
    full = os.path.join(ROOT, path)
    old = open(full, encoding="utf-8").read() if os.path.exists(full) else None
    if old != text:
        with open(full, "w", encoding="utf-8") as f:
            f.write(text)

# What the specification says display lists do not record (and everything after GL 2.1, which runs at once).
NOT_LISTED_PREFIX = ("glGet", "glIs", "glGen", "glCreate", "glDelete", "glMap", "glUnmap", "glBindBuffer", "glBufferData", "glBufferSubData", "glAttachShader", "glDetachShader", "glShaderSource", "glCompileShader",
                     "glLinkProgram", "glValidateProgram", "glBindAttribLocation", "glReadPixels", "glPixelStore", "glRenderMode", "glFeedbackBuffer", "glSelectBuffer", "glFinish", "glFlush", "glNewList", "glEndList",
                     "glPushClientAttrib", "glPopClientAttrib", "glEnableClientState", "glDisableClientState", "glClientActiveTexture", "glInterleavedArrays", "glAreTexturesResident", "glEnableVertexAttribArray",
                     "glDisableVertexAttribArray", "glVertexAttribPointer", "glVertexPointer", "glNormalPointer", "glColorPointer", "glTexCoordPointer", "glIndexPointer", "glEdgeFlagPointer", "glSecondaryColorPointer",
                     "glFogCoordPointer")
ALLOWED_IN_BEGIN = re.compile(r"^gl(Vertex|Color|SecondaryColor|Normal|TexCoord|MultiTexCoord|Index|FogCoord|EdgeFlag|Material|EvalCoord|EvalPoint|ArrayElement|CallList|CallLists|End|VertexAttrib\d|Rect$)")

def op_flags(name, versions, removed, new_commands):
    flags = 0
    if name not in new_commands and not name.startswith(NOT_LISTED_PREFIX):
        flags |= 1
    if ALLOWED_IN_BEGIN.match(name) and name not in ("glRectf",):
        flags |= 2
    if name in removed:
        flags |= 4
    return flags

def generate_enums():
    used = set()
    for root, _, files in os.walk(os.path.join(ROOT, "src/OpenGl")):
        if "generated" in root or "khronos" in root:
            continue
        for f in files:
            if f.endswith((".cpp", ".hpp")):
                text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', open(os.path.join(root, f), encoding="utf-8").read())
                text = re.sub(r"//[^\n]*", "", text)
                used |= set(re.findall(r"\bGL_[A-Z0-9_a-z]+\b", text))
    tree = ET.parse(os.path.join(ROOT, "src/OpenGl/khronos/gl.xml")).getroot()
    values = {}
    for group in tree.findall("enums"):
        for e in group.findall("enum"):
            if e.get("api") in (None, "gl"):
                values.setdefault(e.get("name"), e.get("value"))
    missing = sorted(u for u in used if u not in values)
    if missing:
        sys.exit("enums not in the registry: " + " ".join(missing))
    out = [LICENSE, "#pragma once\n"]
    for name in sorted(used):
        v = values[name]
        kind = "uint64_t" if name == "GL_TIMEOUT_IGNORED" else "uint32_t"
        out.append("constexpr %s %s = %s;\n" % (kind, name, v if not v.startswith("-") else "0xFFFFFFFFu"))
    write("src/OpenGl/generated/GlEnums.inc", "".join(out))

def generate_kernel(commands, ops, versions, removed):
    decl = [LICENSE]
    decode = [LICENSE]
    names = [LICENSE]
    for op, name in enumerate(ops):
        c = commands[name]
        args = ", ".join("%s %s" % (kernel_type(p), p.name) for p in c.params)
        decl.append("%s %s(%s);\n" % (kernel_ret(c), name, args))
        names.append('"%s",\n' % name)
        body = ["case %d: {" % (op + 1)]
        call = []
        for p in c.params:
            n = p.inline_count()
            if n:
                body.append(" %s %s[%d]; rd.array(%s);" % (p.base, p.c_name, n, p.c_name))
                call.append("ArrayView<const %s>(%s, %d)" % (p.base, p.c_name, n))
            else:
                body.append(" const %s %s = %s;" % ("UserPtr" if p.is_pointer() else p.base, p.c_name, read_expr(p)))
                call.append(p.c_name)
        expr = "api.%s(%s)" % (name, ", ".join(call))
        body.append((" result = %s;" % expr) if not c.is_void() else (" %s;" % expr))
        body.append(" break; }\n")
        decode.append("".join(body))
    write("src/OpenGl/generated/GlApiDecl.inc", "".join(decl))
    write("src/OpenGl/generated/GlDecode.inc", "".join(decode))
    write("src/OpenGl/generated/GlOpNames.inc", "".join(names))
    old = set()
    for v, n in versions.items():
        if tuple(int(x) for x in v.split(".")) <= (2, 1):
            old |= n
    new_commands = set(ops) - old
    flags = [LICENSE] + ["%d,\n" % op_flags(name, versions, removed, new_commands) for name in ops]
    write("src/OpenGl/generated/GlOpFlags.inc", "".join(flags))
    ids = [LICENSE, "#pragma once\n"] + ["constexpr uint32_t kOp_%s = %d;\n" % (name, i + 1) for i, name in enumerate(ops)] + ["constexpr uint32_t kOpCount = %d;\n" % len(ops)]
    write("src/OpenGl/generated/GlOpIds.inc", "".join(ids))

def c_param(p):
    text = p.decl
    return "%s %s" % (text, p.c_name)

def stub_body(c, op):
    words = 1 + sum(words_of(p) for p in c.params)
    out = ["  uint32_t *s = gl_stream_begin(%d, %d);\n" % (op + 1, words)]
    at = 1
    for p in c.params:
        n = p.inline_count()
        if n:
            out.append("  gl_stream_copy(s + %d, %s, %d);\n" % (at, p.c_name, n * ELEMENT_BYTES[p.base]))
        elif p.is_pointer():
            out.append("  gl_stream_u64(s + %d, (uint64_t)(uintptr_t)%s);\n" % (at, p.c_name))
        elif p.base in LONG_TYPES:
            k = LONG_TYPES[p.base]
            if k == "D":
                out.append("  gl_stream_f64(s + %d, %s);\n" % (at, p.c_name))
            elif k == "P":
                out.append("  gl_stream_u64(s + %d, (uint64_t)(uintptr_t)%s);\n" % (at, p.c_name))
            else:
                out.append("  gl_stream_u64(s + %d, (uint64_t)%s);\n" % (at, p.c_name))
        elif WORD_TYPES[p.base] == "F":
            out.append("  s[%d] = gl_stream_f32(%s);\n" % (at, p.c_name))
        else:
            out.append("  s[%d] = (uint32_t)%s;\n" % (at, p.c_name))
        at += words_of(p)
    if c.sync():
        out.append("  %sgl_stream_sync(%d);\n" % ("" if c.is_void() else "const uint64_t r = ", words))
        if not c.is_void():
            cast = "(%s)(uintptr_t)r" % c.ret if "*" in c.ret or c.ret == "GLsync" else "(%s)r" % c.ret
            out.append("  return %s;\n" % cast)
    else:
        out.append("  gl_stream_end(%d);\n" % words)
    return "".join(out)

def generate_user(commands, ops, aliases):
    op_of = {n: i for i, n in enumerate(ops)}
    out = [LICENSE]
    procs = []
    for name in ops:
        c = commands[name]
        args = ", ".join(c_param(p) for p in c.params) or "void"
        out.append("GLAPI %s APIENTRY %s(%s) {\n%s}\n" % (c.ret, name, args, stub_body(c, op_of[name])))
        procs.append(name)
    for name in sorted(aliases):
        target = commands[aliases[name]]
        c = commands[name]
        if len(c.params) != len(target.params):
            continue
        args = ", ".join(c_param(p) for p in c.params) or "void"
        call = ", ".join("(%s)%s" % (t.decl, p.c_name) for p, t in zip(c.params, target.params))
        ret = "" if c.is_void() else "return (%s)" % c.ret
        out.append("GLAPI %s APIENTRY %s(%s) { %s%s(%s); }\n" % (c.ret, name, args, ret, target.name, call))
        procs.append(name)
    write("src/OpenGl/generated/GlEntries.inc", "".join(out))
    table = [LICENSE] + ['GL_PROC(%s)\n' % n for n in sorted(procs)]
    write("src/OpenGl/generated/GlProcs.inc", "".join(table))
    return set(procs)

def generate_stub_list(defined):
    path = "src/Windows/stubs/opengl32.inc"
    full = os.path.join(ROOT, path)
    text = open(full, encoding="utf-8").read()
    lines = text.split("\n")
    kept = []
    own = set(defined) | set(re.findall(r"\b(wgl\w+|Glmf\w+|GlmfPlay\w*|glDebugEntry)\b", own_dll_source()))
    for line in lines:
        m = re.match(r"X\((\w+)\)", line)
        if m and m.group(1) in own:
            continue
        kept.append(line)
    write(path, "\n".join(kept))
    return sum(1 for l in kept if l.startswith("X("))

def own_dll_source():
    d = os.path.join(ROOT, "src/Windows/opengl32")
    text = ""
    for f in sorted(os.listdir(d)):
        if f.endswith((".c", ".inc")):
            text += open(os.path.join(d, f), encoding="utf-8").read()
    return " ".join(re.findall(r"WINGDIAPI\s+[\w \*]+?\s+WINAPI\s+(\w+)", text))

def main():
    commands, versions, extensions, removed = load()
    generate_variants(commands, versions)
    implemented = implemented_names()
    unknown = implemented - set(commands)
    if unknown:
        sys.exit("not in the registry: %s" % ", ".join(sorted(unknown)))
    ops = sorted(n for n in implemented)
    if "--report" in sys.argv:
        have = set(ops)
        total_have = total = 0
        for v in sorted(versions, key=lambda s: tuple(int(x) for x in s.split("."))):
            names = versions[v]
            miss = sorted(n for n in names if canonical(commands, n) not in have)
            total += len(names)
            total_have += len(names) - len(miss)
            print("GL %-4s %4d/%4d" % (v, len(names) - len(miss), len(names)), (" missing: " + " ".join(miss)) if miss and "-v" in sys.argv else "")
        print("all versions: %d/%d" % (total_have, total))
        return
    aliases = {}
    for name, c in commands.items():
        if c.alias and canonical(commands, name) in implemented and name not in implemented:
            aliases[name] = canonical(commands, name)
    generate_enums()
    generate_kernel(commands, ops, versions, removed)
    defined = generate_user(commands, ops, aliases)
    left = generate_stub_list(defined)
    print("%d commands, %d aliases, %d exports of opengl32.dll still stubs" % (len(ops), len(aliases), left))

main()
