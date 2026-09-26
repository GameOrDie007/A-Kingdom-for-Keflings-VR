/* A Kingdom for Keflings VR -- read the engine's own internals out of the running process.
 *
 * The exe is Armadillo-packed, so there is nothing to read statically: .text
 * and .rdata have zero raw size and the payload is encrypted.  But by the time
 * a frame has been drawn the packer has decrypted the lot into memory, and we
 * are inside that process.
 */
#include "proxy.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 1. The configuration vocabulary.
 *
 * Anchor on ini keys the shipped settings.ini proves the engine reads, and dump
 * what is written beside them -- a compiler lays a string table out together,
 * so the keys it did NOT document sit next to the ones it did.
 * ------------------------------------------------------------------------ */

static const char *const k_anchors[] = {
    "XScreenRes", "ScreenFreq", "FullScreen", "AdjustLOD", "MultiSampling",
    "ForceGraphicsReset", "ContentPack1", "EffectsVolume", "NetworkPort",
    "ConnectToIP", "PlayerName", "YScreenRes",
};
#define NANCHOR (sizeof(k_anchors) / sizeof(k_anchors[0]))

#define NEIGHBOURHOOD 3072      /* bytes either side of an anchor to dump */
#define MAX_HITS      64
#define MAX_OUT       (512 * 1024)

static int is_text(unsigned char c) {
    return (c >= 32 && c < 127);
}

static void emit_runs(FILE *f, const unsigned char *p, size_t n) {
    size_t i = 0;
    char buf[256];
    while (i < n) {
        size_t j = i;
        while (j < n && is_text(p[j])) j++;
        if (j - i >= 4) {
            size_t len = j - i;
            if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
            memcpy(buf, p + i, len);
            buf[len] = 0;
            fprintf(f, "    %s\n", buf);
        }
        i = (j > i) ? j : i + 1;
    }
}

static const unsigned char *find_bytes(const unsigned char *hay, size_t n,
                                       const char *needle) {
    size_t m = strlen(needle);
    size_t i;
    if (m == 0 || n < m) return NULL;
    for (i = 0; i + m <= n; i++)
        if (hay[i] == (unsigned char)needle[0] && !memcmp(hay + i, needle, m))
            return hay + i;
    return NULL;
}

/* shared with fovpatch.c */
FILE *open_out(const char *kind) {
    char path[MAX_PATH], dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 64) return NULL;
    lstrcatA(dir, "\\KeflingsVR");
    CreateDirectoryA(dir, NULL);
    _snprintf(path, MAX_PATH, "%s\\%s-%lu.txt", dir, kind,
              GetCurrentProcessId());
    return fopen(path, "w");
}

void dump_engine_strings(void) {
    static int done;
    FILE *f;
    SYSTEM_INFO si;
    unsigned char *addr;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned hits = 0;
    long written = 0;

    if (done) return;
    done = 1;

    f = open_out("strings");
    if (!f) return;

    fprintf(f, "Engine configuration vocabulary, recovered from the decrypted\n"
               "process image.\n\n");

    GetSystemInfo(&si);
    addr = (unsigned char *)si.lpMinimumApplicationAddress;

    while (addr < (unsigned char *)si.lpMaximumApplicationAddress &&
           hits < MAX_HITS && written < MAX_OUT) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;

        if (mbi.State == MEM_COMMIT && mbi.RegionSize &&
            !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY))) {
            /* This memory was decrypted at runtime by a packer; a fault while
               reading it must not take the game down with it. */
            __try {
                unsigned char *base = (unsigned char *)mbi.BaseAddress;
                size_t size = mbi.RegionSize;
                size_t a;
                for (a = 0; a < NANCHOR && hits < MAX_HITS; a++) {
                    const unsigned char *hit = find_bytes(base, size, k_anchors[a]);
                    const unsigned char *lo, *hi;
                    if (!hit) continue;
                    hits++;
                    lo = hit - NEIGHBOURHOOD;
                    hi = hit + NEIGHBOURHOOD;
                    if (lo < base) lo = base;
                    if (hi > base + size) hi = base + size;
                    fprintf(f, "==== \"%s\" at %p (region %p +%u) ====\n",
                            k_anchors[a], (void *)hit, (void *)base,
                            (unsigned)size);
                    emit_runs(f, lo, (size_t)(hi - lo));
                    fprintf(f, "\n");
                    written = ftell(f);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                /* region is not really readable; skip it */
            }
        }

        if (mbi.RegionSize == 0) break;
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }

    fprintf(f, "\n=== %u anchor hits ===\n", hits);
    fclose(f);
    kv_log("engine string dump written (%u anchors found)", hits);
}

/* ---------------------------------------------------------------------------
 * 2. The engine's own camera field of view.
 *
 * Replacing the GL projection with a wider one does NOT make the engine draw
 * more -- that was measured, and the world still arrives with holes wherever
 * the head turns.  So the culling runs against the engine's own camera data,
 * not against the matrix it hands to GL, and widening the view means finding
 * that data.
 *
 * The foothold: glMultMatrixd is called with a pointer INTO the engine's own
 * storage for its projection.  Whatever object owns that matrix very likely
 * owns the field of view that built it, within a few hundred bytes.  So scan
 * the neighbourhood for every form 45 degrees can take.
 * ------------------------------------------------------------------------ */

typedef struct { double v; const char *what; } Known;

static const Known k_known[] = {
    { 45.0,                "fovy in degrees" },
    { 22.5,                "half fovy in degrees" },
    { 0.78539816339744828, "fovy in radians" },
    { 0.39269908169872414, "half fovy in radians" },
    { 0.41421356237309503, "tan(half fovy)" },
    { 2.4142135623730951,  "1/tan(half fovy) -- the matrix m5" },
    { 1.7777777777777777,  "aspect 16:9" },
    { 0.73640000000000000, "tan(half fovx) at 16:9" },
    { 70.0,                "near plane" },
    { 2200.0,              "far plane" },
};
#define NKNOWN (sizeof(k_known) / sizeof(k_known[0]))

static int near_enough(double a, double b) {
    double d = a - b;
    double mag = (b < 0) ? -b : b;
    if (d < 0) d = -d;
    return d < (1e-5 * (mag + 1e-6));
}

void dump_camera_neighbourhood(const void *proj_matrix) {
    static int done;
    FILE *f;
    const unsigned char *base = (const unsigned char *)proj_matrix;
    long off;
    const long SPAN = 8192;

    if (done || !proj_matrix) return;
    done = 1;

    f = open_out("camera");
    if (!f) return;

    fprintf(f, "The engine's projection matrix lives at %p.\n", proj_matrix);
    fprintf(f, "Scanning +/- %ld bytes around it for the field of view that\n"
               "built it -- that is the value its culling uses, and the only\n"
               "thing that can widen what the engine is willing to draw.\n"
               "Offsets are relative to the matrix.\n\n", SPAN);

    __try {
        for (off = -SPAN; off <= SPAN; off += 4) {
            const unsigned char *p = base + off;
            double d;
            float fl;
            size_t k;
            memcpy(&fl, p, sizeof(fl));
            for (k = 0; k < NKNOWN; k++)
                if (near_enough((double)fl, k_known[k].v))
                    fprintf(f, "  %+6ld  float   %-18.9g  %s\n", off,
                            (double)fl, k_known[k].what);
            if (((ULONG_PTR)p & 7) == 0) {
                memcpy(&d, p, sizeof(d));
                for (k = 0; k < NKNOWN; k++)
                    if (near_enough(d, k_known[k].v))
                        fprintf(f, "  %+6ld  double  %-18.17g  %s\n", off, d,
                                k_known[k].what);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(f, "  (read fault -- scan stopped early)\n");
    }

    fprintf(f, "\nSeveral copies at a fixed stride would be the camera stored\n"
               "per split screen; the engine asserts on WLIB_MAX_SPLIT_SCREENS.\n");
    fclose(f);
    kv_log("camera neighbourhood dump written");
}


/* ---------------------------------------------------------------------------
 * What does the game take input through?
 *
 * Controller support needs two halves: reading the Quest controllers (OpenXR's
 * action system) and getting that INTO the game.  The second half depends
 * entirely on how the engine reads input -- DirectInput, XInput, raw input, or
 * plain Win32 messages -- and that is a fact about the process, not something
 * to guess.  A loaded-module list answers it outright.
 * ------------------------------------------------------------------------ */
void dump_modules(void) {
    static int done;
    FILE *f;
    HMODULE mods[512];
    DWORD needed = 0, i;
    HMODULE psapi;
    BOOL (WINAPI *enumMods)(HANDLE, HMODULE *, DWORD, LPDWORD);
    DWORD (WINAPI *getName)(HANDLE, HMODULE, LPSTR, DWORD);

    if (done) return;
    done = 1;

    psapi = LoadLibraryA("psapi.dll");
    if (!psapi) return;
    enumMods = (void *)GetProcAddress(psapi, "EnumProcessModules");
    getName = (void *)GetProcAddress(psapi, "GetModuleFileNameExA");
    if (!enumMods || !getName) return;

    f = open_out("modules");
    if (!f) return;
    fprintf(f, "Modules loaded by the game.  Looking for how it reads input:\n"
               "  dinput8.dll / dinput.dll  -> DirectInput, proxy it\n"
               "  xinput*.dll               -> XInput, proxy it\n"
               "  neither                   -> Win32 messages or GetAsyncKeyState,\n"
               "                               which means synthesising input instead\n\n");

    if (enumMods(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (i = 0; i < count; i++) {
            char name[MAX_PATH];
            name[0] = 0;
            if (getName(GetCurrentProcess(), mods[i], name, MAX_PATH))
                fprintf(f, "  %s\n", name);
        }
        fprintf(f, "\n%u modules\n", (unsigned)count);
    }
    fclose(f);
    FreeLibrary(psapi);
    kv_log("module list written");
}


/* ---------------------------------------------------------------------------
 * Raw Input, and the game's own vocabulary.
 * ------------------------------------------------------------------------ */
static const char *usage_name(USHORT page, USHORT usage) {
    if (page != 0x01) return "(not a generic desktop device)";
    switch (usage) {
    case 0x02: return "MOUSE";
    case 0x04: return "JOYSTICK  <-- a pad, read through Raw Input";
    case 0x05: return "GAMEPAD   <-- a pad, read through Raw Input";
    case 0x06: return "KEYBOARD";
    case 0x08: return "multi-axis controller";
    default: break;
    }
    return "other";
}

void dump_rawinput(void) {
    static int done;
    FILE *f;
    UINT n = 0;
    RAWINPUTDEVICE devs[32];
    UINT r;

    if (done) return;
    done = 1;
    f = open_out("rawinput");
    if (!f) return;

    fprintf(f, "What this process has registered for Raw Input.\n");
    fprintf(f, "Raw Input needs no DLL of its own, so a game reading a pad this\n"
               "way is invisible in a module list -- which is the one route the\n"
               "earlier measurements could NOT rule out.\n\n");

    r = GetRegisteredRawInputDevices(NULL, &n, sizeof(RAWINPUTDEVICE));
    if (n == 0) {
        fprintf(f, "  NOTHING registered.\n\n"
                   "  So the game does not use Raw Input either.  With\n"
                   "  DirectInput, XInput and the joystick API already ruled\n"
                   "  out by measurement, it reads keyboard and mouse only.\n");
    } else {
        if (n > 32) n = 32;
        r = GetRegisteredRawInputDevices(devs, &n, sizeof(RAWINPUTDEVICE));
        if (r != (UINT)-1) {
            UINT i;
            for (i = 0; i < r; i++)
                fprintf(f, "  usagePage=0x%02X usage=0x%02X flags=0x%lX  %s\n",
                        devs[i].usUsagePage, devs[i].usUsage,
                        (unsigned long)devs[i].dwFlags,
                        usage_name(devs[i].usUsagePage, devs[i].usUsage));
        }
    }
    fclose(f);
    kv_log("raw input registration dumped (%u devices)", n);
}

/* Every printable string in the game's OWN image.  Menu text lives here, so if
   there is a control scheme -- or a gamepad option -- it will show up. */
void dump_game_strings(void) {
    static int done;
    FILE *f;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *base, *addr;
    long written = 0;

    if (done) return;
    done = 1;
    base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base) return;
    f = open_out("gamestrings");
    if (!f) return;
    fprintf(f, "Printable strings inside the game's own image at %p.\n\n",
            (void *)base);

    addr = base;
    while (addr < base + (32u << 20) && written < (2 << 20)) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        if (mbi.AllocationBase != (void *)base) break;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            !(mbi.Protect & PAGE_NOACCESS)) {
            __try {
                emit_runs(f, (unsigned char *)mbi.BaseAddress, mbi.RegionSize);
                written = ftell(f);
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        if (mbi.RegionSize == 0) break;
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    fclose(f);
    kv_log("game string dump written");
}

/* Dump the WORLD camera's structure, annotated, looking for a cull radius.
 *
 * The objects that sit on the ground -- trees, rocks, sheep, crystals -- are
 * clipped by a circle that is not centred on the player.  A frustum cannot do
 * that, and the engine's frustum is 152 degrees wide in any case, so this is a
 * separate distance test against some point that is not the player.  Its
 * radius is a number in here somewhere.
 *
 * Every float within range of a camera block is printed with anything we can
 * already name marked, plus a flag on values in the range a tile radius would
 * plausibly occupy, so the candidates stand out rather than having to be
 * picked out of eight thousand numbers by eye.
 */
void dump_camera_world(float fovy, float aspect, float znear, float zfar) {
    static int done;
    FILE *f;
    unsigned c, nc;

    if (done) return;
    done = 1;
    f = open_out("camera-world");
    if (!f) return;

    nc = fov_candidate_count();
    fprintf(f, "The WORLD camera, in a loaded level.\n");
    fprintf(f, "fovy %.2f  aspect %.4f  near %.1f  far %.1f\n", fovy, aspect,
            znear, zfar);
    fprintf(f, "%u camera blocks.\n\n", nc);
    fprintf(f, "Looking for a CULL RADIUS: the ground objects are clipped by a\n"
               "circle that is not centred on the player, which the frustum\n"
               "cannot be doing. Values flagged <- are in the range such a\n"
               "radius would occupy (roughly a quarter to four times the far\n"
               "plane, or a plain tile count).\n\n");

    for (c = 0; c < nc; c++) {
        unsigned char *base = (unsigned char *)fov_candidate(c);
        long off;
        if (!base) continue;
        fprintf(f, "==== camera block %u at %p ====\n", c + 1, (void *)base);
        __try {
            for (off = -1024; off <= 1024; off += 4) {
                float v;
                const char *tag = "";
                memcpy(&v, base + off, 4);
                if (v != v) continue;                  /* NaN */
                if (v == 0.0f) continue;
                if (v > znear * 0.2f && v < zfar * 4.0f) tag = "  <- distance-like";
                else if (v >= 4.0f && v <= 256.0f && v == (float)(int)v)
                    tag = "  <- whole number, tile-count-like";
                if (!*tag) continue;
                fprintf(f, "  %+6ld  %14.4f%s\n", off, (double)v, tag);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fprintf(f, "  (read fault)\n");
        }
        fprintf(f, "\n");
    }
    fclose(f);
    kv_log("camera-world dump written (hunting the cull radius)");
}

/* Dump memory around the pointer the engine hands OpenGL for its VIEW matrix.
 *
 * Everything so far has searched for the camera by value, across the whole
 * address space, and found nothing that mattered.  But the engine tells us
 * where its camera lives every frame without being asked: glLoadMatrixf is
 * called with a POINTER to the view matrix, and that memory belongs to the
 * camera object.  Whatever else that object holds -- its position, its
 * orientation as separate fields, the size of the region it decides to draw --
 * is within a few dozen bytes of it.
 *
 * Annotated with everything already known, so the structure can be read rather
 * than guessed at.
 */
/* close_to lives in fovpatch.c and is private to it; a local copy is
   simpler than exporting it and cannot drift from what that file means
   by "close". */
static int vs_close(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 1e-3f;
}

void dump_view_neighbourhood(const void *view_matrix, const float *vm,
                             float fovy, float znear, float zfar) {
    static int done;
    FILE *f;
    const unsigned char *base = (const unsigned char *)view_matrix;
    long off;
    float right[3], up[3], back[3], pos[3];

    if (done || !view_matrix || !vm) return;
    /* Only the WORLD camera.  The menu backdrop has a near plane of 10 and a
       camera at the origin, and dumping that tagged every zero in memory as
       "camera X" -- pages of it, meaning nothing. */
    {
        static int said;
        if (!said) {
            said = 1;
            kv_log("VIEW: first look -- near %.1f far %.1f fovy %.1f", znear,
                   zfar, fovy);
        }
    }
    if (znear < 10.5f) return;

    right[0] = vm[0]; right[1] = vm[4]; right[2] = vm[8];
    up[0]    = vm[1]; up[1]    = vm[5]; up[2]    = vm[9];
    back[0]  = vm[2]; back[1]  = vm[6]; back[2]  = vm[10];
    /* Camera position in world space: -(R^T * t). */
    pos[0] = -(vm[12] * vm[0] + vm[13] * vm[1] + vm[14] * vm[2]);
    pos[1] = -(vm[12] * vm[4] + vm[13] * vm[5] + vm[14] * vm[6]);
    pos[2] = -(vm[12] * vm[8] + vm[13] * vm[9] + vm[14] * vm[10]);

    /* A camera at the origin is the menu's, not the game's. */
    /* No check on the camera position.  The engine installs its rotation and
       its translation as separate multiplies, so the first matrix of the pass
       has no translation in it yet and the position reads as the origin --
       which is not a sign of the wrong camera, and rejecting it meant the dump
       never ran at all.  The near plane already identifies the right pass. */
    done = 1;

    f = open_out("view-struct");
    if (!f) return;
    fprintf(f, "Memory around the VIEW matrix the engine hands OpenGL.\n"
               "This is the camera's own storage, so what decides which ground\n"
               "tiles get drawn is very likely a few bytes from here.\n\n");
    fprintf(f, "view basis right (%.4f %.4f %.4f)\n"
               "           up    (%.4f %.4f %.4f)\n"
               "           back  (%.4f %.4f %.4f)\n"
               "camera position  (%.2f %.2f %.2f)\n"
               "fovy %.2f  near %.1f  far %.1f\n\n",
            right[0], right[1], right[2], up[0], up[1], up[2],
            back[0], back[1], back[2], pos[0], pos[1], pos[2], fovy,
            znear, zfar);

    /* Print the STRUCTURE, not a filtered guess at it.  The previous version
       printed only lines it could tag, and tagged against a camera position of
       zero -- so every near-zero float in two kilobytes came out as "camera X"
       and the real fields were invisible.  A readable window of raw values is
       worth more than a clever filter built on a wrong assumption. */
    fprintf(f, "offset      float            int      notes\n");
    __try {
        for (off = -256; off <= 512; off += 4) {
            float v;
            int iv;
            const char *tag = "";
            memcpy(&v, base + off, 4);
            memcpy(&iv, base + off, 4);
            if (v > -1e-4f && v < 1e-4f) v = 0.0f;   /* denormal noise */
            if (vs_close(v, right[0])) tag = "right.x";
            else if (vs_close(v, right[1])) tag = "right.y";
            else if (vs_close(v, up[1])) tag = "up.y";
            else if (vs_close(v, up[2])) tag = "up.z";
            else if (vs_close(v, back[1])) tag = "back.y";
            else if (vs_close(v, back[2])) tag = "back.z";
            else if (vs_close(v, fovy)) tag = "FOVY";
            else if (vs_close(v, znear)) tag = "NEAR";
            else if (vs_close(v, zfar)) tag = "FAR";
            else if (v != 0.0f && v == (float)(int)v &&
                     v >= 4.0f && v <= 512.0f) tag = "whole number";
            else if (iv >= 4 && iv <= 512 && v == 0.0f) tag = "small int";
            fprintf(f, "  %+5ld  %14.5f  %11d  %s\n", off, (double)v, iv, tag);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(f, "  (read fault)\n");
    }
    fclose(f);
    kv_log("VIEW: dumped the camera structure around %p", view_matrix);
}

/* Dump every heap camera object, annotated against the view basis in use right
   now.  The previous version of this ran at a different moment from the scan
   that found the vectors, so it compared against a basis the camera no longer
   had and tagged nothing. */
void dump_heap_cameras(const float *vm) {
    static int done;
    FILE *f;
    unsigned c, n;
    float right[3], up[3], back[3];

    if (done || !vm) return;
    /* Do not mark this done before the camera objects have been found.  The
       view is installed earlier in the frame than the scan that locates them,
       so the first call has nothing to dump -- and latching on that call meant
       the file came out empty and was never written again. */
    if (fov_candidate_count() == 0) return;
    done = 1;
    right[0] = vm[0]; right[1] = vm[4]; right[2] = vm[8];
    up[0]    = vm[1]; up[1]    = vm[5]; up[2]    = vm[9];
    back[0]  = vm[2]; back[1]  = vm[6]; back[2]  = vm[10];

    f = open_out("heap-cameras");
    if (!f) return;
    n = fov_candidate_count();
    fprintf(f, "Heap camera objects, annotated against the basis in use now.\n");
    fprintf(f, "right (%.5f %.5f %.5f)\nup    (%.5f %.5f %.5f)\nback  (%.5f %.5f %.5f)\n\n",
            right[0], right[1], right[2], up[0], up[1], up[2],
            back[0], back[1], back[2]);

    for (c = 0; c < n; c++) {
        unsigned char *base = (unsigned char *)fov_candidate(c);
        long off;
        int found = 0;
        if (!base) continue;
        fprintf(f, "==== camera %u at %p (offset 0 is the field of view) ====\n",
                c + 1, (void *)base);
        __try {
            for (off = -256; off <= 256; off += 4) {
                float v[3];
                const char *tag = NULL;
                memcpy(v, base + off, 12);
                if (v[0] != v[0] || v[1] != v[1] || v[2] != v[2]) continue;
                if (vs_close(v[0], right[0]) && vs_close(v[1], right[1]) &&
                    vs_close(v[2], right[2])) tag = "RIGHT";
                else if (vs_close(v[0], up[0]) && vs_close(v[1], up[1]) &&
                         vs_close(v[2], up[2])) tag = "UP";
                else if (vs_close(v[0], back[0]) && vs_close(v[1], back[1]) &&
                         vs_close(v[2], back[2])) tag = "BACK";
                else if (vs_close(v[0], -back[0]) && vs_close(v[1], -back[1]) &&
                         vs_close(v[2], -back[2])) tag = "FORWARD";
                if (!tag) continue;
                fprintf(f, "  %+5ld  (%9.5f %9.5f %9.5f)  %s\n", off,
                        (double)v[0], (double)v[1], (double)v[2], tag);
                found++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fprintf(f, "  (read fault)\n");
        }
        if (!found) fprintf(f, "  no basis vector within 256 bytes\n");
        fprintf(f, "\n");
    }
    fclose(f);
    kv_log("HEAPCAM: dumped %u camera objects", n);
}
