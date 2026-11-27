# gdb script for a QEMU whose virglrenderer context dies on a host GL error in a draw ("context 1 failed to dispatch DRAW_VBO" in QEMU's output): stops at Mesa's
# message (run QEMU with MESA_DEBUG=1 so that Mesa prints it), when the failing draw's GL context is still current, and prints what the host's GL says of it -
# the current program, glValidateProgram's log (it names the reason: samplers of different types on one unit, ...), the draw buffers, transform feedback, the
# blend factors and the GLSL virglrenderer made of the guest's shaders. Then QEMU is killed.
#   MESA_DEBUG=1 HOST_GDB=scripts/host-gl-error.gdb tests/heaven_qemu.sh direct3d11      (the output is in WORK/qemu.log, lines starting "=== ")
# The stop is a breakpoint on fprintf (which Mesa prints its message with) that only stops for that message - not a catch of write(), which fires on every eventfd
# write and slows QEMU enough to hide a race. The stack cannot be unwound through Mesa's libgallium; virglrenderer's symbols come from debuginfod.
set pagination off
set debuginfod enabled on
set confirm off
set print elements 0
handle SIGUSR1 SIGUSR2 SIGPIPE SIG32 SIG33 SIG34 SIG35 SIG36 nostop noprint pass
python
import gdb

class DrawError(gdb.Breakpoint):
    def stop(self):
        # (__fprintf_chk(file, flag, format, ...): the format and the first arguments, any of which may hold the message)
        for register in ("$rdx", "$rcx", "$r8", "$r9"):
            try:
                text = gdb.parse_and_eval("(char*)" + register).string()      # (to its NUL: a fixed length may run past the mapped memory)
            except Exception:
                continue
            if "GL_INVALID_OPERATION in glDraw" in text:
                print("\n=== GLERR stop: " + text)
                return True
        return False

DrawError("__fprintf_chk")
end
run
python
import gdb
def ev(e): return gdb.parse_and_eval(e)
def ex(c): gdb.execute(c)
try:
    ex("set $iv = (int*)malloc(64)")
    ex("set $buf = (char*)malloc(200000)")
    def geti(pname, index=None):
        if index is None: ex("call ((void(*)(unsigned,int*))epoxy_glGetIntegerv)(%d, $iv)" % pname)
        else: ex("call ((void(*)(unsigned,unsigned,int*))epoxy_glGetIntegeri_v)(%d, %d, $iv)" % (pname, index))
        return int(ev("$iv[0]"))
    prog = geti(0x8B8D)
    print("=== program %d" % prog)
    ex("call ((void(*)(unsigned))epoxy_glValidateProgram)(%d)" % prog)
    ex("call ((void(*)(unsigned,unsigned,int*))epoxy_glGetProgramiv)(%d, 0x8B83, $iv)" % prog)
    print("=== validate status %d" % int(ev("$iv[0]")))
    ex("call ((void(*)(unsigned,int,int*,char*))epoxy_glGetProgramInfoLog)(%d, 200000, 0, $buf)" % prog)
    ex('printf "=== validate log: %s\\n", $buf')
    print("=== draw buffers: %d %d %d %d" % tuple(geti(0x8825 + i) for i in range(4)))
    print("=== program pipeline %d, transform feedback active %d, paused %d" % (geti(0x825A), geti(0x8E24), geti(0x8E23)))
    print("=== vertex array %d, draw framebuffer %d" % (geti(0x85B5), geti(0x8CA6)))
    # (virglrenderer's own state: the shaders it has bound for each stage - vertex, tess control, tess eval, geometry, fragment - and the program ids it linked)
    for e in ["vrend_state.current_ctx->sub->shaders[0]", "vrend_state.current_ctx->sub->shaders[1]", "vrend_state.current_ctx->sub->shaders[2]",
              "vrend_state.current_ctx->sub->shaders[3]", "vrend_state.current_ctx->sub->shaders[4]", "vrend_state.current_ctx->sub->prog_ids",
              "vrend_state.current_ctx->sub->shader_dirty", "vrend_state.current_ctx->sub->ve", "vrend_state.current_ctx->sub->nr_cbufs"]:
        try: print("=== " + e + " = " + str(ev(e)))
        except Exception as x: print("=== " + e + ": " + str(x))
    print("=== draw framebuffer status %x" % int(ev("((unsigned(*)(unsigned))epoxy_glCheckFramebufferStatus)(0x8CA9)")))
    ex("call ((void(*)(unsigned,int*))epoxy_glGetIntegerv)(0x0B40, $iv)")
    print("=== polygon mode %x %x" % (int(ev("$iv[0]")), int(ev("$iv[1]"))))
    for i in range(4):
        print("=== blend %d: src rgb %x, dst rgb %x, src alpha %x, dst alpha %x" % (i, geti(0x80C9, i), geti(0x80C8, i), geti(0x80CB, i), geti(0x80CA, i)))
    ex("set $sh = (unsigned*)malloc(64)")
    ex("call ((void(*)(unsigned,int,int*,unsigned*))epoxy_glGetAttachedShaders)(%d, 8, $iv, $sh)" % prog)
    for k in range(int(ev("$iv[0]"))):
        sh = int(ev("$sh[%d]" % k))
        ex("call ((void(*)(unsigned,unsigned,int*))epoxy_glGetShaderiv)(%d, 0x8B4F, $iv)" % sh)
        print("=== shader %d type %x" % (sh, int(ev("$iv[0]"))))
        ex("call ((void(*)(unsigned,int,int*,char*))epoxy_glGetShaderSource)(%d, 200000, 0, $buf)" % sh)
        ex('printf "%s\\n=== end shader\\n", $buf')
except Exception as x:
    print("=== failed: " + str(x))
end
kill
quit
