/* A Kingdom for Keflings VR -- widen the field of view the ENGINE is willing to draw.
 *
 * Why this file exists.  The engine culls the world against its own camera,
 * not against the matrix it hands to OpenGL: replacing the GL projection with
 * a wider one was measured and produced holes rather than more world.  Its
 * vertical field of view is a hardcoded 45 degrees -- the recovered ini
 * vocabulary has no key for it -- so the only way to widen the view is to
 * change that number in the running process.
 *
 * How it is found.  glMultMatrixd is called with a pointer to the projection
 * matrix the engine just built, and a scan around that pointer found
 *
 *     +184  float 45.0          fovy in degrees
 *     +188  float 1.7777778     aspect
 *
 * adjacent, which is a camera parameter block.  That copy is on the stack and
 * therefore transient, but the ADJACENCY is a signature: a float of exactly
 * 45 immediately followed by a float equal to the window's aspect is not a
 * coincidence anywhere else in a 500 MB process.  So search the whole address
 * space for that pair and patch the copies that live in real storage.
 *
 * How it is verified WITHOUT a headset.  If the patch takes, the engine's own
 * projection changes, and the proxy already reads that matrix back and logs
 * its half-angle tangents.  tany moving off 0.4142 is proof the engine
 * accepted a different camera; it needs nobody to put a headset on.
 */
#include "proxy.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAX_CAND 64

static void  *g_cand[MAX_CAND];
static unsigned g_ncand;
static void  *g_farcand[MAX_CAND];
static unsigned g_nfarcand;
static int    g_far_scanned;
static char   g_far_is_double[MAX_CAND];
static float  g_far_applied;
static int    g_scanned;
static int    g_stop;
static float  g_applied;

/* The stack holds a transient copy of the camera block.  Patching it is
   useless -- it is rebuilt every frame -- and writing to a live stack frame is
   a good way to corrupt a return address.  Anything close to our own locals is
   the stack. */
static int on_our_stack(const void *p) {
    char here;
    const char *a = (const char *)p;
    const char *b = &here;
    ptrdiff_t d = (a > b) ? (a - b) : (b - a);
    return d < (2 << 20);
}

/* Where does an address live?  A float 45.0 beside a float 1.7778 inside some
   unrelated DLL is a coincidence, and writing into another module's data is a
   way to break something that has nothing to do with this game.  Patch only
   the game's own image and its heap. */
enum { OWN_IMAGE, OTHER_MODULE, PRIVATE_MEM };

static int classify(const MEMORY_BASIC_INFORMATION *mbi, const char **name) {
    static const unsigned char *exe_base;
    if (!exe_base) exe_base = (const unsigned char *)GetModuleHandleA(NULL);
    if (mbi->Type == MEM_IMAGE) {
        if ((const unsigned char *)mbi->AllocationBase == exe_base) {
            *name = "game image";
            return OWN_IMAGE;
        }
        *name = "ANOTHER MODULE";
        return OTHER_MODULE;
    }
    *name = "heap";
    return PRIVATE_MEM;
}

static int writable(DWORD protect) {
    return (protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static int close_to(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 1e-4f;
}

/* A RELATIVE comparison, for values that have been round-tripped through a
   projection matrix.  The far plane is recovered as m14/(m10+1), which for a
   stored 2200 comes back as something like 2200.0012 -- and an absolute
   tolerance of 1e-4 then rejects the very value it is looking for.  That is
   why the first draw-distance search found nothing. */
static int close_rel(double a, double b) {
    double d = a - b, m = (b < 0) ? -b : b;
    if (d < 0) d = -d;
    return d <= m * 5e-4 + 1e-4;
}

/* Find every plausible copy of the camera's vertical field of view.
 *
 * The first attempt required a float 45.0 IMMEDIATELY followed by the aspect.
 * That found exactly one address, in the game's static image, and patching it
 * changed nothing measurable -- the engine almost certainly copies that block
 * into an active camera each frame and culls against the copy, so the template
 * is written after the copy has already been taken.
 *
 * So: accept a float 45.0 anywhere in the game's own memory that has a
 * CORROBORATING camera value nearby -- the aspect, the tangent or cotangent of
 * the half angle, or the near/far planes.  Wider than before, still far too
 * specific to hit by chance, and every hit gets patched so the question
 * becomes one measurement rather than a series of guesses.
 */
static int corroborated(const unsigned char *base, size_t size, size_t at,
                        float aspect) {
    long lo = (long)at - 64, hi = (long)at + 64;
    long i;
    if (lo < 0) lo = 0;
    if (hi > (long)size - 4) hi = (long)size - 4;
    for (i = lo; i <= hi; i += 4) {
        float v;
        if (i == (long)at) continue;
        memcpy(&v, base + i, 4);
        if (close_to(v, aspect)) return 1;          /* the aspect */
        if (close_to(v, 0.41421356f)) return 1;     /* tan(half fovy) */
        if (close_to(v, 2.41421356f)) return 1;     /* cot(half fovy) */
        if (close_to(v, 70.0f)) return 1;           /* near plane */
        if (close_to(v, 2200.0f)) return 1;         /* far plane */
    }
    return 0;
}

void fov_scan(float fovy, float aspect) {
    SYSTEM_INFO si;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr;
    unsigned image_hits = 0, heap_hits = 0;

    /* Re-scan if the last attempt found nothing.  The first one runs while
       the game is in the menu, whose camera is not the one that draws the
       world, so a single locked-out attempt means never finding it. */
    if (g_scanned && g_ncand) return;
    g_scanned = 1;
    g_ncand = 0;
    g_ncand = 0;

    GetSystemInfo(&si);
    addr = (unsigned char *)si.lpMinimumApplicationAddress;

    while (addr < (unsigned char *)si.lpMaximumApplicationAddress &&
           g_ncand < MAX_CAND) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;

        if (mbi.State == MEM_COMMIT && mbi.RegionSize &&
            mbi.Type != MEM_MAPPED &&
            mbi.RegionSize < (64u << 20) &&
            !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS) &&
            writable(mbi.Protect)) {
            __try {
                unsigned char *base = (unsigned char *)mbi.BaseAddress;
                size_t size = mbi.RegionSize;
                size_t i;
                const char *where = "?";
                if (classify(&mbi, &where) == OTHER_MODULE) goto next;
                for (i = 0; i + 4 <= size && g_ncand < MAX_CAND; i += 4) {
                    float a;
                    memcpy(&a, base + i, 4);
                    if (!close_to(a, fovy)) continue;
                    if (on_our_stack(base + i)) continue;
                    if (!corroborated(base, size, i, aspect)) continue;
                    g_cand[g_ncand++] = base + i;
                    if (classify(&mbi, &where) == OWN_IMAGE) image_hits++;
                    else heap_hits++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                /* region is not really readable; skip it */
            }
        }
next:
        if (mbi.RegionSize == 0) break;
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }

    kv_log("FOV: %u candidate copies of the camera field of view "
           "(%u in the game image, %u on the heap)", g_ncand, image_hits,
           heap_hits);
    if (!g_ncand)
        kv_log("FOV: none found. The vertical field of view is not stored as a "
               "plain float anywhere the engine can write, so it is computed "
               "in code and this route is closed.");
}

/* Write the override over every candidate whose value is still one we put
   there, or the original.  Re-applied every frame because the engine may
   recompute it; cheap, and it means the patch survives a menu or a load. */
/* The SECOND field of view.
 *
 * Widening the camera made the terrain and the distant hills draw further, but
 * the objects that sit on the ground -- trees, rocks, sheep, crystals -- kept
 * appearing and disappearing inside a lopsided region offset away from the
 * player.  Bisecting every value in the camera structures against the draw
 * count found why: one more float of 45.0 that the field-of-view scan never
 * treated as a candidate, because it has no aspect ratio stored beside it.
 * Scaling that single value alone draws 147% more geometry.
 *
 * So the engine keeps two copies of its field of view and culls different
 * things with each.  Patching only the one with an aspect beside it left the
 * ground objects clipped to the original 45 degree cone -- which, from a
 * camera tilted down, lands on the ground as exactly the off-centre circle
 * that was reported.
 *
 * These are patched separately rather than by loosening the original search,
 * because the corroboration rule is what keeps that search honest: a bare 45.0
 * appears all over a process, and only the ones inside a known camera block
 * are safe to write to.
 */
void fov_apply_second(float want, float original) {
    unsigned c, n = 0, seen = 0;
    static int said;
    if (!g_ncand || want <= 0.0f) {
        /* A SEPARATE flag.  Setting the main one here would latch during the
           menu, before any camera block exists, and then silence the real
           answer for the rest of the session -- which is how three other
           instruments in this port managed to report the wrong thing. */
        static int said_notrun;
        if (!said_notrun) {
            said_notrun = 1;
            kv_log("FOV2: not run yet (%u camera blocks, want %.1f)", g_ncand,
                   (double)want);
        }
        return;
    }
    for (c = 0; c < g_ncand; c++) {
        /* g_cand[] points AT the field-of-view float itself, not at the start
           of a block -- so the window is around that value, and the value at
           offset 0 is the one fov_apply already owns. */
        unsigned char *centre = (unsigned char *)g_cand[c];
        long off;
        if (!centre) continue;
        __try {
            for (off = -1024; off <= 1024; off += 4) {
                float *p;
                if (off == 0) continue;              /* fov_apply owns this one */
                p = (float *)(centre + off);
                /* close_to, not ==.  An exact float comparison is the same
                   mistake that hid the far plane for a whole evening: the
                   value reads 45.000 but need not be exactly 45.0f. */
                if (!close_to(*p, original)) continue;
                seen++;
                *p = want;
                n++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (n) {
        if (!said) {
            said = 1;
            (void)seen;
            kv_log("FOV2: widened %u further copies of %.1f -- these are what "
                   "the ground objects are culled against, which is why they "
                   "were clipped to a lopsided circle while the terrain was "
                   "not.", n, (double)original);
        }
    } else if (!said) {
        said = 1;
        kv_log("FOV2: found no other copy of %.1f near the camera blocks, so "
               "the second field of view is somewhere else and the ground "
               "objects are still clipped.", (double)original);
    }
}

/* The camera's ASPECT, beside the field of view it is multiplied by.
 *
 * The engine builds its cull frustum from fovy and this, so widening fovy
 * for a headset also widens the horizontal by the same ratio -- to 141.7
 * degrees when 116 would cover the eye. These are the same camera blocks
 * fov_scan already vouched for; only the offset within them differs. */
#define MAX_ACAND 128
static void *g_acand[MAX_ACAND];
static unsigned g_nacand;
static int   g_ascanned;
static float g_a_applied;

void aspect_scan(float aspect) {
    unsigned i;
    if (g_ascanned && g_nacand) return;
    if (!g_ncand) return;               /* no cameras yet; try again later */
    g_ascanned = 1;
    g_nacand = 0;
    for (i = 0; i < g_ncand && g_nacand < MAX_ACAND; i++) {
        unsigned char *p = (unsigned char *)g_cand[i];
        long k;
        if (!p) continue;
        __try {
            for (k = -64; k <= 64 && g_nacand < MAX_ACAND; k += 4) {
                float v;
                unsigned j;
                int dup = 0;
                if (k == 0) continue;
                memcpy(&v, p + k, 4);
                if (!close_to(v, aspect)) continue;
                for (j = 0; j < g_nacand; j++)
                    if (g_acand[j] == (void *)(p + k)) { dup = 1; break; }
                if (!dup) g_acand[g_nacand++] = p + k;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    kv_log("ASPECT: %u copies of the camera aspect found beside the %u field "
           "of view copies. Narrowing it makes the engine cull horizontally "
           "to what an eye needs instead of to its own 16:9 shape.",
           g_nacand, g_ncand);
    if (!g_nacand)
        kv_log("ASPECT: none found. The horizontal extent is not stored as a "
               "plain float beside the camera, so this route is closed.");
}

void aspect_apply(float want, float original) {
    unsigned i;
    if (!g_nacand || want <= 0.0f) return;
    for (i = 0; i < g_nacand; i++) {
        __try {
            float cur;
            if (!g_acand[i]) continue;
            memcpy(&cur, g_acand[i], 4);
            if (close_to(cur, want)) continue;
            if (!close_to(cur, original) && !close_to(cur, g_a_applied)) continue;
            memcpy(g_acand[i], &want, 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_acand[i] = NULL; }
    }
    if (g_a_applied < want - 0.01f || g_a_applied > want + 0.01f) {
        kv_log("ASPECT: engine camera narrowed to aspect %.3f across %u "
               "copies (was %.3f). The picture is unaffected -- each eye is "
               "drawn with its own projection, not this one.",
               want, g_nacand, original);
        g_a_applied = want;
    }
}

void fov_apply(float want, float original) {
    unsigned i;
    if (!g_ncand || want <= 0.0f) return;
    for (i = 0; i < g_ncand; i++) {
        __try {
            float cur;
            memcpy(&cur, g_cand[i], 4);
            if (close_to(cur, want)) continue;               /* already set */
            if (!close_to(cur, original) && !close_to(cur, g_applied)) continue;
            memcpy(g_cand[i], &want, 4);
            memcpy(&cur, g_cand[i], 4);
            if (!close_to(cur, want))
                kv_log("FOV: write to %p did not stick (read back %.3f)",
                       g_cand[i], cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_cand[i] = NULL;
        }
    }
    /* The width now follows the head, so it changes every frame; only say so
       when it has moved enough to be worth a line. */
    if (g_applied < want - 2.0f || g_applied > want + 2.0f) {
        kv_log("FOV: engine camera widened to %.1f degrees across %u copies "
               "(was %.1f). This is what stops the engine culling the world "
               "you can see.", want, g_ncand, original);
        g_applied = want;
    }
}

unsigned fov_candidate_count(void) { return g_ncand; }
/* camrot.c looks for the orientation near these same blocks. */
void *fov_candidate(unsigned i) {
    return (i < g_ncand) ? g_cand[i] : NULL;
}

/* Does our write survive?  Two cases look identical in the projection and need
   opposite fixes: the engine reading this value and ignoring it (so it is not
   the source), or the engine writing 45 back over us every frame (so it IS a
   copy and the real source is upstream). */
void fov_report(void) {
    unsigned i;
    for (i = 0; i < g_ncand; i++) {
        float cur = -1.0f;
        if (!g_cand[i]) continue;
        __try { memcpy(&cur, g_cand[i], 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        kv_log("  FOV candidate %u at %p now reads %.2f %s", i + 1, g_cand[i],
               cur,
               close_to(cur, g_applied) ? "(our value survived: the engine "
                                          "is not reading it)"
                                        : "(overwritten by the engine: this is "
                                          "a copy, the source is upstream)");
    }
}

/* Where in the engine's code is the projection actually set?  The return
   address of the glMultMatrixd call names the caller, which says whether the
   engine builds the matrix itself or goes through glu32's gluPerspective. */
void fov_note_caller(void *ret) {
    static int said;
    MEMORY_BASIC_INFORMATION mbi;
    char name[MAX_PATH];
    if (said || !ret) return;
    said = 1;
    name[0] = 0;
    if (VirtualQuery(ret, &mbi, sizeof(mbi)) && mbi.AllocationBase)
        GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, MAX_PATH);
    kv_log("FOV: the projection is set from %p, module base %p (%s)",
           ret, mbi.AllocationBase, name[0] ? name : "unknown");
    kv_log("FOV: offset into that module = +0x%X",
           (unsigned)((unsigned char *)ret - (unsigned char *)mbi.AllocationBase));
}


/* ---------------------------------------------------------------------------
 * The draw distance.
 *
 * Widening the camera stopped the world being cut off at the EDGES of view,
 * but not in the DISTANCE: "it stops drawing objects, and as soon as I move in
 * that direction it'll draw them" is a limit that travels with the camera.
 * The engine's projection declares near 70 and far 2200, and the far plane is
 * exactly that shape of limit.
 *
 * Same method as the field of view: find the value in the engine's own memory
 * with a corroborating neighbour, and rewrite it every frame after the engine
 * has finished writing it.  The port's own frustum follows automatically,
 * because it reads near and far back out of the engine's projection.
 * ------------------------------------------------------------------------ */
/* Search INSIDE the camera blocks we already found, rather than the whole
 * address space.  A blind hunt for a float 2200 beside a float 70 found
 * nothing -- the two are evidently not adjacent, or not both floats, and
 * gluPerspective takes DOUBLES so the engine may well keep them that way.
 * But the field of view patch works, so we know exactly where the cameras
 * live; the draw distance is a member of the same structure.
 */
void far_scan(float far_value, float near_value) {
    unsigned c;
    if (g_far_scanned) return;
    g_far_scanned = 1;
    g_nfarcand = 0;
    /* No camera blocks is not a reason to stop: the whole-memory search below
       does not need them.  Returning here is what made the last run report
       nothing at all. */
    if (!g_ncand)
        kv_log("FAR: no camera blocks known; going straight to the whole-memory "
               "search");
    for (c = 0; c < g_ncand && g_nfarcand < MAX_CAND; c++) {
        unsigned char *base = (unsigned char *)g_cand[c];
        long off;
        if (!base) continue;
        __try {
            /* Widened from 512: near sits 4 bytes from the field of view, so
               the struct is compact, but far is evidently not inside it. */
            for (off = -4096; off <= 4096 && g_nfarcand < MAX_CAND; off += 4) {
                unsigned char *p = base + off;
                float f;
                double d;
                memcpy(&f, p, 4);
                if (close_rel((double)f, (double)far_value)) {
                    g_farcand[g_nfarcand++] = p;
                    kv_log("FAR: float  %.0f at camera %u %+ld", (double)f, c + 1, off);
                    continue;
                }
                if (((ULONG_PTR)p & 7) == 0) {
                    memcpy(&d, p, 8);
                    if (close_rel(d, (double)far_value)) {
                        g_farcand[g_nfarcand++] = p;
                        g_far_is_double[g_nfarcand - 1] = 1;
                        kv_log("FAR: double %.1f at camera %u %+ld", d, c + 1, off);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    kv_log("FAR: %u copies of the draw distance found inside %u camera blocks "
           "(looked for %.4f within 0.05%%, near plane %.2f)",
           g_nfarcand, g_ncand, (double)far_value, (double)near_value);

    /* Nothing near the camera blocks.  That was true last time too, and the
       search stopped there -- which only ever proved the far plane is not
       stored beside the field of view, not that it is not stored.

       So search the whole address space, the way the field of view itself was
       found: a value on its own appears thousands of times and means nothing,
       but a far plane with its OWN near plane a few bytes away is a camera.
       That pairing is what made the field of view search work, and it is the
       only part of it worth reusing. */
    if (!g_nfarcand) {
        SYSTEM_INFO si;
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *addr;
        unsigned checked = 0;

        kv_log("FAR: not beside the camera. Searching the whole address space "
               "for a far plane of %.1f with a near plane of %.1f within 64 "
               "bytes of it.", (double)far_value, (double)near_value);

        GetSystemInfo(&si);
        addr = (unsigned char *)si.lpMinimumApplicationAddress;
        while (addr < (unsigned char *)si.lpMaximumApplicationAddress &&
               g_nfarcand < MAX_CAND) {
            if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_COMMIT && mbi.RegionSize &&
                mbi.RegionSize < (64u << 20) && writable(mbi.Protect) &&
                !(mbi.Protect & PAGE_GUARD) && !(mbi.Protect & PAGE_NOACCESS)) {
                __try {
                    unsigned char *base = (unsigned char *)mbi.BaseAddress;
                    size_t size = mbi.RegionSize, i;
                    for (i = 0; i + 4 <= size && g_nfarcand < MAX_CAND; i += 4) {
                        float f;
                        long j;
                        int near_found = 0;
                        memcpy(&f, base + i, 4);
                        if (!close_rel((double)f, (double)far_value)) continue;
                        if (on_our_stack(base + i)) continue;
                        checked++;
                        for (j = -64; j <= 64 && !near_found; j += 4) {
                            float n;
                            if ((long)i + j < 0 ||
                                (size_t)((long)i + j) + 4 > size) continue;
                            memcpy(&n, base + i + j, 4);
                            if (close_rel((double)n, (double)near_value))
                                near_found = 1;
                        }
                        if (!near_found) continue;
                        g_farcand[g_nfarcand] = base + i;
                        g_far_is_double[g_nfarcand] = 0;
                        g_nfarcand++;
                        kv_log("FAR: far %.1f with its near plane beside it, at %p",
                               (double)f, (void *)(base + i));
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
            if (mbi.RegionSize == 0) break;
            addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        }
        kv_log("FAR: %u corroborated copies out of %u bare matches. %s",
               g_nfarcand, checked,
               g_nfarcand ? "These are camera structures."
                          : "Not one of them had a near plane beside it, so "
                            "the draw distance is not a stored far plane at "
                            "all -- it is a radius the engine tests against, "
                            "and extending a frustum will never reach it.");
    }
}

void far_apply(float want, float original) {
    unsigned i;
    if (!g_nfarcand || want <= 0.0f) return;
    for (i = 0; i < g_nfarcand; i++) {
        __try {
            float cur;
            if (!g_farcand[i]) continue;
            if (g_far_is_double[i]) {
                double dcur, dwant = (double)want;
                memcpy(&dcur, g_farcand[i], 8);
                if (close_rel(dcur, dwant)) continue;
                memcpy(g_farcand[i], &dwant, 8);
                continue;
            }
            memcpy(&cur, g_farcand[i], 4);
            if (close_rel(cur, want)) continue;
            if (!close_rel(cur, original) && !close_rel(cur, g_far_applied))
                continue;
            memcpy(g_farcand[i], &want, 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_farcand[i] = NULL; }
    }
    if (g_far_applied != want) {
        kv_log("FAR: draw distance widened to %.0f across %u copies (was %.0f)",
               want, g_nfarcand, original);
        g_far_applied = want;
    }
}


/* ---------------------------------------------------------------------------
 * Where does the engine keep its camera ORIENTATION?
 *
 * Widening the field of view works but is a blunt instrument: at a 76 degree
 * head turn it needs a frustum approaching 200 degrees, which cannot be built,
 * and long before that the engine is drawing a cone it cannot afford.  The
 * answer is to turn the engine's camera with the head instead -- then its
 * frustum points where you look, the culling is right at any angle, and it
 * only ever has to cover the headset's own field of view.
 *
 * We have the view matrix GL was given, and the addresses of the camera blocks
 * that hold the field of view.  The orientation must be in there in one of a
 * few forms, so look for each: the 3x3 basis in either row or column order, a
 * forward direction vector, or a quaternion.
 * ------------------------------------------------------------------------ */
static int triple_near(const float *a, const float *b) {
    int i;
    for (i = 0; i < 3; i++) {
        float d = a[i] - b[i];
        if (d < 0) d = -d;
        if (d > 1e-3f) return 0;
    }
    return 1;
}

void orientation_scan(const float *vm) {
    unsigned c;
    unsigned found = 0;
    /* The view matrix is column-major; its upper 3x3 rows are the camera's
       right, up and back vectors in world space. */
    float right[3] = { vm[0], vm[4], vm[8] };
    float up[3]    = { vm[1], vm[5], vm[9] };
    float back[3]  = { vm[2], vm[6], vm[10] };
    float fwd[3]   = { -vm[2], -vm[6], -vm[10] };

    kv_log("ORI: engine view basis right=(%.4f %.4f %.4f) up=(%.4f %.4f %.4f) "
           "forward=(%.4f %.4f %.4f)", right[0], right[1], right[2],
           up[0], up[1], up[2], fwd[0], fwd[1], fwd[2]);

    if (!g_ncand) { kv_log("ORI: no camera blocks known"); return; }

    for (c = 0; c < g_ncand; c++) {
        unsigned char *base = (unsigned char *)g_cand[c];
        long off;
        if (!base) continue;
        __try {
            for (off = -1024; off <= 1024; off += 4) {
                float v[3];
                memcpy(v, base + off, 12);
                if (triple_near(v, right)) {
                    kv_log("ORI: camera %u %+ld  RIGHT vector", c + 1, off); found++;
                } else if (triple_near(v, up)) {
                    kv_log("ORI: camera %u %+ld  UP vector", c + 1, off); found++;
                } else if (triple_near(v, back)) {
                    kv_log("ORI: camera %u %+ld  BACK vector", c + 1, off); found++;
                } else if (triple_near(v, fwd)) {
                    kv_log("ORI: camera %u %+ld  FORWARD vector", c + 1, off); found++;
                }
                if (found > 40) break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        if (found > 40) break;
    }
    kv_log("ORI: %u orientation vectors found near the camera blocks.%s", found,
           found ? "  The engine's camera can be turned." :
                   "  Not stored as a plain basis; it may be a quaternion, or "
                   "held somewhere else entirely.");
}


/* ---------------------------------------------------------------------------
 * Print the camera structure, annotated.
 *
 * The orientation scan proved the basis vectors are in there, but not how the
 * structure is laid out -- and writing a rotation into a camera you have only
 * inferred is a good way to corrupt it.  So dump the floats around each camera
 * block with every value we can already name marked, and read the layout off
 * the page instead of guessing at it.
 * ------------------------------------------------------------------------ */
void camera_layout_dump(const float *vm, float fovy, float aspect,
                        float znear, float zfar) {
    static int done;
    FILE *f;
    unsigned c;
    float right[3] = { vm[0], vm[4], vm[8] };
    float up[3]    = { vm[1], vm[5], vm[9] };
    float back[3]  = { vm[2], vm[6], vm[10] };

    if (done || !g_ncand) return;
    done = 1;
    f = open_out("camera-layout");
    if (!f) return;

    fprintf(f, "Camera structures, annotated.  The aim is to find the\n"
               "orientation so the engine can be made to look where the head\n"
               "looks -- widening the field of view only buys about 17 degrees\n"
               "of head turn before a frustum becomes unbuildable.\n\n");
    fprintf(f, "view basis: right (%.4f %.4f %.4f)\n"
               "            up    (%.4f %.4f %.4f)\n"
               "            back  (%.4f %.4f %.4f)\n",
            right[0], right[1], right[2], up[0], up[1], up[2],
            back[0], back[1], back[2]);
    fprintf(f, "fovy %.3f  aspect %.5f  near %.2f  far %.2f\n\n",
            fovy, aspect, znear, zfar);

    for (c = 0; c < g_ncand && c < 4; c++) {
        unsigned char *base = (unsigned char *)g_cand[c];
        long off;
        if (!base) continue;
        fprintf(f, "==== camera block %u at %p ====\n", c + 1, (void *)base);
        __try {
            for (off = -256; off <= 256; off += 4) {
                float v;
                const char *tag = "";
                memcpy(&v, base + off, 4);
                if (close_to(v, fovy)) tag = "  <- fovy (ours)";
                else if (close_to(v, 45.0f)) tag = "  <- 45, the shipped fovy";
                else if (close_to(v, aspect)) tag = "  <- aspect";
                else if (close_to(v, znear)) tag = "  <- near";
                else if (close_to(v, zfar)) tag = "  <- far";
                else if (close_to(v, right[0])) tag = "  <- right.x";
                else if (close_to(v, right[1])) tag = "  <- right.y";
                else if (close_to(v, up[0])) tag = "  <- up.x";
                else if (close_to(v, up[1])) tag = "  <- up.y";
                else if (close_to(v, up[2])) tag = "  <- up.z";
                else if (close_to(v, back[0])) tag = "  <- back.x";
                else if (close_to(v, back[1])) tag = "  <- back.y";
                else if (close_to(v, back[2])) tag = "  <- back.z";
                fprintf(f, "  %+5ld  %14.6f%s\n", off, (double)v, tag);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fprintf(f, "  (read fault)\n");
        }
        fprintf(f, "\n");
    }
    fclose(f);
    kv_log("camera layout dump written");
}
