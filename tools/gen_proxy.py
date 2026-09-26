"""Generate forwarding thunks + .def for a 32-bit opengl32.dll proxy.

Reads the export table of the real SysWOW64\\opengl32.dll and emits, for every
export we do NOT hook ourselves, a naked jmp thunk through a lazily filled
pointer table.  Hooked exports are declared in HOOKED and implemented by hand
in proxy.c.

Usage:  python gen_proxy.py <out_dir>
"""
import struct
import sys
import os

REAL = r"C:\Windows\SysWOW64\opengl32.dll"

# Exports implemented by hand in proxy.c.  Everything else is a pass-through.
HOOKED = {
    "wglSwapBuffers",
    "wglSwapLayerBuffers",
    "wglCreateContext",
    "wglMakeCurrent",
    "wglDeleteContext",
    "wglGetProcAddress",
    "glViewport",
    "glDrawBuffer",
    "glReadBuffer",
    "glScissor",
    "glGetFloatv",
    "glGetIntegerv",
    "glGetDoublev",
    "glDrawPixels",
    "glBitmap",
    "glCopyPixels",
    "glCopyTexImage2D",
    "glCopyTexSubImage2D",
    "glMatrixMode",
    # A deferred per-eye state needs to be put back before the engine can
    # push, pop or save the projection, viewport or scissor it belongs to.
    "glPushMatrix",
    "glPopMatrix",
    "glPushAttrib",
    "glPopAttrib",
    "glDisable",
    # The per-vertex vocabulary inside glBegin/glEnd, captured into arrays.
    # The redundant-state filter's six: ~14 of these per draw, and the
    # engine re-sets most of them to the value they already hold.
    "glTexEnvi",
    "glTexEnvf",
    "glAlphaFunc",
    "glDepthMask",
    "glShadeModel",
    "glDepthFunc",
    "glMateriali",
    "glMaterialfv",
    "glBindTexture",
    "glColor4ub",
    "glColor4f",
    "glColor4fv",
    "glNormal3f",
    "glTexCoord2f",
    "glLoadIdentity",
    "glLoadMatrixf",
    "glLoadMatrixd",
    "glMultMatrixf",
    "glMultMatrixd",
    "glFrustum",
    "glOrtho",
    "glBegin",
    "glEnd",
    # Only to measure how much of the canvas a 2D primitive covers,
    # which is what tells a dimming backdrop from an icon. Cheap: the
    # world draws through display lists and vertex arrays, so live
    # glVertex calls are a few hundred a frame.
    # A filled quad in one call, bypassing glBegin/glVertex/glEnd
    # entirely -- and therefore invisible to everything else here.
    "glRectf",
    "glRecti",
    "glRectd",
    "glVertex2f",
    "glVertex2i",
    "glVertex2d",
    "glVertex3f",
    "glVertex3i",
    "glVertex3d",
    # Stage 1 mesh capture: the client arrays, and the indices the engine
    # dereferences out of them while a list is being compiled.
    # Stage 2 shadow matrix stack: the operations that were not already
    # hooked. A shadow that diverges is silent, so none of these may be
    # left to chance.
    # The last three engine read-backs, answered by the port so a threaded
    # driver never has to stop mid-frame (tools/no_readbacks2.py).
    "glGetError",
    "glGetMaterialfv",
    "glGetLightfv",
    "glLightfv",
    "glLightf",
    "glTranslatef",
    "glTranslated",
    "glRotatef",
    "glRotated",
    "glScalef",
    "glScaled",
    "glArrayElement",
    "glVertexPointer",
    "glNormalPointer",
    "glTexCoordPointer",
    "glColorPointer",
    "glEnableClientState",
    "glDisableClientState",
    "glNewList",
    "glEndList",
    "glCallList",
    "glCallLists",
    "glDrawArrays",
    "glDrawElements",
    "glClear",
    "glEnable",
    # Once per frame, by the engine.  It makes the CPU wait for the GPU to
    # drain, so the two never overlap.
    "glFinish",
    # Only so the 2D pass can be captured with coverage in the alpha
    # channel; otherwise it is a straight passthrough.
    "glBlendFunc",
}


def exports(path):
    f = open(path, "rb").read()
    pe = struct.unpack_from("<I", f, 0x3C)[0]
    nsec = struct.unpack_from("<H", f, pe + 6)[0]
    optsz = struct.unpack_from("<H", f, pe + 20)[0]
    opt = pe + 24
    dd = opt + 96
    edir_rva, _ = struct.unpack_from("<II", f, dd)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        b = so + 40 * i
        vs, va, rs, ra = struct.unpack_from("<IIII", f, b + 8)
        secs.append((va, vs, ra, rs))

    def r2o(rva):
        for va, vs, ra, rs in secs:
            if va <= rva < va + max(vs, rs):
                return ra + (rva - va)
        raise ValueError("rva %08x unmapped" % rva)

    e = r2o(edir_rva)
    base, nfun, nnam = struct.unpack_from("<III", f, e + 16)
    a_fun, a_nam, a_ord = struct.unpack_from("<III", f, e + 28)
    of, on, oo = r2o(a_fun), r2o(a_nam), r2o(a_ord)
    out = []
    for i in range(nnam):
        nrva = struct.unpack_from("<I", f, on + 4 * i)[0]
        no = r2o(nrva)
        name = f[no:f.index(b"\0", no)].decode()
        idx = struct.unpack_from("<H", f, oo + 2 * i)[0]
        out.append((name, base + idx))
    out.sort(key=lambda x: x[1])
    return out


# A second proxy (winmm) reuses all of this; only the DLL, the hooked set and
# the output names differ.
PRESETS = {
    "opengl32": (REAL, HOOKED, "generated_thunks.c", "opengl32.def",
                 "px_", "hk_", "hkx_", "proxy.h",
                 "g_real", "proxy_bind_passthrough"),
    "winmm": (r"C:\Windows\SysWOW64\winmm.dll",
              {"joyGetNumDevs", "joyGetDevCapsA", "joyGetDevCapsW",
               "joyGetPos", "joyGetPosEx"},
              "winmm_thunks.c", "winmm.def", "wpx_", "whk_", "whkx_",
              "winmm_proxy.h", "wpx_real", "wpx_bind_passthrough"),
}


def main():
    out_dir = sys.argv[1]
    which = sys.argv[2] if len(sys.argv) > 2 else "opengl32"
    (real, hooked_set, cfile, deffile, ppfx, hpfx, tpfx, hdr,
     arrname, bindname) = PRESETS[which]
    exp = exports(real)
    passthru = [(n, o) for n, o in exp if n not in hooked_set]
    hooked = [(n, o) for n, o in exp if n in hooked_set]
    missing = hooked_set - {n for n, _ in exp}
    if missing:
        sys.stderr.write("WARNING: not exported by real DLL: %s\n" % sorted(missing))

    c = []
    c.append("/* GENERATED by tools/gen_proxy.py -- do not edit. */")
    c.append('#include "%s"' % hdr)
    c.append("")
    c.append("void *%s[%d];" % (arrname, len(passthru)))
    c.append("unsigned %s_count[%d];" % (arrname, len(passthru)))
    c.append("unsigned %s_calls;   /* every call the GAME makes through a"
             " thunk */" % arrname)
    c.append("const int %s_n = %d;" % (arrname, len(passthru)))
    c.append("")
    c.append("const char *const k_names_%s[%d] = {" % (ppfx, len(passthru)))
    for n, _ in passthru:
        c.append('    "%s",' % n)
    c.append("};")
    c.append("")
    c.append("void %s(HMODULE h) {" % bindname)
    c.append("    int i;")
    c.append("    for (i = 0; i < %d; i++) %s[i] = (void *)GetProcAddress(h, k_names_%s[i]);"
             % (len(passthru), arrname, ppfx))
    c.append("}")
    c.append("")
    for i, (n, _) in enumerate(passthru):
        c.append("__declspec(naked) void %s%s(void) { __asm { inc dword ptr [%s_count + %d] } "
                 "__asm { inc dword ptr [%s_calls] } "
                 "__asm { jmp dword ptr [%s + %d] } }"
                 % (ppfx, n, arrname, i * 4, arrname, arrname, i * 4))
    c.append("")
    c.append("/* Undecorated cdecl wrappers so the .def can alias __stdcall hooks by")
    c.append("   plain name.  A bare jmp leaves the caller's stack frame untouched, so")
    c.append("   the __stdcall hook's `ret N` returns correctly to the game. */")
    for n, _ in hooked:
        c.append("__declspec(naked) void %s%s(void) { __asm { jmp %s%s } }"
                 % (tpfx, n, hpfx, n))
    c.append("")

    d = ["LIBRARY " + which, "EXPORTS"]
    for n, o in passthru:
        d.append("    %s = %s%s @%d" % (n, ppfx, n, o))
    for n, o in hooked:
        d.append("    %s = %s%s @%d" % (n, tpfx, n, o))

    open(os.path.join(out_dir, cfile), "w").write("\n".join(c) + "\n")
    open(os.path.join(out_dir, deffile), "w").write("\n".join(d) + "\n")
    print("exports=%d passthrough=%d hooked=%d" % (len(exp), len(passthru), len(hooked)))


if __name__ == "__main__":
    main()
