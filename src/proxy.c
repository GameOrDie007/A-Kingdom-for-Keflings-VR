/* A Kingdom for Keflings VR -- the opengl32.dll proxy.
 *
 * Drops in beside "A Kingdom for Keflings.exe".  Forwards every GL/WGL entry
 * point to the real system DLL, and when stereo is on, issues each scene draw
 * once per eye with that eye's projection.
 *
 * What the instrumentation established about this engine, and why the stereo
 * path is shaped the way it is:
 *
 *   - Per frame: 3 perspective projections built with glMultMatrixd, 1 glOrtho,
 *     no glLoadMatrixf and no glFrustum at all.
 *   - It uses ARB shader objects but never asks for glUniformMatrix4fv, so even
 *     the GLSL draws read gl_ModelViewProjectionMatrix.  The fixed-function
 *     matrix stack is therefore the ENTIRE camera, for every draw in the game.
 *   - ~220 immediate-mode glBegin blocks and ~85 glDrawElements per frame, and
 *     zero glDrawArrays.  Everything renders to the default framebuffer.
 *
 * The game's frame loop is inside an encrypted executable and cannot be run
 * twice, so stereo is done by re-issuing each draw.  glDrawElements re-issues
 * directly, since client array state has not changed.  Immediate mode is
 * captured into a display list -- only vertex attribute commands are legal
 * between glBegin and glEnd, so the list is a complete and faithful recording,
 * and it avoids hooking glVertex/glColor/glTexCoord/glNormal individually.
 */
#include "proxy.h"
#include "vr.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>   /* atoi, malloc: the settings.ini rewrite */
#include <math.h>
#include <intrin.h>

#define GL_FRAMEBUFFER_BINDING_EXT 0x8CA6
#define GL_COMPILE                 0x1300

static HMODULE g_realdll;
HMODULE g_self;

void *proxy_real_proc(const char *name) {
    return g_realdll ? (void *)GetProcAddress(g_realdll, name) : NULL;
}

static FILE *g_log;
static FILE *g_log2;   /* the copy beside the game */
static CRITICAL_SECTION g_lock;

/* ---- originals -------------------------------------------------------- */
static BOOL  (WINAPI *o_wglSwapBuffers)(HDC);
static BOOL  (WINAPI *o_wglSwapLayerBuffers)(HDC, UINT);
static HGLRC (WINAPI *o_wglCreateContext)(HDC);
static BOOL  (WINAPI *o_wglMakeCurrent)(HDC, HGLRC);
static BOOL  (WINAPI *o_wglDeleteContext)(HGLRC);
static PROC  (WINAPI *o_wglGetProcAddress)(LPCSTR);
static BOOL  (WINAPI *o_wglSwapIntervalEXT)(int);
static void  (APIENTRY *o_glDrawBuffer)(GLenum);
static void  (APIENTRY *o_glReadBuffer)(GLenum);
/* the engine's own framebuffer binder, if it ever asks for one */
static void  (APIENTRY *o_glBindFramebufferEng)(GLenum, GLuint);
static void  (APIENTRY *o_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void  (APIENTRY *o_glScissor)(GLint, GLint, GLsizei, GLsizei);
static void  (APIENTRY *o_glPushMatrix)(void);
static void  (APIENTRY *o_glPopMatrix)(void);
static void  (APIENTRY *o_glFinish)(void);
static void  (APIENTRY *o_glTexEnvi)(GLenum, GLenum, GLint);
static void  (APIENTRY *o_glTexEnvf)(GLenum, GLenum, GLfloat);
static void  (APIENTRY *o_glAlphaFunc)(GLenum, GLclampf);
static void  (APIENTRY *o_glDepthMask)(GLboolean);
static void  (APIENTRY *o_glArrayElement)(GLint);
static void  (APIENTRY *o_glTranslatef)(GLfloat, GLfloat, GLfloat);
static GLenum (APIENTRY *o_glGetError)(void);
static void  (APIENTRY *o_glGetMaterialfv)(GLenum, GLenum, GLfloat *);
static void  (APIENTRY *o_glGetLightfv)(GLenum, GLenum, GLfloat *);
static void  (APIENTRY *o_glLightfv)(GLenum, GLenum, const GLfloat *);
static void  (APIENTRY *o_glLightf)(GLenum, GLenum, GLfloat);
static void  (APIENTRY *o_glTranslated)(GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat);
static void  (APIENTRY *o_glRotated)(GLdouble, GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glScalef)(GLfloat, GLfloat, GLfloat);
static void  (APIENTRY *o_glScaled)(GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glVertexPointer)(GLint, GLenum, GLsizei, const GLvoid *);
static void  (APIENTRY *o_glNormalPointer)(GLenum, GLsizei, const GLvoid *);
static void  (APIENTRY *o_glTexCoordPointer)(GLint, GLenum, GLsizei, const GLvoid *);
static void  (APIENTRY *o_glColorPointer)(GLint, GLenum, GLsizei, const GLvoid *);
static void  (APIENTRY *o_glEnableClientState)(GLenum);
static void  (APIENTRY *o_glDisableClientState)(GLenum);
static void  (APIENTRY *o_glShadeModel)(GLenum);
static void  (APIENTRY *o_glDepthFunc)(GLenum);
static void  (APIENTRY *o_glMateriali)(GLenum, GLenum, GLint);
static void  (APIENTRY *o_glMaterialfv)(GLenum, GLenum, const GLfloat *);
static void  (APIENTRY *o_glBindTextureR)(GLenum, GLuint);
static void  (APIENTRY *o_glPushAttrib)(GLbitfield);
static void  (APIENTRY *o_glPopAttrib)(void);
static void  (APIENTRY *o_glColor4ub)(GLubyte, GLubyte, GLubyte, GLubyte);
static void  (APIENTRY *o_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
static void  (APIENTRY *o_glColor4fv)(const GLfloat *);
static void  (APIENTRY *o_glNormal3f)(GLfloat, GLfloat, GLfloat);
static void  (APIENTRY *o_glTexCoord2f)(GLfloat, GLfloat);
static void  (APIENTRY *o_glDrawPixels)(GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
static void  (APIENTRY *o_glBitmap)(GLsizei, GLsizei, GLfloat, GLfloat, GLfloat, GLfloat, const GLubyte *);
static void  (APIENTRY *o_glCopyPixels)(GLint, GLint, GLsizei, GLsizei, GLenum);
static void  (APIENTRY *o_glCopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
static void  (APIENTRY *o_glCopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
static unsigned g_n_drawpixels, g_n_bitmap, g_n_copypixels, g_n_copytex;
static unsigned g_n_getproj, g_n_getmv;
/* Does the engine ask what its screen rectangle is?  If it never does,
   the tiles are not being culled against it and this whole idea is wrong
   -- which is worth knowing before anyone is asked to look at it. */
static unsigned g_n_getviewport;
static void  (APIENTRY *o_glMatrixMode)(GLenum);
static void  (APIENTRY *o_glLoadIdentity)(void);
static void  (APIENTRY *o_glLoadMatrixf)(const GLfloat *);
static void  (APIENTRY *o_glLoadMatrixd)(const GLdouble *);
static void  (APIENTRY *o_glMultMatrixf)(const GLfloat *);
static void  (APIENTRY *o_glMultMatrixd)(const GLdouble *);
static void  (APIENTRY *o_glFrustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glBegin)(GLenum);
static void  (APIENTRY *o_glRectf)(GLfloat, GLfloat, GLfloat, GLfloat);
static void  (APIENTRY *o_glRecti)(GLint, GLint, GLint, GLint);
static void  (APIENTRY *o_glRectd)(GLdouble, GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glVertex2f)(GLfloat, GLfloat);
static void  (APIENTRY *o_glVertex2i)(GLint, GLint);
static void  (APIENTRY *o_glVertex2d)(GLdouble, GLdouble);
static void  (APIENTRY *o_glVertex3f)(GLfloat, GLfloat, GLfloat);
static void  (APIENTRY *o_glVertex3i)(GLint, GLint, GLint);
static void  (APIENTRY *o_glVertex3d)(GLdouble, GLdouble, GLdouble);
static void  (APIENTRY *o_glEnd)(void);
static void  (APIENTRY *o_glDrawArrays)(GLenum, GLint, GLsizei);
static void  (APIENTRY *o_glDrawElements)(GLenum, GLsizei, GLenum, const GLvoid *);
static void  (APIENTRY *o_glClear)(GLbitfield);
static void  (APIENTRY *o_glEnable)(GLenum);
static void  (APIENTRY *o_glBlendFunc)(GLenum, GLenum);
static void  (APIENTRY *o_glDisable)(GLenum);
static GLboolean (APIENTRY *o_glIsEnabled)(GLenum);
#define GL_SCISSOR_TEST_ 0x0C11
#define GL_QUADS_            0x0007
#define GL_REPLACE_          0x1E01
#define GL_TEXTURE_ENV_      0x2300
#define GL_TEXTURE_ENV_MODE_ 0x2200
static GLboolean g_scissor_was;
static void  (APIENTRY *o_glGetDoublev)(GLenum, GLdouble *);
static void  (APIENTRY *o_glGetIntegerv)(GLenum, GLint *);
static void  (APIENTRY *o_glGetFloatv)(GLenum, GLfloat *);
static const GLubyte *(APIENTRY *o_glGetString)(GLenum);
static GLuint (APIENTRY *o_glGenLists)(GLsizei);
static void   (APIENTRY *o_glDeleteLists)(GLuint, GLsizei);
static void   (APIENTRY *o_glNewList)(GLuint, GLenum);
static void   (APIENTRY *o_glEndList)(void);
static void   (APIENTRY *o_glCallList)(GLuint);
static void   (APIENTRY *o_glCallLists)(GLsizei, GLenum, const GLvoid *);

/* ---- census ----------------------------------------------------------- */
typedef struct {
    unsigned swap, matmode_proj, matmode_mv, loadidentity;
    unsigned multmatf, multmatd, frustum, ortho;
    unsigned begin, drawarrays, drawelements, clear, viewport;
    unsigned dup_scene, dup_hud, lists;
} Census;
static Census g_s;

static unsigned g_frames;
static unsigned g_makecurrent;
static GLenum g_mode = GL_MODELVIEW;
static DWORD  g_report_tick;
static int    g_swapint_done;
/* Frame pacing. A scratchy OpenAL stream is usually starvation, so the
   number that matters is not the average rate but the worst stall. */
static LARGE_INTEGER g_qpf, g_last_qpc;
/* Where the per-draw duplication cost actually goes.  Immediate mode is
   captured into a display list, which means compiling one per glBegin block
   -- about 200 a frame -- and that is the obvious suspect for a 2010 game
   failing to hold 90 on the test PC. Measure before optimising. */
static LONGLONG g_t_list, g_t_elem;
/* The two paths that were never timed, and which turn out to be seven
   eighths of all duplication. */
static LONGLONG g_t_cl, g_t_cls;
static unsigned g_n_cl, g_n_cls;
/* The engine's once-per-frame glFinish: how many it made, how many we
   swallowed. Declared here rather than beside the hook because the frame
   report, which prints them, is earlier in this file. */
static unsigned g_finish_skipped, g_finish_logged, g_finish_calls;
/* Could the second eye's draws be batched?  A run is consecutive glCallList
   calls with NOTHING in between -- no pass-through, no hooked state call.
   Only a run can share one viewport switch and one glCallLists. */
static unsigned g_run_mark;      /* g_real_calls at the last list call */
static unsigned g_runs, g_run_lists, g_run_len, g_run_max;
static unsigned g_run_hist[6];   /* runs of 1, 2, 3-4, 5-8, 9-16, 17+ */
/* Which pass-through functions appear in the gap between two list calls,
   and how wide is the gap.  Diagnostic only: it diffs the whole counter
   array per draw. */
static unsigned g_gap_snap[512];
static unsigned g_gap_hits[512];
static unsigned g_gap_hist[8];   /* gap of 0,1,2,3,4,5-8,9-16,17+ calls */
static unsigned g_gap_n, g_gap_total;
/* What the engine records INTO a display list.  If nothing here changes
   state, a state cache survives glCallList and the full filter is safe. */
static unsigned g_lst_snap[512];
static unsigned g_lst_hits[512];
static unsigned g_lst_count;
static void list_begin_note(void) {
    int i, n = g_real_n < 512 ? g_real_n : 512;
    for (i = 0; i < n; i++) g_lst_snap[i] = g_real_count[i];
}
static void list_end_note(void) {
    int i, n = g_real_n < 512 ? g_real_n : 512;
    for (i = 0; i < n; i++) g_lst_hits[i] += g_real_count[i] - g_lst_snap[i];
    g_lst_count++;
}
static void gap_note(void) {
    int i, n = g_real_n < 512 ? g_real_n : 512;
    unsigned moved = 0;
    for (i = 0; i < n; i++) {
        unsigned d = g_real_count[i] - g_gap_snap[i];
        if (d) { g_gap_hits[i] += d; moved += d; g_gap_snap[i] = g_real_count[i]; }
    }
    g_gap_total += moved;
    g_gap_n++;
    if (moved == 0) g_gap_hist[0]++;
    else if (moved == 1) g_gap_hist[1]++;
    else if (moved == 2) g_gap_hist[2]++;
    else if (moved == 3) g_gap_hist[3]++;
    else if (moved == 4) g_gap_hist[4]++;
    else if (moved <= 8) g_gap_hist[5]++;
    else if (moved <= 16) g_gap_hist[6]++;
    else g_gap_hist[7]++;
}
static void run_close(void) {
    if (!g_run_len) return;
    g_runs++;
    if (g_run_len > g_run_max) g_run_max = g_run_len;
    if (g_run_len == 1) g_run_hist[0]++;
    else if (g_run_len == 2) g_run_hist[1]++;
    else if (g_run_len <= 4) g_run_hist[2]++;
    else if (g_run_len <= 8) g_run_hist[3]++;
    else if (g_run_len <= 16) g_run_hist[4]++;
    else g_run_hist[5]++;
    g_run_len = 0;
}
/* Frames that took longer than stall_ms, with where their time went.  The
   worst few print in the next report: a stall that is mostly compositor
   wait is the headset link, one that is mostly unaccounted is the game or
   the driver. */
#define STALLS 5
static struct { unsigned frame; double ms, submit, wait, mirror; unsigned draws; }
       g_stall[STALLS];
static unsigned g_nstall, g_stall_total;
static LONGLONG g_pf_finish, g_pf_wait, g_pf_mirror;
static unsigned g_pf_draws;
static unsigned g_n_lists, g_n_elems;
/* What the game's own backdrop is doing this frame, found by watching the
   colour it draws with rather than by counting draws. */
static int   g_dim_seen;
static float g_dim_alpha;
/* The backdrop's colour, not assumed black: measured on this game it is a
   dark navy, and a black surround beside it reads as a second, different
   dim.  Red and green matched to the pixel; only blue gave it away. */
static float g_dim_rgb[3];
static float g_dim_held_rgb[3];
static int   g_dim_checks;
static int   g_dim_draws;    /* draws in the pass, looked at or not */
/* Bounding box of the primitive currently between glBegin and glEnd, in the
   engine's own 2D canvas units.  The one measurement that separates a
   dimming backdrop from an icon. */
static float g_vb_minx, g_vb_maxx, g_vb_miny, g_vb_maxy;
static int   g_vb_have;
static float g_dim_cover;   /* fraction of the canvas the last one spanned */
static int   g_dim_by_cover; /* the backdrop was found by size, not colour */
static float g_dim_bestcover; /* biggest a translucent draw got, this frame */
static float g_dim_measured;  /* how much the 2D pass darkened the view */
static int   g_dim_confirm;   /* consecutive samples agreeing it is a dim */
/* The last dim the game actually drew, and whether we are currently standing
   in for one it has stopped drawing. */
static float g_dim_held;
/* Frames the hold condition has been true. The condition goes true for a
   couple of frames during EVERY screen change, so acting on it immediately
   put a two-frame dim flash on every transition. A real hold lasts as long as
   the screen is up. */
static int   g_dim_hold_wait;
static int   g_dim_holding;
/* Did the GAME itself draw a backdrop this frame?  g_dim_seen cannot answer
   that once the hold has synthesised one, and the difference is the whole
   double-dim question. */
static int   g_dim_was_real;
/* Frames on which both dims landed.  The reported double dim is a
   transient, so a counter is the only instrument that can see it: a capture
   taken a frame later shows nothing at all. */
static unsigned g_dim_both_frames;
/* Defined further down, beside the surround they feed; called from the
   pass bookkeeping, which comes first in this file. */
static void dim_sample_after(void);
static void dim_fill_canvas(void);
static void panel_apply_blend(void);
static int  panel_blend_ready(void);
static void panel_composite(void);

/* While this is set the 2D pass is being drawn ONCE into a canvas-sized
   target instead of once per eye. Everything that duplicates must stand down
   for the duration, and the engine's own viewport and scissor then apply at
   1:1 -- which is how they were always meant to. */
static int      g_panel_capture;
/* A 3D model drawn INSIDE the menu: the giant in "Choose your Giant", the
   keflings and giants on the town square's upgrade cards. The game sets a
   close perspective camera (near 0.58 against the world's 70 and the
   backdrop's 10) in the middle of the 2D pass, after the world, and draws a
   few models clipped to the card. Taken for the world, it ended the menu
   capture and was drawn with the headset's eyes: huge, out of its box, in
   one eye (25 Sep). It belongs to the menu, drawn into the panel with the
   game's own camera exactly as on the monitor. */
static int      g_embed3d;
/* The frame number in which a WORLD camera (near > 10.5) last began. The
   capture used to resume after a scene pass only if it drew over 100
   objects, and that count follows the head: looking at open sky left the
   second world pass under 100, the menu capture did not resume, and the
   giant on his card was drawn as the world again, in the box or not
   depending on the view (25 Sep). A world camera having run this frame is the
   fact that matters, not how much it drew. */
static unsigned g_world_frame;
static int      g_world_frame_ok;
/* glBlendFuncSeparate, resolved through wglGetProcAddress -- it is GL 1.4 and
   the system opengl32 only exports 1.1. Without it the panel's alpha channel
   comes out squared and every translucent element composites wrongly. */
static void (APIENTRY *d_blendsep)(GLenum, GLenum, GLenum, GLenum);
static int      d_blendsep_tried;
/* Did THIS frame capture anything? The texture keeps its contents between
   frames, so compositing without this would paint the last menu back on for
   every frame after it closed. */
static int      g_panel_have;
static GLenum   g_panel_src = GL_SRC_ALPHA, g_panel_dst = GL_ONE_MINUS_SRC_ALPHA;
static unsigned g_panel_frames;

/* The dim painters' GL entry points.  Resolved here rather than in the proxy's
   bind table: these are plain passthroughs the port never needed until now,
   and the table is for the calls it intercepts.

   In its own function because TWO painters need them and the other one can
   return before resolving -- which left the canvas fill silently dead on any
   screen where the game draws no backdrop of its own. */
static int dim_procs_ready(void);
/* The fill is per FRAME. A frame holds several 2D passes and painting it
   in each one composes two layers of the same alpha -- a second dim
   screen. Latched here, cleared at swap. */
static int      g_dim_filled_this_frame;
static unsigned g_dim_fill_suppressed;
static const char *g_dim_path = "?";  /* which call is drawing right now */
static int   g_dim_traced;    /* lines emitted for this pass */
static int   g_dim_trace_total;   /* lines emitted all session */
/* Armed by a CHANGE in the 2D pass's draw count -- what a screen opening
   looks like -- rather than by the menu classifier, which is the thing
   this is here to stop trusting. */
static int   g_dim_trace_armed;
static unsigned g_dim_prev_draws;
/* The same record, scoped to ONE frame, for the per-opening report. The
   300-frame one above is right for the 300-frame line and wrong for this. */
static float g_dim_bestf[4];
static int   g_dim_bestf_ok;
static float g_dim_best[4];
static int   g_dim_best_ok;
static unsigned g_dim_frames;      /* frames the surround actually drew */
static double g_worst_ms;
/* Draws attributed to each pass classification, and the viewport in force for
   the ones the port never claimed. */
static unsigned g_mode_draws[3];
static int      g_orphan_vp[4];
/* The engine's frame, split at its first draw call: time before it is
   simulation or sleeping, time after it is submission. */
static LARGE_INTEGER g_swap_left;      /* when our swap hook returned */
static LONGLONG g_t_idle, g_t_draw;
static int      g_saw_first_draw;
/* Where the frame goes.  Ticks, summed over the report window, so the report
   can divide by the frame count and print milliseconds per frame. */
static LONGLONG g_t_finish, g_t_mirror, g_t_wait, g_t_input, g_t_frame;
/* Desktop mirror pacing.  See the note below in hk_wglSwapBuffers. */
static LONGLONG g_next_present;
static unsigned g_presents, g_present_skips;
static unsigned g_stalls;

/* ---- stereo state ------------------------------------------------------ */
enum { DUP_NONE = 0, DUP_HUD, DUP_SCENE };
static int    g_dup_mode;
static int    g_internal;        /* we are issuing GL ourselves; do not track */
static int    g_capturing;       /* inside OUR display-list capture */
static int    g_engine_list;     /* the ENGINE is compiling a list of its own */
static GLuint g_list;
/* ---- in-block census (diagnostic) --------------------------------------- */
static unsigned g_bc_snap[512];      /* pass-through counters at glBegin */
static unsigned g_bc_inside[512];    /* calls inside blocks, per name */
static unsigned g_bc_vertex[8];      /* hooked glVertex* inside blocks */
static unsigned g_bc_modes[16];      /* glBegin mode histogram */
static unsigned g_bc_blocks, g_bc_verts, g_bc_maxverts, g_bc_cur;
static int      g_bc_in;
/* ---- pass-list stereo: one recording per scene pass ------------------- */
#define PL_MAXSEG 8
#define GL_COMPILE_AND_EXECUTE_   0x1301
#define GL_ALL_ATTRIB_BITS_       0x000FFFFF
#define GL_CLIENT_ALL_ATTRIB_BITS_ 0xFFFFFFFF
#define GL_PROGRAM_OBJECT_ARB_    0x8B40
static int      g_pl_rec;        /* a scene pass is being recorded now */
static int      g_pl_open;       /* a segment list is open */
static int      g_pl_nseg;       /* segments closed so far this pass */
static int      g_pl_done;       /* this pass already replayed (overflow) */
static int      g_pl_reopen;     /* reopen a segment after the engine's list */
static int      g_pl_draws;      /* draws recorded this pass */
static GLuint   g_pl_base;       /* PL_MAXSEG contiguous list names */
static DWORD    g_pl_tid;        /* the thread the recording belongs to */
static int      g_pl_failed;
static unsigned g_pl_prog;       /* the shader bound when recording began */
static int      g_pl_prog_ok;
static unsigned g_pl_passes, g_pl_segs, g_pl_draws_total, g_pl_overflow,
                g_pl_errors;
static LONGLONG g_pl_t_replay;
static void   (APIENTRY *pl_pushattr)(GLbitfield);
static void   (APIENTRY *pl_popattr)(void);
static void   (APIENTRY *pl_pushcattr)(GLbitfield);
static void   (APIENTRY *pl_popcattr)(void);
static void   (APIENTRY *pl_pushm)(void);
static void   (APIENTRY *pl_popm)(void);
static GLenum (APIENTRY *pl_geterror)(void);
static HGLRC  (WINAPI   *pl_getcurrent)(void);
static unsigned (APIENTRY *pl_getprog)(GLenum);
static void   (APIENTRY *pl_useprog)(unsigned);
/* ---- deferred per-eye state ------------------------------------------ */
static int      g_dup_loaded = -1;  /* eye whose state is loaded; -1 = the
                                       engine's own */
static DWORD    g_dup_tid;          /* the thread that loaded it */
static unsigned g_dup_switches, g_dup_flushes;
static void dup_flush(void);
/* Defined with the state filter further down; the attrib hooks and the
   swap, which are earlier in this file, all have to drop the cache. */
static void sf_drop(void);
static void pl_begin(void);
static void pl_end(void);
static void pl_open_segment(void);
static void pl_close_segment(void);
/* Any engine call that touches the projection is the end of the pass, and
   it must replay BEFORE the call goes through, or eye 1 is drawn with the
   next pass's projection. */
#define PL_GUARD() do { if (!g_internal && g_mode == GL_PROJECTION) { \
                           if (g_pl_rec) pl_end(); dup_flush(); } } while (0)
static double g_near = 1.0, g_far = 1000.0;
/* The last frame that drew a 3D scene. The splash and loading screens draw
   none, and must stay upright at eye height like any picture in a headset:
   menu_tilt and menu_height only match a tipped-up WORLD (25 Sep). */
static unsigned g_scene_frame;
static int g_scene_seen;
int kv_scene_drawn(void) { return g_scene_seen && g_frames - g_scene_frame <= 3; }
static double g_far_original;   /* before we widened it */
static float  g_last_engine_fov;
static float  g_eng_aspect0;     /* the engine's own aspect, before we narrow */
/* How many frames actually had their view turned by the head.  The engine
   camera's width is derived assuming this happens; counting it is what stops
   the two silently disagreeing. */
static unsigned g_headcull_hits;
static unsigned g_headcull_frame = 0xffffffffu;
/* the window size before we touched it, for the log line */
/* The engine's view matrix for the scene pass, read back at the moment it
   is installed.  Its upper 3x3 is the camera orientation, which is what we
   need to find in the camera struct to make the engine look where the head
   looks -- the only thing that fixes culling at any head angle AND stops
   the engine drawing a 150 degree cone it cannot afford. */
static float  g_view_mat[16];
static int    g_have_view;
/* Which perspective pass is the WORLD one.  A frame installs more than one
   camera: one with a near plane of 10 and one with 70, and the first
   modelview of the frame belongs to the wrong one.  Taking "the first" is
   why the camera dump never once fired. */
static int    g_world_pass, g_world_view_taken;
/* g_have_view is cleared at the TOP of the swap handler, ready for the next
   frame, so anything below that point sees 0 and can never read the view.
   Keep the last good one in its own copy instead. */
static float  g_last_view[16];
static int    g_last_view_ok;
/* Never reset.  The census counters are zeroed every 300 frames, and a
   measurement window that straddles a reset reads as a collapse in draw
   count -- exactly the shape of the signal being looked for. */
static unsigned g_draws_total;
/* Indexed draws, never reset: these are the OBJECTS -- the trees, rocks
   and sheep -- as distinct from the terrain, which is immediate mode. */
static unsigned g_elems_total;
/* Display lists: the STATIC SCENERY.  This is the count that has to move
   -- the indexed path carries 23 things a frame and they are the
   characters, not the trees. */
static unsigned g_lists_total;
/* How far away the objects the engine asks about are. */
static int    g_dpi_result;
static DWORD  g_dpi_err;
static float  g_eng_fovy, g_eng_aspect;  /* as the engine built them */

/* Does widening the engine camera widen its culling?  A single before/after
   run cannot answer that: the draw count depends on where the camera is
   looking, so two runs are two different scenes.  So alternate the patch on
   and off every 300 frames within ONE run and compare the two arms.  Same
   session, same save, interleaved -- the scene difference averages out. */
static double g_arm_draws[2];
static unsigned g_arm_frames[2];
static int g_arm;                 /* 1 = patched this block */
static unsigned g_draws_prev;     /* census value at the last frame */
static float  g_eng_proj[16];    /* the engine's own projection for this pass */
static GLint  g_eng_vp[4];       /* the engine's own viewport */
static GLint  g_eng_sc[4];       /* and its own scissor rectangle */
static float  g_hud_w = 3840.0f, g_hud_h = 2160.0f;  /* 2D canvas */
static int    g_have_scissor;
static unsigned g_n_scissor;

/* Pass trace.  The 2D pass was never identified by the earlier instrumentation
   -- the census counts exactly one glOrtho per frame but it never arrived while
   GL_PROJECTION was the active matrix mode -- and "the menus do not fuse" is
   what that costs.  This records the ordered truth for a couple of frames. */
static int    g_trace;
/* Trace on the SYMPTOM, not on a frame number chosen blind.  The 2D pass
   draws a steady handful for the in-game HUD; a menu multiplies that, so
   a jump is the menu opening and is exactly the frame worth recording. */
static unsigned g_hud_baseline;
static unsigned g_hud_last;
static int    g_trace_armed;
static int    g_menu_up;        /* the 2D pass is drawing a menu, not the HUD */
int kv_menu_up(void) { return g_menu_up; }
static int    g_pass_no;
static unsigned g_pass_draws;
static unsigned g_pass_draws_final;

/* Flushing every line to two files costs the render thread two synchronous
   writes per line, and a frame report is about 45 lines. Flush at most five
   times a second instead -- and always during bring-up, where a crash is
   most likely and a complete file matters more than the milliseconds. */
static DWORD g_log_flush_tick;
static int   g_log_dirty;
void kv_log(const char *fmt, ...) {
    va_list ap;
    int flush_now;
    DWORD now;
    if (!g_log && !g_log2) return;
    EnterCriticalSection(&g_lock);
    now = GetTickCount();
    flush_now = (g_frames < 600) || (now - g_log_flush_tick) >= 200;
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        if (flush_now) fflush(g_log);
    }
    /* The copy beside the game, for machines nobody can log into. */
    if (g_log2) {
        va_start(ap, fmt);
        vfprintf(g_log2, fmt, ap);
        va_end(ap);
        fputc('\n', g_log2);
        if (flush_now) fflush(g_log2);
    }
    if (flush_now) { g_log_flush_tick = now; g_log_dirty = 0; }
    else g_log_dirty = 1;
    LeaveCriticalSection(&g_lock);
}

/* ---- pass classification ---------------------------------------------- */
/* Called after any call that composes a projection matrix.  Reads the result
   back out of GL rather than reconstructing it from the call sequence: the
   engine builds its perspective with glMultMatrixd and its 2D with glOrtho,
   and the composed matrix says unambiguously which it produced. */
/* ---- read-back shadows: see tools/no_readbacks.py ---- */
#define PJ_DEPTH 32
static float g_pj[PJ_DEPTH][16];
static int   g_pj_sp;
/* Self-verifying answers: compare with the driver for the first RB_CHECKS,
   then take over; one real disagreement and it is off for the session. */
#define RB_CHECKS 200
typedef struct { const char *name; int trust; unsigned checks; double maxd; } RbTrust;
static RbTrust g_rb_mv = { "modelview", 0, 0, 0.0 };
static RbTrust g_rb_pj = { "projection", 0, 0, 0.0 };
static RbTrust g_rb_vp = { "viewport", 0, 0, 0.0 };
static void rb_check(RbTrust *t, const float *shadow, const float *real, int n) {
    int i;
    double worst = 0.0, bad = 0.0;
    if (t->trust) return;
    for (i = 0; i < n; i++) {
        double d = (double)shadow[i] - (double)real[i];
        double a = real[i] < 0 ? -(double)real[i] : (double)real[i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
        if (d > 1e-4 * (1.0 + a) && d > bad) bad = d;
    }
    if (worst > t->maxd) t->maxd = worst;
    t->checks++;
    if (bad > 0.0) {
        t->trust = -1;
        kv_log("READ-BACK SHADOW: the %s shadow DISAGREED with the driver by "
               "%.6f after %u checks -- switched off for this session, the "
               "driver answers instead.", t->name, bad, t->checks);
    } else if (t->checks >= RB_CHECKS) {
        t->trust = 1;
        kv_log("READ-BACK SHADOW: the %s shadow matched the driver on %u "
               "reads (largest difference %.7f) -- it answers from now on.",
               t->name, t->checks, t->maxd);
    }
}
/* Implementation constants, cached from the driver's first answer. */
static int rb_is_constant(GLenum p) {
    switch (p) {
        case 0x84E2: /* GL_MAX_TEXTURE_UNITS */
        case 0x8871: /* GL_MAX_TEXTURE_COORDS */
        case 0x8872: /* GL_MAX_TEXTURE_IMAGE_UNITS */
        case 0x0D33: /* GL_MAX_TEXTURE_SIZE */
        case 0x0D31: /* GL_MAX_LIGHTS */
        case 0x0D32: /* GL_MAX_CLIP_PLANES */
        case 0x0D36: /* GL_MAX_MODELVIEW_STACK_DEPTH */
        case 0x0D38: /* GL_MAX_PROJECTION_STACK_DEPTH */
            return 1;
        default: return 0;
    }
}
static struct { GLenum p; GLint v; } g_rb_const[16];
static unsigned g_rb_nconst, g_rb_const_hits;
static int      g_vp_stale;             /* glPopAttrib may have restored it */
static int      g_colmat_on;            /* GL_COLOR_MATERIAL, tracked */
static void     lt_drop(void);
static HGLRC    g_last_rc;              /* last non-NULL context made current */
/* skip_noop (tools/skip_noops.py): exact enable flags, -1 = unknown */
static signed char g_en_light = -1, g_en_atest = -1, g_en_cmat = -1;
static unsigned    g_nop_cand, g_nop_skip, g_nop_frames;   /* window */
static int         g_nop_trust;          /* 0 verifying, 1 trusted, -1 off */
static unsigned    g_nop_checked;
static DWORD       g_nop_lastcheck;
static int         g_nop_on;             /* skipping active THIS frame */
static double      g_nop_arm_ms[2]; static unsigned g_nop_arm_n[2];
static unsigned    g_nop_since_switch;
static unsigned    g_nop_frame_world;    /* world list draws this frame */
/* exact state for the no-op rule (tools/noop_exact_state.py); -1 unknown */
#define NX_UNITS 4
typedef struct {
    signed char light, atest, cmat;
    signed char tex[NX_UNITS], env[NX_UNITS];   /* env: 1 MODULATE, 0 other */
    signed char unit;                           /* active texture unit */
    signed char dif_ok; float dif_a;            /* front diffuse alpha */
    signed char af_ok; GLenum af_func; float af_ref;
} NxState;
static NxState  g_nx;
/* bound GL_TEXTURE_2D on unit 0, for the cursor sprite (the grip mouse) */
static GLuint   g_tex0 = 0xFFFFFFFFu;
static struct { GLbitfield mask; NxState st; } g_nx_stack[16];
static int      g_nx_sp;
static void nx_defaults(void) {
    int u;
    g_nx.light = 0; g_nx.atest = 0; g_nx.cmat = 0;
    for (u = 0; u < NX_UNITS; u++) { g_nx.tex[u] = 0; g_nx.env[u] = 1; }
    g_nx.unit = 0;
    g_nx.dif_ok = 1; g_nx.dif_a = 1.0f;         /* GL default diffuse alpha 1 */
    g_nx.af_ok = 1; g_nx.af_func = GL_ALWAYS; g_nx.af_ref = 0.0f;
    g_nx_sp = 0;
}
static DWORD       g_nop_t0;             /* tick the world first drew */
/* scissor enable and current colour shadows (tools/no_readbacks3.py) */
static RbTrust  g_rb_sc = { "scissor enable", 0, 0, 0.0 };
static RbTrust  g_rb_cc = { "current colour", 0, 0, 0.0 };
static int      g_sc_on = -1;          /* engine's GL_SCISSOR_TEST, -1 unknown */
static GLfloat  g_cc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static int      g_cc_valid;            /* 0 unknown: ask the driver */
static unsigned g_isync_n;             /* the port's own reads that still went to the driver */
static int sc_enabled(void) {
    int real;
    if (g_rb_sc.trust == 1 && g_sc_on >= 0) return g_sc_on;
    real = (o_glIsEnabled && o_glIsEnabled(0x0C11)) ? 1 : 0;
    g_isync_n++;
    if (g_rb_sc.trust == 0 && g_sc_on >= 0) {
        float a = (float)g_sc_on, b = (float)real;
        rb_check(&g_rb_sc, &a, &b, 1);
    }
    g_sc_on = real;
    return real;
}
static struct { void *ra; unsigned n, errs; GLenum first; unsigned first_frame; } g_ge[8];
static RbTrust  g_rb_err = { "glGetError", 0, 0, 0.0 };
static RbTrust  g_rb_mat = { "material", 0, 0, 0.0 };
static RbTrust  g_rb_lt  = { "light", 0, 0, 0.0 };

static void projection_changed(void) {
    GLfloat m[16];
    if (g_internal) return;
    /* Belt and braces: the guards in the matrix hooks end a recording
       before the projection moves, so this is normally already 0. */
    pl_end();
    dup_flush();
    g_pl_done = 0;
    if (g_rb_pj.trust == 1) {
        memcpy(m, g_pj[g_pj_sp], sizeof(m));
    } else {
        o_glGetFloatv(GL_PROJECTION_MATRIX, m);
        if (g_rb_pj.trust == 0) rb_check(&g_rb_pj, g_pj[g_pj_sp], m, 16);
    }
    memcpy(g_eng_proj, m, sizeof(m));
    if (g_trace && g_pass_no)
        kv_log("   pass %d ended with %u draws", g_pass_no, g_pass_draws);
    /* The w row is (0,0,-1,0) for a frustum and (0,0,0,1) for an ortho. */
    if (m[11] != 0.0f && g_panel_capture && m[10] != 1.0f &&
        m[14] / (m[10] - 1.0f) > 0.0f && m[14] / (m[10] - 1.0f) < 5.0f) {
        /* a model inside the menu: stay in the capture, as 2D, and give it
           a clean depth buffer (the panel's is never cleared per frame) */
        static void (APIENTRY *pa)(GLbitfield);
        static void (APIENTRY *pp)(void);
        static void (APIENTRY *dm)(GLboolean);
        static int said;
        if (!pa) {
            pa = (void (APIENTRY *)(GLbitfield))proxy_real_proc("glPushAttrib");
            pp = (void (APIENTRY *)(void))proxy_real_proc("glPopAttrib");
            dm = (void (APIENTRY *)(GLboolean))proxy_real_proc("glDepthMask");
        }
        g_embed3d = 1;
        if (pa && pp && dm) {
            g_internal++;
            pa(GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_ENABLE_BIT);
            o_glDisable(GL_SCISSOR_TEST_);
            dm(GL_TRUE);
            o_glClear(GL_DEPTH_BUFFER_BIT);
            pp();
            g_internal--;
        }
        if (!said) {
            said = 1;
            kv_log("EMBED: a close 3D camera (near %.2f) inside the menu -- a "
                   "model on a card; drawn into the menu screen with the "
                   "game's own camera, not as the world",
                   m[14] / (m[10] - 1.0f));
        }
    } else if (m[11] != 0.0f) {
        g_embed3d = 0;
        g_scene_frame = g_frames; g_scene_seen = 1;
        g_near = m[14] / (m[10] - 1.0f);
        g_far  = m[14] / (m[10] + 1.0f);
        /* Remember what the engine shipped with, so the patch always scales
           the original rather than compounding on itself every frame. */
        if (g_far_original <= 1.0 && g_far > 1.0 && g_near > 10.0)
            g_far_original = g_far;
        g_dup_mode = DUP_SCENE;
        g_world_pass = (g_near > 10.5);
        if (g_world_pass) { g_world_frame = g_frames; g_world_frame_ok = 1; }
        if (g_world_pass) g_world_view_taken = 0;
        /* Report the world camera's depth range whenever it CHANGES.  This is
           the proof that a draw-distance patch was accepted: the engine builds
           this matrix from the value we wrote, so if the far plane here moves,
           the engine is culling to the new distance. */
        {
            /* Only the WORLD camera, and only when its far plane really moves.
               The frame sets up two cameras -- the menu backdrop at 10..2000
               and the world at 70..8800 -- so a plain "has it changed" test
               fires twice every frame and wrote 443 KB of log in one session. */
            static double said_far;
            if (g_near > 10.0 && g_far != said_far) {
                said_far = g_far;
                kv_log("CAMERA: the engine is drawing from %.1f to %.1f",
                       g_near, g_far);
            }
        }
        /* Tell the VR side how wide the engine is willing to draw, so the eye
           frustum can be fitted inside it. */
        /* The scene pass is beginning.  Take the view matrix from GL right
           now, before any object transform has been pushed onto it.  It was
           being taken from "the first modelview multiply", which is not the
           same moment -- so the view used to store an object's world position
           did not match the view used to compute it, and the same tree landed
           somewhere new every frame.  That is why nothing ever deduplicated
           and the cache filled to its limit in a second. */
        /* A scene pass is starting.  If the panel target is still bound
           the world would be drawn into a canvas-sized texture instead of
           into the eyes, so let go of it first.  Whatever the 2D pass already
           put there is kept and composited at swap. */
        if (g_panel_capture) {
            /* Each distinct camera that ends a capture, once: a model on a
               card whose camera is NOT close enough to be caught above would
               show up here (the town square's upgrade cards are unmeasured). */
            static float seen[8];
            static int nseen;
            int k, have = 0;
            for (k = 0; k < nseen; k++)
                if (fabs(seen[k] - g_near) < 0.5) have = 1;
            if (!have && nseen < 8) {
                seen[nseen++] = (float)g_near;
                kv_log("EMBED: a 3D camera (near %.2f) ended the menu capture "
                       "and is drawn as the world. Right for the world (70) and "
                       "the backdrop (10); a model on a menu card would be "
                       "wrong here.", g_near);
            }
            vr_panel_end();
            g_panel_capture = 0;
        }
        if (m[0] > 0.0f && m[5] > 0.0f) {
            vr_set_engine_fov(1.0f / m[0], 1.0f / m[5]);
            g_eng_fovy = (float)(2.0 * atan(1.0 / m[5]) * 57.295779513082323);
            g_eng_aspect = m[5] / m[0];
            /* The shape it chose for itself, captured before anything of
               ours has narrowed it -- that is what aspect_apply searches
               for and what it reverts to. */
            if (g_eng_aspect0 <= 0.0f && g_eng_aspect > 1.2f)
                g_eng_aspect0 = g_eng_aspect;
        }
    } else {
        /* The 2D pass is starting, so the scene pass has just finished and its
           state is still current -- the only point in the frame where the
           remembered scenery can be drawn with the same projection, depth
           test and fog the engine used for the real thing. */
        /* Only after a substantial scene pass.  A frame contains several
           perspective passes, and flushing at the first transition to 2D ran
           before the main one had drawn anything -- so every object was
           "missing" and got drawn back, which is the duplicate world that
           appeared. */
        /* Is this the 2D pass that follows the WORLD?  Read both facts
           before g_dup_mode is reassigned below: it still holds the outgoing
           pass's mode, and g_pass_draws is still the outgoing pass's count.

           This matters because the frame's FIRST 2D pass is not necessarily
           after the main scene pass, and a fill painted in an earlier one is
           simply drawn over by the world -- which is what made the held dim
           vanish while its own counter said it was painting every frame. */
        int after_world = (g_dup_mode == DUP_SCENE && g_pass_draws > 100);
        int world_done = (g_dup_mode == DUP_SCENE && g_world_frame_ok &&
                          g_world_frame == g_frames);
        g_embed3d = 0;
        g_dup_mode = DUP_HUD;
        /* Behind everything this pass is about to draw, which is where the
           game's own backdrop would be -- and on top of the world, which is
           the half the latch got wrong. */
        if (after_world) dim_fill_canvas();
        /* Draw this pass ONCE into a canvas-sized target rather than once per
           eye. Only the pass that follows the world: an earlier 2D pass would
           be overwritten by the world anyway, and capturing it would hide it
           from the composite. */
        /* Say WHY when it does not engage.  A silent no-op here read as a
           clean A/B pass: both arms were identical because neither captured,
           which is the most flattering way for a test to be worthless. */
        {
            /* Once per distinct state, not once ever: the first evaluation
               happens on the splash screen before any world has been drawn,
               so a one-shot log answers a question nobody asked. */
            /* And only for the pass that could engage.  A frame holds a 2D
               pass before the world and one after it, so after_world
               alternates every pass and a line keyed on it was written
               TWICE A FRAME -- a flushed file write per pass, for the whole
               session, and a 13 MB log.  The pass before the world can
               never capture and has nothing to explain. */
            static int said = -1;
            int state = (vr_stereo_active() ? 2 : 0) |
                        (g_vrcfg.panel_flat_test ? 4 : 0) |
                        (panel_blend_ready() ? 8 : 0) |
                        (g_panel_capture ? 16 : 0);
            if (world_done && said != state && g_vrcfg.panel_once) {
                said = state;
                kv_log("PANEL gate: after_world=%d panel_once=%d stereo=%d "
                       "flat_test=%d canvas=%.0fx%.0f blend_ready=%d "
                       "(all must be non-zero, and stereo OR flat_test)",
                       world_done, g_vrcfg.panel_once, vr_stereo_active(),
                       g_vrcfg.panel_flat_test, g_hud_w, g_hud_h,
                       panel_blend_ready());
            }
        }
        if (world_done && g_vrcfg.panel_once && !g_panel_capture &&
            (vr_stereo_active() || g_vrcfg.panel_flat_test) &&
            g_hud_w > 0.0f && g_hud_h > 0.0f && panel_blend_ready()) {
            if (vr_panel_begin((int)g_hud_w, (int)g_hud_h, !g_panel_have)) {
                g_panel_capture = 1;
                g_panel_have = 1;
                /* The engine may draw without setting a blend mode, relying
                   on whatever the last pass left. Establish ours now. */
                g_internal = 1;
                panel_apply_blend();
                g_internal = 0;
                g_panel_frames++;
                if (g_panel_frames == 1)
                    kv_log("PANEL: the 2D pass is now drawn once into a "
                           "%.0fx%.0f target and blitted to each eye, instead "
                           "of being re-issued per eye at eye resolution.",
                           g_hud_w, g_hud_h);
            }
        }
        /* Per PASS, not per frame.  A frame holds several 2D passes and the
           in-game HUD is drawn first, so a per-frame budget was always spent
           before the menu drew its backdrop. */
        g_dim_checks = 0;
        g_dim_draws = 0;
    }
    /* Reset per pass ALWAYS, not only while tracing: the menu detector reads
       the final pass's own count, and if this only ran during a trace the
       count would be the whole frame's and the detector would never fire. */
    g_pass_no++;
    g_pass_draws = 0;
    if (g_trace) {
        if (g_dup_mode == DUP_SCENE)
            kv_log("  pass %d SCENE  near=%.2f far=%.2f  tanx=%.4f tany=%.4f "
                   "(%.1f x %.1f deg)", g_pass_no, g_near, g_far,
                   1.0f / m[0], 1.0f / m[5],
                   2.0 * atan(1.0 / m[0]) * 57.2957795,
                   2.0 * atan(1.0 / m[5]) * 57.2957795);
        else
            kv_log("  pass %d 2D/ORTHO  m0=%.6f m5=%.6f m12=%.4f m13=%.4f "
                   "(identity=%d)", g_pass_no, m[0], m[5], m[12], m[13],
                   (m[0] == 1.0f && m[5] == 1.0f && m[12] == 0.0f));
    }
}

/* How many eyes this pass is drawn into.  Normally every eye; with the 2D
   A/B running, the 2D arm alternates between both and just the left, which is
   the only way to see what duplicating it costs. */
static int g_hud_arm;               /* 0 = both eyes, 1 = left only */
static double g_arm_fps_sum[2];
static unsigned g_arm_fps_n[2];

/* desk_stereo: duplicate at the desk exactly as in the headset, each eye
   half of the engine's own viewport. See tools/desk_stereo.py. */
static int desk_stereo_on(void) {
    return g_vrcfg.desk_stereo && !vr_stereo_active();
}
static int pass_eye_count(void) {
    int n = desk_stereo_on() ? 2 : vr_eye_count();
    if (g_vrcfg.hud_ab && g_hud_arm && g_dup_mode == DUP_HUD && n > 1)
        return 1;
    return n;
}

/* The first draw call of the engine's frame closes the idle half and opens
   the drawing half.  Called from the draw entry points, and cheap: one compare
   after the first one each frame. */
/* Every draw, tallied against what the port thinks it is drawing into.  A
   draw under DUP_NONE is one the port never duplicated and never gave an eye
   viewport to -- it goes wherever the engine's own viewport points, which is
   the canvas, in the corner of a larger eye buffer. */
static void note_draw_mode(void) {
    if (g_internal) return;
    g_mode_draws[g_dup_mode == DUP_SCENE ? 1 :
                 (g_dup_mode == DUP_HUD ? 2 : 0)]++;
    if (g_dup_mode != DUP_SCENE && g_dup_mode != DUP_HUD) {
        g_orphan_vp[0] = g_eng_vp[0]; g_orphan_vp[1] = g_eng_vp[1];
        g_orphan_vp[2] = g_eng_vp[2]; g_orphan_vp[3] = g_eng_vp[3];
    }
}

static void note_first_draw(void) {
    LARGE_INTEGER now;
    if (g_saw_first_draw || g_internal) return;
    g_saw_first_draw = 1;
    if (!g_swap_left.QuadPart) return;
    QueryPerformanceCounter(&now);
    g_t_idle += now.QuadPart - g_swap_left.QuadPart;
    g_swap_left = now;              /* reused as "when drawing started" */
}

static int dup_active(void) {
    /* While the ENGINE is compiling a display list of its own, nothing is
       being drawn -- it is being recorded.  Duplicating there would nest a
       list inside a list (illegal) and would end the engine's list early,
       corrupting geometry it will draw for the rest of the session.  The
       duplication happens later, when it calls the list. */
    /* Capturing the 2D pass means drawing it once, so nothing here may
       duplicate. This is the whole saving: ~2100 blocks a menu frame. */
    if (g_panel_capture) return 0;
    if (g_dup_mode == DUP_NONE || g_internal || g_capturing ||
        g_engine_list || !(vr_stereo_active() || desk_stereo_on())) return 0;
    /* Pass-list stereo: the first draw of a scene pass opens the recording,
       and every draw while it is open is issued ONCE -- drawn for eye 0 and
       recorded for eye 1 in the same call.  Nothing here duplicates. */
    if (g_dup_mode == DUP_SCENE && g_vrcfg.stereo_pass_list &&
        !vr_screen_mode()) {
        if (!g_pl_rec && !g_pl_done) pl_begin();
        if (g_pl_rec) { g_pl_draws++; return 0; }
    }
    return 1;
}

/* Defined below, beside the glScissor hook they belong with. */
#define MAXEYE_CACHE 4

static void eye_scissor(int eye, const float *m);
static void eye_clip_planes_off(void);
static void restore_scissor(void);

/* Install one eye's viewport, and for a scene pass its projection too.  A 2D
   pass keeps the engine's own ortho and only changes viewport: its layout is
   in pixels and must land identically in each half. */
/* The eye projection depends on the eye and on the pass's near/far planes,
   and on nothing else -- not on which object is being drawn. It was being
   rebuilt once per eye per draw: 1180 times a frame at 590 draws, every one
   of them identical. Cache it against exactly what it depends on. */
static struct {
    int    valid;
    double near_, far_;
    float  m[16];
} g_eyeproj[MAXEYE_CACHE];

static void eye_projection_cached(int eye, double zn, double zf, float *out) {
    if (eye >= 0 && eye < MAXEYE_CACHE) {
        if (g_eyeproj[eye].valid && g_eyeproj[eye].near_ == zn &&
            g_eyeproj[eye].far_ == zf) {
            memcpy(out, g_eyeproj[eye].m, sizeof(float) * 16);
            return;
        }
        vr_eye_projection(eye, zn, zf, out);
        memcpy(g_eyeproj[eye].m, out, sizeof(float) * 16);
        g_eyeproj[eye].near_ = zn;
        g_eyeproj[eye].far_  = zf;
        g_eyeproj[eye].valid = 1;
        return;
    }
    vr_eye_projection(eye, zn, zf, out);
}

/* The head moves every frame, so the cached matrices are good for one frame
   only. Called once per swap. */
static void eye_projection_cache_flush(void) {
    int i;
    for (i = 0; i < MAXEYE_CACHE; i++) g_eyeproj[i].valid = 0;
}

/* The duplication is 4.0 ms of an 11.5 ms frame and I have been guessing
   at which part. Time the two helpers every path shares; whatever is left
   over is the cost of actually issuing the draw a second time, which is
   irreducible. */
static LONGLONG g_t_eyestate, g_t_restore;
static unsigned g_n_eyestate, g_n_restore;

static void eye_state(int eye) {
    LARGE_INTEGER _a, _b;
    int x, y, w, h;
    float m[16];
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&_a);
    if (desk_stereo_on()) {
        /* Half the engine's viewport, the engine's projection: the same
           calls the headset path makes, a picture nobody needs to look at. */
        int hw = g_eng_vp[2] / 2;
        g_internal = 1;
        o_glViewport(g_eng_vp[0] + eye * hw, g_eng_vp[1], hw, g_eng_vp[3]);
        o_glMatrixMode(GL_PROJECTION);
        o_glLoadMatrixf(g_eng_proj);
        o_glMatrixMode(g_mode);
        g_internal = 0;
        if (g_vrcfg.dup_profile) QueryPerformanceCounter(&_b);
        if (g_vrcfg.dup_profile) g_t_eyestate += _b.QuadPart - _a.QuadPart;
        g_n_eyestate++;
        (void)x; (void)y; (void)w; (void)h; (void)m;
        return;
    }
    vr_eye_viewport(eye, &x, &y, &w, &h);
    g_internal = 1;
    o_glViewport(x, y, w, h);
    if (g_dup_mode == DUP_SCENE && vr_screen_mode()) {
        /* Keep the ENGINE's own frustum exactly and only slide the camera
           sideways.  Anything else -- a wider frustum, or a frustum rotated by
           the head -- asks for world the engine has already culled, and no
           setting can conjure it back.  The head instead looks around the
           window that this image is presented on. */
        float o = vr_eye_offset_world(eye);
        float cv = vr_converge_world();
        memcpy(m, g_eng_proj, sizeof(m));
        m[12] = -o * m[0];                       /* camera moved to x = o */
        if (cv > 1.0f) m[8] = -m[0] * o / cv;    /* eyes agree at cv */
        o_glMatrixMode(GL_PROJECTION);
        o_glLoadMatrixf(m);
        o_glMatrixMode(g_mode);
        eye_scissor(eye, NULL);
    } else if (g_dup_mode == DUP_SCENE) {
        /* Push OUR far plane out without touching the engine's.  If the
           engine is still submitting distant geometry and it was the
           projection clipping it, this alone reveals it -- and it costs one
           multiply rather than another memory hunt.  If nothing changes, the
           engine really is culling by distance and the value has to be found. */
        eye_projection_cached(eye, g_near,
                              g_far * (g_vrcfg.draw_distance > 1.0f
                                       ? g_vrcfg.draw_distance : 1.0f), m);
        o_glMatrixMode(GL_PROJECTION);
        o_glLoadMatrixf(m);
        o_glMatrixMode(g_mode);
        eye_scissor(eye, NULL);
    } else if (g_dup_mode == DUP_HUD && vr_screen_mode()) {
        eye_scissor(eye, NULL);
        /* Nothing to do but the viewport.  The eye image is stretched onto the
           window, which undoes the half-width squash exactly, and identical 2D
           in both eyes lands on the window plane and fuses. */
    } else if (g_dup_mode == DUP_HUD) {
        /* Place the engine's 2D canvas as a panel of a set angular size with
           its OWN aspect ratio.  Scaling it to the eye buffer instead stretched
           it nearly four times vertically and spread it across the whole field
           of view, which no amount of convergence can make fusable. */
        float canvas_aspect = (g_hud_h > 0.0f) ? (g_hud_w / g_hud_h) : 1.7778f;
        /* Only the in-game screen is fixed in space.  A menu has no world
           point to stay attached to, and fixing it would mean being able
           to lose it behind you. */
        /* Menus used to be excluded from world locking, on the grounds
           that a menu has no world point to stay attached to and could be
           lost behind you.  It anchors to the head now and re-anchors if you
           turn away from it, so it cannot be lost -- and a menu welded to the
           face is one whose corners can never be looked at. */
        vr_hud_projection(eye, canvas_aspect,
                          g_menu_up ? g_vrcfg.menu_size : g_vrcfg.hud_size,
                          1, m);
        o_glMatrixMode(GL_PROJECTION);
        o_glLoadMatrixf(m);
        o_glMatrixMode(g_mode);
        eye_scissor(eye, m);
    }
    g_internal = 0;
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&_b);
    if (g_vrcfg.dup_profile) g_t_eyestate += _b.QuadPart - _a.QuadPart;
    g_n_eyestate++;
}

/* Put back exactly what the engine believes is set, so anything of its own
   that reads the matrix or the viewport still gets its own answer. */
static void restore_engine_state(void) {
    LARGE_INTEGER _a, _b;
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&_a);
    g_internal = 1;
    if (g_dup_mode != DUP_NONE) {
        o_glMatrixMode(GL_PROJECTION);
        o_glLoadMatrixf(g_eng_proj);
        o_glMatrixMode(g_mode);
    }
    o_glViewport(g_eng_vp[0], g_eng_vp[1], g_eng_vp[2], g_eng_vp[3]);
    restore_scissor();
    g_internal = 0;
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&_b);
    if (g_vrcfg.dup_profile) g_t_restore += _b.QuadPart - _a.QuadPart;
    g_n_restore++;
}

/* ---- one switch per draw -------------------------------------------------
   Draw N: eye 0 then eye 1.  Draw N+1: eye 1 -- already loaded, no switch --
   then eye 0.  The engine's own projection and viewport are put back only
   when it is about to look at them (dup_flush, called from every hook that
   can see them).  Each eye still receives every draw in the engine's order;
   only the interleaving between the eyes changes, and depth testing does not
   care about that. */
static int dup_defer_ok(int n) {
    return g_vrcfg.dup_alternate && g_dup_mode == DUP_SCENE && n > 1 &&
           !g_panel_capture;
}
static void dup_eye(int eye) {
    if (g_dup_loaded == eye) return;
    eye_state(eye);
    g_dup_loaded = eye;
    g_dup_tid = GetCurrentThreadId();
    g_dup_switches++;
}
static int dup_first(int n) {
    if (dup_defer_ok(n) && g_dup_loaded >= 0 && g_dup_loaded < n)
        return g_dup_loaded;
    return 0;
}
static void dup_after(int n) {
    if (dup_defer_ok(n)) return;        /* leave the last eye loaded */
    restore_engine_state();
    g_dup_loaded = -1;
}
/* The hooked calls bypass the thunks, so they have to bump the same counter
   by hand or a state change between two draws looks like no change at all. */
#define STATE_TOUCH() do { if (!g_internal) g_real_calls++; } while (0)
static void dup_flush(void) {
    if (g_dup_loaded < 0) return;
    /* Another thread's context is not the one this state is loaded in. */
    if (GetCurrentThreadId() != g_dup_tid) return;
    restore_engine_state();
    g_dup_loaded = -1;
    g_dup_flushes++;
}
#define DUP_LOOP(n, DRAW) do {                                            \
        int _k, _f = dup_first(n);                                        \
        for (_k = 0; _k < (n); _k++) {                                    \
            dup_eye((_f + _k) % (n));                                     \
            DRAW;                                                         \
        }                                                                 \
        dup_after(n);                                                     \
    } while (0)

/* ---- pass-list stereo --------------------------------------------------- */
static int pl_procs_ready(void) {
    if (g_pl_failed) return 0;
    if (g_pl_base) return 1;
    pl_pushattr  = (void (APIENTRY *)(GLbitfield))proxy_real_proc("glPushAttrib");
    pl_popattr   = (void (APIENTRY *)(void))proxy_real_proc("glPopAttrib");
    pl_pushcattr = (void (APIENTRY *)(GLbitfield))proxy_real_proc("glPushClientAttrib");
    pl_popcattr  = (void (APIENTRY *)(void))proxy_real_proc("glPopClientAttrib");
    pl_pushm     = (void (APIENTRY *)(void))proxy_real_proc("glPushMatrix");
    pl_popm      = (void (APIENTRY *)(void))proxy_real_proc("glPopMatrix");
    pl_geterror  = (GLenum (APIENTRY *)(void))proxy_real_proc("glGetError");
    pl_getcurrent = (HGLRC (WINAPI *)(void))proxy_real_proc("wglGetCurrentContext");
    /* The shader binding is the one piece of state PushAttrib does not
       cover.  Optional: without it the replay still starts from the right
       state unless a pass changes program mid-way and depends on the one
       bound before it. */
    if (o_wglGetProcAddress) {
        pl_getprog = (unsigned (APIENTRY *)(GLenum))
                     o_wglGetProcAddress("glGetHandleARB");
        pl_useprog = (void (APIENTRY *)(unsigned))
                     o_wglGetProcAddress("glUseProgramObjectARB");
        if (!pl_getprog || !pl_useprog) { pl_getprog = NULL; pl_useprog = NULL; }
    }
    if (!pl_pushattr || !pl_popattr || !pl_pushcattr || !pl_popcattr ||
        !pl_pushm || !pl_popm || !o_glGenLists || !o_glNewList ||
        !o_glEndList || !o_glCallList) {
        kv_log("PASS LIST: GL entry points missing; staying with per-draw "
               "stereo");
        g_pl_failed = 1;
        return 0;
    }
    g_pl_base = o_glGenLists(PL_MAXSEG);
    if (!g_pl_base) {
        kv_log("PASS LIST: glGenLists refused; staying with per-draw stereo");
        g_pl_failed = 1;
        return 0;
    }
    kv_log("PASS LIST: stereo by whole-pass display list is ON -- each scene "
           "pass is recorded while eye 0 draws and replayed once for eye 1. "
           "stereo_pass_list = 0 goes back to per-draw duplication; F10 "
           "toggles it live with panel_flat_test.");
    return 1;
}

static void pl_open_segment(void) {
    if (g_pl_nseg >= PL_MAXSEG) {
        /* Out of room.  Replay what there is and let the rest of this pass
           go the per-draw way; g_pl_done keeps it from restarting. */
        g_pl_overflow++;
        pl_end();
        g_pl_done = 1;
        return;
    }
    g_internal = 1;
    o_glNewList(g_pl_base + g_pl_nseg, GL_COMPILE_AND_EXECUTE_);
    g_internal = 0;
    g_pl_open = 1;
}

static void pl_close_segment(void) {
    if (!g_pl_open) return;
    g_internal = 1;
    o_glEndList();
    g_internal = 0;
    g_pl_open = 0;
    g_pl_nseg++;
}

/* At the first draw of a scene pass: eye 0's state, then save everything the
   replay has to start from, then open the recording.  The save comes AFTER
   the eye state on purpose -- the pop restores it and eye 1's state then
   overrides it -- and BEFORE the list, so none of it is recorded. */
static void pl_begin(void) {
    if (!pl_procs_ready() || vr_eye_count() < 2) { g_pl_done = 1; return; }
    eye_state(0);
    g_internal = 1;
    g_pl_prog_ok = 0;
    if (pl_getprog) { g_pl_prog = pl_getprog(GL_PROGRAM_OBJECT_ARB_); g_pl_prog_ok = 1; }
    pl_pushattr(GL_ALL_ATTRIB_BITS_);
    pl_pushcattr(GL_CLIENT_ALL_ATTRIB_BITS_);
    o_glMatrixMode(GL_PROJECTION); pl_pushm();
    o_glMatrixMode(GL_MODELVIEW);  pl_pushm();
    o_glMatrixMode(g_mode);
    g_internal = 0;
    g_pl_tid = GetCurrentThreadId();
    g_pl_nseg = 0; g_pl_open = 0; g_pl_draws = 0; g_pl_reopen = 0;
    g_pl_rec = 1;
    pl_open_segment();
}

/* The pass is over: back to the saved state, eye 1's state on top of it,
   call the recording, and give the engine its own projection and viewport
   back.  The GL state ends exactly where the engine left it, because the
   replay applies the same commands to the same starting state. */
static void pl_end(void) {
    LARGE_INTEGER t0, t1;
    int eye, n, i;
    GLenum err;
    if (!g_pl_rec) return;
    if (GetCurrentThreadId() != g_pl_tid) return;
    pl_close_segment();
    QueryPerformanceCounter(&t0);
    g_internal = 1;
    o_glMatrixMode(GL_MODELVIEW);  pl_popm();
    o_glMatrixMode(GL_PROJECTION); pl_popm();
    pl_popcattr();
    pl_popattr();
    if (g_pl_prog_ok && pl_useprog) pl_useprog(g_pl_prog);
    g_internal = 0;
    n = vr_eye_count();
    for (eye = 1; eye < n; eye++) {
        eye_state(eye);
        g_internal = 1;
        for (i = 0; i < g_pl_nseg; i++) o_glCallList(g_pl_base + i);
        g_internal = 0;
    }
    restore_engine_state();
    if (pl_geterror) {
        g_internal = 1;
        err = pl_geterror();
        g_internal = 0;
        if (err) {
            g_pl_errors++;
            if (g_pl_errors <= 3)
                kv_log("PASS LIST: GL error 0x%X after replaying a pass of %d "
                       "segments / %d draws", err, g_pl_nseg, g_pl_draws);
        }
    }
    QueryPerformanceCounter(&t1);
    g_pl_t_replay += t1.QuadPart - t0.QuadPart;
    g_pl_passes++;
    g_pl_segs += (unsigned)g_pl_nseg;
    g_pl_draws_total += (unsigned)g_pl_draws;
    g_s.dup_scene += (unsigned)g_pl_draws;
    g_pl_rec = 0; g_pl_nseg = 0; g_pl_draws = 0; g_pl_open = 0;
}

/* ---- immediate-mode blocks as arrays -------------------------------------
   Between glBegin and glEnd nothing is forwarded.  Each glVertex appends a
   record carrying the current colour, normal and texture coordinates; at
   glEnd the block is one glDrawArrays per eye from that buffer. */
#define GL_VERTEX_ARRAY_         0x8074
#define GL_NORMAL_ARRAY_         0x8075
#define GL_COLOR_ARRAY_          0x8076
#define GL_INDEX_ARRAY_          0x8077
#define GL_TEXTURE_COORD_ARRAY_  0x8078
#define GL_EDGE_FLAG_ARRAY_      0x8079
#define GL_TEXTURE0_ARB_         0x84C0
#define GL_CURRENT_NORMAL_       0x0B02
#define GL_CURRENT_TEXTURE_COORDS_ 0x0B03
#define BA_UNITS 1
typedef struct {
    float pos[3];
    float nrm[3];
    float tc[BA_UNITS][2];
    unsigned char col[4];
} BaVert;                                   /* 36 bytes */
static BaVert  *g_ba_buf;
static unsigned g_ba_cap, g_ba_n;
static BaVert   g_ba_cur;                   /* the current attributes */
static int      g_ba_on;                    /* inside a captured block */
static GLenum   g_ba_mode;
static unsigned g_ba_units;                 /* texture units set in block */
static int      g_ba_set_col, g_ba_set_nrm, g_ba_set_tc[BA_UNITS];
static int      g_ba_col_is_ub;
static float    g_ba_col4f[4];
static int      g_ba_failed;
static unsigned g_ba_blocks, g_ba_verts, g_ba_mtc_calls, g_ba_lost,
                g_ba_maxverts;
static void (APIENTRY *ba_enablecs)(GLenum);
static void (APIENTRY *ba_disablecs)(GLenum);
static void (APIENTRY *ba_vp)(GLint, GLenum, GLsizei, const GLvoid *);
static void (APIENTRY *ba_np)(GLenum, GLsizei, const GLvoid *);
static void (APIENTRY *ba_cp)(GLint, GLenum, GLsizei, const GLvoid *);
static void (APIENTRY *ba_tp)(GLint, GLenum, GLsizei, const GLvoid *);
static void (APIENTRY *ba_pushc)(GLbitfield);
static void (APIENTRY *ba_popc)(void);
static void (APIENTRY *ba_clientactive)(GLenum);
/* The engine's multitexture entry points, wrapped through
   wglGetProcAddress. */
static void (APIENTRY *ba_mtc2f)(GLenum, GLfloat, GLfloat);
static void (APIENTRY *ba_mtc2fv)(GLenum, const GLfloat *);
static void (APIENTRY *ba_mtc4f)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *ba_mtc4fv)(GLenum, const GLfloat *);

static int ba_procs_ready(void) {
    if (g_ba_failed) return 0;
    if (g_ba_buf) return 1;
    ba_enablecs  = (void (APIENTRY *)(GLenum))proxy_real_proc("glEnableClientState");
    ba_disablecs = (void (APIENTRY *)(GLenum))proxy_real_proc("glDisableClientState");
    ba_vp = (void (APIENTRY *)(GLint, GLenum, GLsizei, const GLvoid *))
            proxy_real_proc("glVertexPointer");
    ba_np = (void (APIENTRY *)(GLenum, GLsizei, const GLvoid *))
            proxy_real_proc("glNormalPointer");
    ba_cp = (void (APIENTRY *)(GLint, GLenum, GLsizei, const GLvoid *))
            proxy_real_proc("glColorPointer");
    ba_tp = (void (APIENTRY *)(GLint, GLenum, GLsizei, const GLvoid *))
            proxy_real_proc("glTexCoordPointer");
    ba_pushc = (void (APIENTRY *)(GLbitfield))proxy_real_proc("glPushClientAttrib");
    ba_popc  = (void (APIENTRY *)(void))proxy_real_proc("glPopClientAttrib");
    if (o_wglGetProcAddress && !ba_clientactive)
        ba_clientactive = (void (APIENTRY *)(GLenum))
                          o_wglGetProcAddress("glClientActiveTextureARB");
    if (!ba_enablecs || !ba_disablecs || !ba_vp || !ba_np || !ba_cp ||
        !ba_tp || !ba_pushc || !ba_popc || !o_glDrawArrays || !o_glColor4ub ||
        !o_glColor4f || !o_glNormal3f || !o_glTexCoord2f) {
        kv_log("BLOCK ARRAYS: entry points missing; blocks stay on the "
               "display-list path");
        g_ba_failed = 1;
        return 0;
    }
    g_ba_cap = 65536;
    g_ba_buf = (BaVert *)malloc(sizeof(BaVert) * g_ba_cap);
    if (!g_ba_buf) { g_ba_failed = 1; return 0; }
    g_ba_cur.tc[0][0] = 0.0f; g_ba_cur.tc[0][1] = 0.0f;
    kv_log("BLOCK ARRAYS: immediate-mode blocks are captured on the CPU and "
           "drawn as one array draw per eye (%s multitexture units). "
           "block_arrays = 0 forwards every call and compiles a list per "
           "block instead; F8 toggles it live with panel_flat_test.",
           ba_clientactive ? "with" : "WITHOUT");
    return 1;
}

static unsigned char ba_f2ub(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static void ba_begin(GLenum mode) {
    GLfloat c[4], n3[3], t[4];
    int u;
    g_internal++;
    o_glGetFloatv(GL_CURRENT_COLOR, c);
    o_glGetFloatv(GL_CURRENT_NORMAL_, n3);
    o_glGetFloatv(GL_CURRENT_TEXTURE_COORDS_, t);
    g_internal--;
    g_ba_cur.col[0] = ba_f2ub(c[0]); g_ba_cur.col[1] = ba_f2ub(c[1]);
    g_ba_cur.col[2] = ba_f2ub(c[2]); g_ba_cur.col[3] = ba_f2ub(c[3]);
    g_ba_cur.nrm[0] = n3[0]; g_ba_cur.nrm[1] = n3[1]; g_ba_cur.nrm[2] = n3[2];
    g_ba_cur.tc[0][0] = t[0]; g_ba_cur.tc[0][1] = t[1];
    g_ba_mode = mode;
    g_ba_n = 0;
    g_ba_units = 1;
    g_ba_set_col = 0; g_ba_set_nrm = 0;
    for (u = 0; u < BA_UNITS; u++) g_ba_set_tc[u] = 0;
    g_ba_on = 1;
}

static void ba_push(float x, float y, float z) {
    BaVert *v;
    if (g_ba_n >= g_ba_cap) {
        unsigned ncap = g_ba_cap * 2;
        BaVert *nb = (BaVert *)realloc(g_ba_buf, sizeof(BaVert) * ncap);
        if (!nb) { g_ba_lost++; return; }
        g_ba_buf = nb; g_ba_cap = ncap;
    }
    v = &g_ba_buf[g_ba_n++];
    *v = g_ba_cur;
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
}

static void ba_tc(GLenum target, float s, float t, float r, float q) {
    unsigned u = (unsigned)(target - GL_TEXTURE0_ARB_);
    g_ba_mtc_calls++;
    (void)r; (void)q;
    if (u >= BA_UNITS) { g_ba_lost++; return; }
    g_ba_cur.tc[u][0] = s; g_ba_cur.tc[u][1] = t;
    g_ba_units |= 1u << u;
    g_ba_set_tc[u] = 1;
}

static void (APIENTRY *sf_active_next)(GLenum);
static void APIENTRY w_glActiveTextureARB(GLenum unit) {
    sf_drop();
    if (!g_internal && !g_engine_list) {
        int u = (int)unit - (int)GL_TEXTURE0_ARB_;
        g_nx.unit = (signed char)((u >= 0 && u < NX_UNITS) ? u : -1);
    }
    if (sf_active_next) sf_active_next(unit);
}
static void APIENTRY w_glMultiTexCoord2fARB(GLenum tg, GLfloat s, GLfloat t) {
    if (g_ba_on && !g_internal) { ba_tc(tg, s, t, 0.0f, 1.0f); return; }
    ba_mtc2f(tg, s, t);
}
static void APIENTRY w_glMultiTexCoord2fvARB(GLenum tg, const GLfloat *v) {
    if (g_ba_on && !g_internal) { ba_tc(tg, v[0], v[1], 0.0f, 1.0f); return; }
    ba_mtc2fv(tg, v);
}
static void APIENTRY w_glMultiTexCoord4fARB(GLenum tg, GLfloat s, GLfloat t,
                                            GLfloat r, GLfloat q) {
    if (g_ba_on && !g_internal) { ba_tc(tg, s, t, r, q); return; }
    ba_mtc4f(tg, s, t, r, q);
}
static void APIENTRY w_glMultiTexCoord4fvARB(GLenum tg, const GLfloat *v) {
    if (g_ba_on && !g_internal) { ba_tc(tg, v[0], v[1], v[2], v[3]); return; }
    ba_mtc4fv(tg, v);
}

/* glEnd for a captured block: draw it per eye, then leave the engine's
   current attributes where its own calls would have left them. */
static void ba_end(void) {
    LARGE_INTEGER t0, t1;
    int n, u;
    const GLsizei stride = (GLsizei)sizeof(BaVert);
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&t0);
    g_ba_on = 0;
    if (g_ba_n > 0) {
        g_internal = 1;
        ba_pushc(GL_CLIENT_ALL_ATTRIB_BITS_);
        ba_enablecs(GL_VERTEX_ARRAY_);
        ba_vp(3, GL_FLOAT, stride, g_ba_buf[0].pos);
        ba_enablecs(GL_NORMAL_ARRAY_);
        ba_np(GL_FLOAT, stride, g_ba_buf[0].nrm);
        ba_enablecs(GL_COLOR_ARRAY_);
        ba_cp(4, GL_UNSIGNED_BYTE, stride, g_ba_buf[0].col);
        ba_disablecs(GL_INDEX_ARRAY_);
        ba_disablecs(GL_EDGE_FLAG_ARRAY_);
        if (ba_clientactive) ba_clientactive(GL_TEXTURE0_ARB_);
        ba_enablecs(GL_TEXTURE_COORD_ARRAY_);
        ba_tp(2, GL_FLOAT, stride, g_ba_buf[0].tc[0]);
        n = pass_eye_count();
        DUP_LOOP(n, o_glDrawArrays(g_ba_mode, 0, (GLsizei)g_ba_n));
        g_internal = 1;
        ba_popc();
        g_internal = 0;
        g_ba_verts += g_ba_n;
        if (g_ba_n > g_ba_maxverts) g_ba_maxverts = g_ba_n;
        if (g_dup_mode == DUP_SCENE) g_s.dup_scene++; else g_s.dup_hud++;
    }
    /* What the engine's own calls would have left as current state. */
    if (g_ba_set_col) {
        if (g_ba_col_is_ub)
            o_glColor4ub(g_ba_cur.col[0], g_ba_cur.col[1], g_ba_cur.col[2],
                         g_ba_cur.col[3]);
        else
            o_glColor4f(g_ba_col4f[0], g_ba_col4f[1], g_ba_col4f[2],
                        g_ba_col4f[3]);
    }
    if (g_ba_set_nrm)
        o_glNormal3f(g_ba_cur.nrm[0], g_ba_cur.nrm[1], g_ba_cur.nrm[2]);
    if (g_ba_set_tc[0])
        o_glTexCoord2f(g_ba_cur.tc[0][0], g_ba_cur.tc[0][1]);
    (void)u;
    g_ba_blocks++;
    g_n_lists++;
    if (g_vrcfg.dup_profile) {
        QueryPerformanceCounter(&t1);
        g_t_list += t1.QuadPart - t0.QuadPart;
    }
}

/* ---- the redundant-state filter ---------------------------------------
   The engine sets everything from scratch per object.  Keep what it last
   set and drop the call when it asks for the same thing again.

   Safe across glCallList because the engine's lists were measured and hold
   only glArrayElement and the client-array pointers -- nothing that
   changes what is cached here.  Everything that COULD change it behind our
   back drops the cache: the top of each frame (which covers the port's own
   GL at swap time), push/pop attrib, a context switch, a texture unit
   change, and compiling a list. */
#define SF_SLOTS 256
/* GL enum names collide badly in their low bits: GL_TEXTURE_ENV_MODE
   (0x2200) and GL_SOURCE0_RGB (0x8580) share the bottom six. A
   collision is safe -- it reads as "changed" and the call goes
   through -- but it makes the filter look useless when it is not.
   Fold the high byte in so the two ranges separate. */
#define SF_KEY(p) ((unsigned)(((p) ^ ((p) >> 8)) & (SF_SLOTS - 1)))
/* The two 256-entry tables are invalidated by GENERATION, not by memset:
   an entry is valid only if it was written in the current generation, and
   sf_drop() starts a new one. The engine calls glActiveTexture around
   nearly every draw and each one drops the cache -- as 14 KB of memset that
   was 10% of the render thread (sampler, 24 Sep 2026). Same answers, O(1). */
typedef struct { GLenum a, b; GLint i; GLfloat f; unsigned valid; } SfEnt;
static SfEnt   g_sf_env[SF_SLOTS];      /* glTexEnvi / glTexEnvf */
static struct { GLenum face, pname; GLfloat v[4]; GLint iv;
                unsigned valid; } g_sf_mat[SF_SLOTS];
static unsigned g_sf_gen = 1;           /* 0 is never current: a zeroed entry is invalid */
#define SF_OK(e) ((e).valid == g_sf_gen)
static struct { GLenum func; GLclampf ref; unsigned char valid; } g_sf_alpha;
static struct { GLboolean f; unsigned char valid; } g_sf_dmask;
static struct { GLenum m; unsigned char valid; } g_sf_shade, g_sf_dfunc;
static struct { GLenum target; GLuint tex; unsigned char valid; } g_sf_bind[4];
static unsigned g_sf_skipped, g_sf_seen;

/* ---- group census -------------------------------------------------------
   A running signature of the tracked GL state, and per-frame sets of the
   distinct signatures and distinct display lists the world pass draws with.

   The signature is Zobrist: sig = XOR over slots of hash(slot, value), so a
   change XORs the old value out and the new one in. Exact, order-independent,
   O(1) per state call -- which matters at 47,000 state calls a frame. */
#define GK_SLOTS 1024
static unsigned      g_gk_val[GK_SLOTS];
static unsigned char g_gk_have[GK_SLOTS];
static unsigned      g_gk_sig;

static unsigned gk_mix(unsigned slot, unsigned val) {
    unsigned h = 2166136261u;
    h = (h ^ slot) * 16777619u;
    h = (h ^ val)  * 16777619u;
    h ^= h >> 15;
    return h ? h : 1u;
}
static void gk_set(unsigned slot, unsigned val) {
    if (slot >= GK_SLOTS) return;
    if (g_gk_have[slot] && g_gk_val[slot] == val) return;
    if (g_gk_have[slot]) g_gk_sig ^= gk_mix(slot, g_gk_val[slot]);
    g_gk_sig ^= gk_mix(slot, val);
    g_gk_val[slot] = val;
    g_gk_have[slot] = 1;
}
static unsigned gk_f(GLfloat f) { unsigned u; memcpy(&u, &f, 4); return u; }
static unsigned gk_v4(const GLfloat *v) {
    unsigned h = 2166136261u; int i;
    for (i = 0; i < 4; i++) h = (h ^ gk_f(v[i])) * 16777619u;
    return h;
}
/* Slot ranges. texenv i and f share a slot deliberately: they set the same
   GL state, so a draw cannot tell which call put the value there. */
#define GKS_ENV(p)  (SF_KEY(p))              /* 0   .. 255 */
#define GKS_MAT(p)  (256u + SF_KEY(p))       /* 256 .. 511 */
#define GKS_BIND(i) (512u + (unsigned)(i))   /* 512 .. 515 */
#define GKS_AFUNC   600u
#define GKS_AREF    601u
#define GKS_DMASK   602u
#define GKS_SHADE   603u
#define GKS_DFUNC   604u
#define GKS_BSRC    605u
#define GKS_BDST    606u
#define GKS_CAP(i)  (610u + (unsigned)(i))   /* 610 .. 616 */

/* The enables the engine actually toggles around draws. Anything not here is
   simply absent from the key, which is why the report calls itself a lower
   bound. */
static int gk_cap_slot(GLenum cap) {
    switch (cap) {
        case 0x0BE2: return 0;   /* GL_BLEND       */
        case 0x0BC0: return 1;   /* GL_ALPHA_TEST  */
        case 0x0DE1: return 2;   /* GL_TEXTURE_2D  */
        case 0x0B50: return 3;   /* GL_LIGHTING    */
        case 0x0B44: return 4;   /* GL_CULL_FACE   */
        case 0x0B71: return 5;   /* GL_DEPTH_TEST  */
        case 0x0B60: return 6;   /* GL_FOG         */
        default:     return -1;
    }
}

/* Per-frame sets. Open addressing, power-of-two, linear probe. */
#define GC_CAP 16384
typedef struct { unsigned key, n; } GcEnt;
static GcEnt    g_gc_state[GC_CAP], g_gc_list[GC_CAP];
static unsigned g_gc_state_n, g_gc_list_n, g_gc_draws, g_gc_full;
/* Window accumulators, averaged over the frames between two reports. */
static double   g_gcw_states, g_gcw_lists, g_gcw_draws, g_gcw_single;
static unsigned g_gcw_max, g_gcw_frames, g_gcw_listmax;

static void gc_add(GcEnt *t, unsigned *count, unsigned key) {
    unsigned i, tries;
    if (!key) key = 1u;
    i = (key * 2654435761u) & (GC_CAP - 1u);
    for (tries = 0; tries < GC_CAP; tries++) {
        if (!t[i].n)        { t[i].key = key; t[i].n = 1; (*count)++; return; }
        if (t[i].key == key) { t[i].n++; return; }
        i = (i + 1u) & (GC_CAP - 1u);
    }
    g_gc_full++;                      /* saturated: the report says so */
}

/* One world-pass draw, with whatever state is in force right now.
   listkey distinguishes a real display list from the other draw paths. */
static void gc_note_draw(unsigned listkey) {
    gc_add(g_gc_state, &g_gc_state_n, g_gk_sig);
    gc_add(g_gc_list,  &g_gc_list_n,  listkey);
    g_gc_draws++;
}

static void gc_frame_end(void) {
    unsigned i, single = 0, mx = 0, lmx = 0;
    if (!g_gc_draws) return;
    for (i = 0; i < GC_CAP; i++) {
        if (g_gc_state[i].n) {
            if (g_gc_state[i].n == 1) single++;
            if (g_gc_state[i].n > mx)  mx = g_gc_state[i].n;
        }
        if (g_gc_list[i].n > lmx) lmx = g_gc_list[i].n;
    }
    g_gcw_states += (double)g_gc_state_n;
    g_gcw_lists  += (double)g_gc_list_n;
    g_gcw_draws  += (double)g_gc_draws;
    g_gcw_single += (double)single;
    if (mx  > g_gcw_max)     g_gcw_max = mx;
    if (lmx > g_gcw_listmax) g_gcw_listmax = lmx;
    g_gcw_frames++;
    memset(g_gc_state, 0, sizeof(g_gc_state));
    memset(g_gc_list,  0, sizeof(g_gc_list));
    g_gc_state_n = 0; g_gc_list_n = 0; g_gc_draws = 0;
}

static void sf_drop(void) {
    if (++g_sf_gen == 0) {                /* wrapped: really clear, once */
        memset(g_sf_env, 0, sizeof(g_sf_env));
        memset(g_sf_mat, 0, sizeof(g_sf_mat));
        g_sf_gen = 1;
    }
    g_sf_alpha.valid = 0; g_sf_dmask.valid = 0;
    g_sf_shade.valid = 0; g_sf_dfunc.valid = 0;
    memset(g_sf_bind, 0, sizeof(g_sf_bind));
}
/* The cache is TRACKED whenever it safely can be -- the engine's own calls,
   outside a list being compiled -- so the report can say what a filter
   would save before anyone turns it on. Only the SKIP is gated on
   state_filter. */
static unsigned g_mc_state_in_list;   /* stage 1: state recorded INTO a list */
static int sf_on(void) {
    if (!g_internal && g_engine_list) g_mc_state_in_list++;
    return !g_internal && !g_engine_list;
}

void APIENTRY hk_glTexEnvi(GLenum target, GLenum pname, GLint param) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list && target == GL_TEXTURE_ENV &&
        pname == GL_TEXTURE_ENV_MODE) {
        if (g_nx.unit >= 0) g_nx.env[g_nx.unit] = (signed char)(param == GL_MODULATE);
        else { int u; for (u = 0; u < NX_UNITS; u++) g_nx.env[u] = -1; }
    }
    if (sf_on()) {
        SfEnt *e = &g_sf_env[SF_KEY(pname)];
        g_sf_seen++;
        if (SF_OK(*e) && e->a == target && e->b == pname && e->i == param) {
            g_sf_skipped++;
            if (g_vrcfg.state_filter) return;
        }
        e->a = target; e->b = pname; e->i = param; e->f = 0.0f; e->valid = g_sf_gen;
        gk_set(GKS_ENV(pname), (unsigned)param);
    }
    o_glTexEnvi(target, pname, param);
}
void APIENTRY hk_glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list && target == GL_TEXTURE_ENV &&
        pname == GL_TEXTURE_ENV_MODE) {
        if (g_nx.unit >= 0)
            g_nx.env[g_nx.unit] = (signed char)(param == (GLfloat)GL_MODULATE);
        else { int u; for (u = 0; u < NX_UNITS; u++) g_nx.env[u] = -1; }
    }
    if (sf_on()) {
        SfEnt *e = &g_sf_env[SF_KEY(pname)];
        g_sf_seen++;
        if (SF_OK(*e) && e->a == target && e->b == pname && e->f == param &&
            e->i == 0) {
            g_sf_skipped++;
            if (g_vrcfg.state_filter) return;
        }
        e->a = target; e->b = pname; e->f = param; e->i = 0; e->valid = g_sf_gen;
        gk_set(GKS_ENV(pname), gk_f(param));
    }
    o_glTexEnvf(target, pname, param);
}
void APIENTRY hk_glAlphaFunc(GLenum func, GLclampf ref) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list) {
        g_nx.af_ok = 1; g_nx.af_func = func; g_nx.af_ref = (float)ref;
    }
    if (sf_on()) {
        g_sf_seen++;
        if (g_sf_alpha.valid && g_sf_alpha.func == func &&
            g_sf_alpha.ref == ref) { g_sf_skipped++; if (g_vrcfg.state_filter) return; }
        g_sf_alpha.func = func; g_sf_alpha.ref = ref; g_sf_alpha.valid = 1;
        gk_set(GKS_AFUNC, (unsigned)func);
        gk_set(GKS_AREF,  gk_f((GLfloat)ref));
    }
    o_glAlphaFunc(func, ref);
}
void APIENTRY hk_glDepthMask(GLboolean flag) {
    STATE_TOUCH();
    if (sf_on()) {
        g_sf_seen++;
        if (g_sf_dmask.valid && g_sf_dmask.f == flag) { g_sf_skipped++; if (g_vrcfg.state_filter) return; }
        g_sf_dmask.f = flag; g_sf_dmask.valid = 1;
        gk_set(GKS_DMASK, (unsigned)flag);
    }
    o_glDepthMask(flag);
}
void APIENTRY hk_glShadeModel(GLenum mode) {
    STATE_TOUCH();
    if (sf_on()) {
        g_sf_seen++;
        if (g_sf_shade.valid && g_sf_shade.m == mode) { g_sf_skipped++; if (g_vrcfg.state_filter) return; }
        g_sf_shade.m = mode; g_sf_shade.valid = 1;
        gk_set(GKS_SHADE, (unsigned)mode);
    }
    o_glShadeModel(mode);
}
void APIENTRY hk_glDepthFunc(GLenum func) {
    STATE_TOUCH();
    if (g_vrcfg.diagnostics && !g_internal && func >= 0x0200 && func <= 0x0207) {
        static unsigned seen;
        if (!(seen & (1u << (func - 0x0200)))) {
            seen |= 1u << (func - 0x0200);
            kv_log("DEPTH FUNC: the engine used 0x%04X for the first time (frame %u, %s)",
                   func, g_frames, g_engine_list ? "inside a list" : "directly");
        }
    }
    if (sf_on()) {
        g_sf_seen++;
        if (g_sf_dfunc.valid && g_sf_dfunc.m == func) { g_sf_skipped++; if (g_vrcfg.state_filter) return; }
        g_sf_dfunc.m = func; g_sf_dfunc.valid = 1;
        gk_set(GKS_DFUNC, (unsigned)func);
    }
    o_glDepthFunc(func);
}
void APIENTRY hk_glMateriali(GLenum face, GLenum pname, GLint param) {
    STATE_TOUCH();
    if (sf_on()) {
        int k = SF_KEY(pname);
        g_sf_seen++;
        if (SF_OK(g_sf_mat[k]) && g_sf_mat[k].face == face &&
            g_sf_mat[k].pname == pname && g_sf_mat[k].iv == param) {
            g_sf_skipped++; if (g_vrcfg.state_filter) return;
        }
        g_sf_mat[k].face = face; g_sf_mat[k].pname = pname;
        g_sf_mat[k].iv = param; g_sf_mat[k].v[0] = 1e30f; g_sf_mat[k].valid = g_sf_gen;
        gk_set(GKS_MAT(pname), (unsigned)param);
    }
    o_glMateriali(face, pname, param);
}
void APIENTRY hk_glMaterialfv(GLenum face, GLenum pname, const GLfloat *v) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list && v && face != GL_BACK &&
        (pname == GL_DIFFUSE || pname == 0x1602 /* AMBIENT_AND_DIFFUSE */)) {
        g_nx.dif_ok = 1; g_nx.dif_a = v[3];
    }
    if (sf_on() && v) {
        int k = SF_KEY(pname);
        g_sf_seen++;
        if (SF_OK(g_sf_mat[k]) && g_sf_mat[k].face == face &&
            g_sf_mat[k].pname == pname &&
            g_sf_mat[k].v[0] == v[0] && g_sf_mat[k].v[1] == v[1] &&
            g_sf_mat[k].v[2] == v[2] && g_sf_mat[k].v[3] == v[3]) {
            g_sf_skipped++; if (g_vrcfg.state_filter) return;
        }
        g_sf_mat[k].face = face; g_sf_mat[k].pname = pname;
        g_sf_mat[k].v[0] = v[0]; g_sf_mat[k].v[1] = v[1];
        g_sf_mat[k].v[2] = v[2]; g_sf_mat[k].v[3] = v[3];
        g_sf_mat[k].iv = 0x7FFFFFFF; g_sf_mat[k].valid = g_sf_gen;
        gk_set(GKS_MAT(pname), gk_v4(v));
    }
    o_glMaterialfv(face, pname, v);
}
void APIENTRY hk_glBindTexture(GLenum target, GLuint texture) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list && target == GL_TEXTURE_2D)
        g_tex0 = (g_nx.unit == 0) ? texture : (g_nx.unit < 0 ? 0xFFFFFFFFu : g_tex0);
    if (sf_on()) {
        int i;
        g_sf_seen++;
        for (i = 0; i < 4; i++) {
            if (g_sf_bind[i].valid && g_sf_bind[i].target == target) {
                if (g_sf_bind[i].tex == texture) { g_sf_skipped++; if (g_vrcfg.state_filter) return; }
                g_sf_bind[i].tex = texture;
                gk_set(GKS_BIND(i), (unsigned)texture);
                o_glBindTextureR(target, texture);
                return;
            }
        }
        for (i = 0; i < 4; i++)
            if (!g_sf_bind[i].valid) {
                g_sf_bind[i].valid = 1; g_sf_bind[i].target = target;
                g_sf_bind[i].tex = texture;
                gk_set(GKS_BIND(i), (unsigned)texture);
                break;
            }
    }
    o_glBindTextureR(target, texture);
}

/* ---- stage 2: the modelview, shadowed in software -----------------------
   glGetFloatv(GL_MODELVIEW_MATRIX) is a pipeline stall and there are 3618
   world draws a frame, so the matrix is tracked here instead.

   Column-major throughout, as GL stores it: m[col*4 + row]. glMultMatrix
   POST-multiplies, C = C * M. */
#define MV_DEPTH 64
static float    g_mv[MV_DEPTH][16];
static int      g_mv_sp;
static unsigned g_mv_overflow, g_mv_underflow;

static void m_ident(float *m) {
    int i;
    for (i = 0; i < 16; i++) m[i] = (i % 5) ? 0.0f : 1.0f;
}
static void m_mul(float *out, const float *a, const float *b) {
    float t[16];
    int c, r, k;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    memcpy(out, t, sizeof(t));
}
/* The OBJECT stack: the same operations, except that the view install is
   replaced by identity. Its top is the object's own matrix, built only from
   the engine's inputs, so it is bit-identical from frame to frame for
   anything that has not moved. g_ob_view[] says whether a level holds a
   view reference at all; a load after the view discards it. */
static float         g_ob[MV_DEPTH][16];
static unsigned char g_ob_view[MV_DEPTH];
/* The camera is set up in TWO steps at the base of the stack -- a rotation,
   then a translation by minus its position (measured: the rotation's own
   translation is exactly zero, and every draw then shares one translation
   that shifts by the camera's motion). So "the view" is every multiply made
   at the view's own stack level; anything pushed above it is an object. */
static int      g_view_depth = -1;
static int      g_pass_idx;         /* world passes so far this frame */
static int      g_view_pushed;      /* an object level exists above the view */
static unsigned g_view_late;        /* view-level multiplies AFTER that --
                                       these would break the assumption */

/* The PROJECTION shadow: the engine's own projection stack, so its reads
   and projection_changed() need not ask the driver. See no_readbacks.py. */
static void pj_load(const float *m) { memcpy(g_pj[g_pj_sp], m, 64); }
static void pj_mult(const float *m) { m_mul(g_pj[g_pj_sp], g_pj[g_pj_sp], m); }
static void pj_loadi(void)          { m_ident(g_pj[g_pj_sp]); }
static void pj_push(void) {
    if (g_pj_sp + 1 < PJ_DEPTH) { memcpy(g_pj[g_pj_sp + 1], g_pj[g_pj_sp], 64); g_pj_sp++; }
}
static void pj_pop(void) { if (g_pj_sp > 0) g_pj_sp--; }
static void pj_multd(const GLdouble *d) {
    float m[16]; int i; for (i = 0; i < 16; i++) m[i] = (float)d[i]; pj_mult(m);
}
static void pj_ortho(double l, double r, double b, double t, double n, double f) {
    float m[16]; m_ident(m);
    m[0]  = (float)(2.0 / (r - l));  m[5]  = (float)(2.0 / (t - b));
    m[10] = (float)(-2.0 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    pj_mult(m);
}
static void pj_frustum(double l, double r, double b, double t, double n, double f) {
    float m[16]; memset(m, 0, sizeof(m));
    m[0]  = (float)(2.0 * n / (r - l)); m[5] = (float)(2.0 * n / (t - b));
    m[8]  = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n)); m[11] = -1.0f;
    m[14] = (float)(-2.0 * f * n / (f - n));
    pj_mult(m);
}


static void mv_init(void) {
    m_ident(g_mv[0]); m_ident(g_ob[0]); g_ob_view[0] = 0; g_mv_sp = 0;
    m_ident(g_pj[0]); g_pj_sp = 0;
}
static void mv_load(const float *m) {
    memcpy(g_mv[g_mv_sp], m, 64);
    memcpy(g_ob[g_mv_sp], m, 64); g_ob_view[g_mv_sp] = 0;
}
static void mv_loadi(void) {
    m_ident(g_mv[g_mv_sp]);
    m_ident(g_ob[g_mv_sp]); g_ob_view[g_mv_sp] = 0;
}
static void mv_mult(const float *m) {
    m_mul(g_mv[g_mv_sp], g_mv[g_mv_sp], m);
    if (g_ob_view[g_mv_sp] && g_mv_sp == g_view_depth) {
        /* Still at the camera's own level: this is the camera, and the
           object stack stays at identity. If objects were already being
           pushed above it, the engine is moving the base between objects
           and the split is unsafe -- count it. */
        if (g_view_pushed) g_view_late++;
        return;
    }
    m_mul(g_ob[g_mv_sp], g_ob[g_mv_sp], m);
}
/* The camera's view going onto the stack. The modelview takes it; the object
   stack starts over from identity, and from here on its top is object space. */
static void mv_view(const float *m) {
    m_mul(g_mv[g_mv_sp], g_mv[g_mv_sp], m);
    m_ident(g_ob[g_mv_sp]); g_ob_view[g_mv_sp] = 1;
    g_view_depth = g_mv_sp; g_view_pushed = 0;
    g_pass_idx++;
}
static void mv_multd(const GLdouble *d) {
    float m[16]; int i;
    for (i = 0; i < 16; i++) m[i] = (float)d[i];
    mv_mult(m);
}
static void mv_loadd(const GLdouble *d) {
    float m[16]; int i;
    for (i = 0; i < 16; i++) m[i] = (float)d[i];
    mv_load(m);
}
static void mv_push(void) {
    if (g_mv_sp + 1 >= MV_DEPTH) { g_mv_overflow++; return; }
    if (g_mv_sp == g_view_depth) g_view_pushed = 1;
    memcpy(g_mv[g_mv_sp + 1], g_mv[g_mv_sp], 64);
    memcpy(g_ob[g_mv_sp + 1], g_ob[g_mv_sp], 64);
    g_ob_view[g_mv_sp + 1] = g_ob_view[g_mv_sp];
    g_mv_sp++;
}
static void mv_pop(void) {
    if (g_mv_sp <= 0) { g_mv_underflow++; return; }
    g_mv_sp--;
}
static void mv_translate(float x, float y, float z) {
    float m[16]; m_ident(m);
    m[12] = x; m[13] = y; m[14] = z;
    mv_mult(m);
}
static void mv_scale(float x, float y, float z) {
    float m[16]; m_ident(m);
    m[0] = x; m[5] = y; m[10] = z;
    mv_mult(m);
}
static void mv_rotate(float deg, float x, float y, float z) {
    float m[16], c, s, one, len;
    len = (float)sqrt((double)(x * x + y * y + z * z));
    if (len <= 0.0f) return;              /* GL ignores a zero axis */
    x /= len; y /= len; z /= len;
    c = (float)cos((double)deg * 3.14159265358979323846 / 180.0);
    s = (float)sin((double)deg * 3.14159265358979323846 / 180.0);
    one = 1.0f - c;
    m_ident(m);
    m[0]  = x * x * one + c;      m[4] = x * y * one - z * s;  m[8]  = x * z * one + y * s;
    m[1]  = y * x * one + z * s;  m[5] = y * y * one + c;      m[9]  = y * z * one - x * s;
    m[2]  = z * x * one - y * s;  m[6] = z * y * one + x * s;  m[10] = z * z * one + c;
    mv_mult(m);
}

/* Prove it, once, against the thing it is standing in for. A shadow that has
   silently diverged draws the whole world in the wrong place and nothing
   anywhere reports an error. */
static int    g_mv_checked;
static double g_mv_maxdiff = -1.0;
static void mv_verify(void) {
    float real[16];
    int i;
    double worst = 0.0;
    if (g_mv_checked || !o_glGetFloatv) return;
    g_mv_checked = 1;
    g_internal++;
    o_glGetFloatv(GL_MODELVIEW_MATRIX, real);
    g_internal--;
    for (i = 0; i < 16; i++) {
        double d = (double)real[i] - (double)g_mv[g_mv_sp][i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    g_mv_maxdiff = worst;
    kv_log("MODELVIEW SHADOW: checked against GL at a real draw -- largest "
           "difference %.6f across the 16 values, stack depth %d. Anything "
           "but a tiny number here means the shadow has diverged and every "
           "stage built on it would be wrong.", worst, g_mv_sp);
    if (worst > 0.001) {
        kv_log("MODELVIEW SHADOW: DIVERGED. ours  %.4f %.4f %.4f %.4f",
               g_mv[g_mv_sp][12], g_mv[g_mv_sp][13], g_mv[g_mv_sp][14],
               g_mv[g_mv_sp][15]);
        kv_log("MODELVIEW SHADOW:           GL    %.4f %.4f %.4f %.4f",
               real[12], real[13], real[14], real[15]);
    }
}

/* ---- what actually moves between frames ---------------------------------
   Key each draw on (display list, modelview) and ask whether that exact pair
   was drawn last frame. Buildings do not move; keflings do. The static
   fraction decides how much of a merged buffer must be rebuilt per frame. */
#define MVC_CAP 16384
static unsigned  g_mvc_a[MVC_CAP], g_mvc_b[MVC_CAP];
static unsigned *g_mvc_cur = g_mvc_a, *g_mvc_prev = g_mvc_b;
static unsigned  g_mvc_same, g_mvc_seen, g_mvc_distinct;
static double    g_mvcw_distinct;
static double    g_mvcw_same, g_mvcw_seen, g_mvcw_frames;
/* The control: the same question asked of the display list alone. The same
   scene draws the same meshes every frame, so this MUST come back near 100%.
   If it does not, the census is measuring nothing -- which is precisely what
   could not be told apart when the first run reported a flat 0%. */
#define MVL_CAP 1024
static unsigned  g_mvl_a[MVL_CAP], g_mvl_b[MVL_CAP];
static unsigned *g_mvl_cur = g_mvl_a, *g_mvl_prev = g_mvl_b;
static unsigned  g_mvl_same, g_mvl_seen;
static double    g_mvlw_same, g_mvlw_seen;

/* inverse of a RIGID transform: transpose the rotation, and -R^T.t. */
static void mv_unview(float *out, const float *v, const float *mv) {
    float inv[16];
    int r, c;
    for (c = 0; c < 3; c++)
        for (r = 0; r < 3; r++) inv[c * 4 + r] = v[r * 4 + c];
    inv[3] = inv[7] = inv[11] = 0.0f; inv[15] = 1.0f;
    inv[12] = -(v[0] * v[12] + v[1]  * v[13] + v[2]  * v[14]);
    inv[13] = -(v[4] * v[12] + v[5]  * v[13] + v[6]  * v[14]);
    inv[14] = -(v[8] * v[12] + v[9]  * v[13] + v[10] * v[14]);
    m_mul(out, inv, mv);
}
/* Draws judged impossible to place because their stack level held no view,
   and a one-time check that the object stack agrees with the modelview once
   the view is divided back out -- which proves the view was identified. */
static unsigned g_mvc_noview;
static double   g_mvcw_noview;
static int      g_ob_checked;
static void ob_verify(void) {
    /* The decomposition itself, checked directly: the full modelview must
       equal the view level times the object stack. Only meaningful for a
       draw pushed ABOVE the view, where there is an object to speak of. */
    float back[16];
    int i;
    double worst = 0.0, scale = 1.0;
    if (g_view_depth < 0 || g_mv_sp <= g_view_depth) return;
    g_ob_checked = 1;
    m_mul(back, g_mv[g_view_depth], g_ob[g_mv_sp]);
    for (i = 0; i < 16; i++) {
        double d = (double)back[i] - (double)g_mv[g_mv_sp][i];
        double a = back[i] < 0 ? -back[i] : back[i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
        if (a > scale) scale = a;
    }
    kv_log("OBJECT STACK: view x object against the full modelview -- largest "
           "difference %.6f on values up to %.1f, object %d levels above the "
           "view. Anything but float rounding means the view/object split is "
           "wrong and the census is measuring the wrong matrix.",
           worst, scale, g_mv_sp - g_view_depth);
}


/* The transpose is only the inverse if the view is rigid. Say so once. */
static void mv_check_rigid(const float *v) {
    static int said;
    double d0, d1, x;
    if (said) return;
    said = 1;
    d0 = (double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2];
    d1 = (double)v[0] * v[4] + (double)v[1] * v[5] + (double)v[2] * v[6];
    x = d0 - 1.0; if (x < 0) x = -x;
    if (d1 < 0) d1 = -d1;
    kv_log("STATIC CENSUS: the view's first axis has length^2 %.6f and dots "
           "%.6f with the second. Both must be 1 and 0 for the transpose to "
           "be its inverse; if not, the object matrices below are wrong.",
           d0, d1);
    if (x > 0.01 || d1 > 0.01)
        kv_log("STATIC CENSUS: the view is NOT rigid -- it carries a scale. "
               "Divide it out properly before trusting any figure here.");
}

static int mvc_probe(unsigned *t, unsigned key, int insert) {
    unsigned i = (key * 2654435761u) & (MVC_CAP - 1u), tries;
    for (tries = 0; tries < 64; tries++) {
        if (t[i] == key) return 1;
        if (t[i] == 0) {
            if (insert) t[i] = key;
            return 0;
        }
        i = (i + 1u) & (MVC_CAP - 1u);
    }
    return 0;                        /* crowded: reads as "moved", never as
                                        "static", so the answer stays honest */
}
static int mvc_probe2(unsigned *t, unsigned key, int insert) {
    unsigned i = (key * 2654435761u) & (MVL_CAP - 1u), tries;
    for (tries = 0; tries < 64; tries++) {
        if (t[i] == key) return 1;
        if (t[i] == 0) { if (insert) t[i] = key; return 0; }
        i = (i + 1u) & (MVL_CAP - 1u);
    }
    return 0;
}
static void mvc_note(GLuint list) {
    float obj[16];
    const float *src = g_mv[g_mv_sp];
    unsigned hl;
    /* The object's own matrix, from the stack that never saw the view. A
       draw from a level with no view reference cannot be judged, so it is
       counted and set aside rather than guessed at. */
    (void)obj;
    if (!g_ob_view[g_mv_sp]) { g_mvc_noview++; return; }
    src = g_ob[g_mv_sp];
    if (g_have_view && !g_ob_checked) ob_verify();
    /* Set membership, not slot position: order cannot affect the answer.
       Two grids, because the sixteen values are not alike -- the rotation is
       order 1 with ~1e-6 of noise, the translation is order 1000 with ~5e-4,
       and one grid for both is what broke the earlier attempt. */
    {
        unsigned k = 2166136261u;
        int r, c;
        /* Exact bits. The object stack is built only from the engine's own
           inputs, so a static object reproduces bit for bit; any grid here
           would only reintroduce the boundary flapping it was meant to
           avoid. */
        const unsigned char *b = (const unsigned char *)src;
        (void)r;
        for (c = 0; c < 64; c++) k = (k ^ b[c]) * 16777619u;
        k = (k ^ (unsigned)list) * 16777619u;
        if (!k) k = 1u;
        g_mvc_seen++;
        if (mvc_probe(g_mvc_prev, k, 0)) g_mvc_same++;
        if (!mvc_probe(g_mvc_cur, k, 1)) g_mvc_distinct++;
    }

    hl = ((2166136261u ^ (unsigned)list) * 16777619u);
    if (!hl) hl = 1u;
    g_mvl_seen++;
    if (mvc_probe2(g_mvl_prev, hl, 0)) g_mvl_same++;
    mvc_probe2(g_mvl_cur, hl, 1);
}
static void mvc_frame_end(void) {
    unsigned *t;
    if (!g_mvc_seen) return;
    g_mvcw_same  += g_mvc_same;
    g_mvcw_seen  += g_mvc_seen;
    g_mvcw_distinct += g_mvc_distinct; g_mvc_distinct = 0;
    g_mvcw_noview   += g_mvc_noview;   g_mvc_noview = 0;
    g_mvlw_same  += g_mvl_same;
    g_mvlw_seen  += g_mvl_seen;
    g_mvcw_frames += 1.0;
    t = g_mvc_prev; g_mvc_prev = g_mvc_cur; g_mvc_cur = t;
    memset(g_mvc_cur, 0, sizeof(g_mvc_a));
    t = g_mvl_prev; g_mvl_prev = g_mvl_cur; g_mvl_cur = t;
    memset(g_mvl_cur, 0, sizeof(g_mvl_a));
    g_mvc_same = 0; g_mvc_seen = 0;
    g_mvl_same = 0; g_mvl_seen = 0;
}

void APIENTRY hk_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_MODELVIEW) mv_translate(x, y, z);
    o_glTranslatef(x, y, z);
}
void APIENTRY hk_glTranslated(GLdouble x, GLdouble y, GLdouble z) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_MODELVIEW)
        mv_translate((float)x, (float)y, (float)z);
    o_glTranslated(x, y, z);
}
void APIENTRY hk_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_MODELVIEW) mv_rotate(a, x, y, z);
    o_glRotatef(a, x, y, z);
}
void APIENTRY hk_glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_MODELVIEW)
        mv_rotate((float)a, (float)x, (float)y, (float)z);
    o_glRotated(a, x, y, z);
}
void APIENTRY hk_glScalef(GLfloat x, GLfloat y, GLfloat z) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_MODELVIEW) mv_scale(x, y, z);
    o_glScalef(x, y, z);
}
void APIENTRY hk_glScaled(GLdouble x, GLdouble y, GLdouble z) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_MODELVIEW)
        mv_scale((float)x, (float)y, (float)z);
    o_glScaled(x, y, z);
}

/* ---- stage 1: mesh capture ----------------------------------------------
   Keep our own copy of every display list's geometry, read out of the
   engine's client arrays at the moment it compiles the list.  Observe only:
   every call is still forwarded and the picture cannot move.

   Why this works: glArrayElement dereferences the arrays at COMPILE time, so
   the values are there to be read in the same call. */
#define MC_N        512            /* power of two; 91 distinct lists seen */
#define MC_MAXVERT  65536          /* per mesh; anything larger falls back */

enum { MC_OK = 0, MC_STATE, MC_FORMAT, MC_NOARRAY, MC_TOOBIG, MC_NOMEM,
       MC_MIXED, MC_REASONS };
static const char *k_mc_why[MC_REASONS] = {
    "captured", "the list set GL state as well as geometry",
    "an array format we do not decode", "no vertex array was bound",
    "more vertices than the buffer holds", "out of memory",
    "the list mixes arrays with immediate vertices or nested draws" };

typedef struct {
    GLuint        id;              /* 0 = empty slot */
    BaVert       *v;
    unsigned      nvert, cap;
    unsigned      nblocks;         /* glBegin blocks inside */
    GLenum        mode;            /* first block's primitive mode */
    unsigned      recompiles;
    unsigned char ok, why;
    /* stage 3: the primitive structure, and which attributes were present */
    GLenum       *bmode;
    unsigned     *bfirst, *bcount;
    unsigned      nb, capb;
    unsigned char has_n, has_t, has_c, attrs_set, mergeable;
    unsigned char sets_c;          /* a glColor recorded INTO the list */
    unsigned      gen;             /* bumped on every compile */
    GLuint        vbo;             /* mode 5: this list, on the GPU */
    unsigned      vbo_gen;
    unsigned char vbo_ok;
} Mesh;
static Mesh     g_mc[MC_N];
static unsigned g_mc_verts_held, g_mc_bad[MC_REASONS];
/* per-frame accumulators */
static double   g_mcw_vert_drawn, g_mcw_frames;
static unsigned g_mc_vert_frame, g_mc_recompile_frame, g_mcw_recompiles,
                g_mc_miss_frame, g_mcw_miss;

/* The client arrays, as the engine last set them. */
typedef struct { const unsigned char *p; GLint size; GLenum type;
                 GLsizei stride; int on; } CArr;
static CArr g_ca_v, g_ca_n, g_ca_t, g_ca_c;

/* The list being captured right now. */
static Mesh *g_mc_cur;
static int   g_mc_fail;
static int   g_mc_inblock;
/* Anything that emits geometry by another route during an engine compile
   means stage 1 saw only part of the list. */
static void mc_taint(void) { if (g_mc_cur && !g_mc_fail) g_mc_fail = MC_MIXED; }

static Mesh *mc_find(GLuint id, int make) {
    unsigned h = (unsigned)id * 2654435761u;
    int i;
    if (!id) return NULL;
    for (i = 0; i < 8; i++) {
        unsigned s = (h + (unsigned)i) & (MC_N - 1);
        if (g_mc[s].id == id) return &g_mc[s];
        if (g_mc[s].id == 0) {
            if (!make) return NULL;
            g_mc[s].id = id;
            return &g_mc[s];
        }
    }
    return NULL;                   /* table full at this probe: fall back */
}

/* One attribute out of a client array.  Only the formats this engine
   actually uses are decoded; anything else fails the whole mesh rather than
   guessing, because a wrong decode is a picture bug three stages later. */
static int mc_read3f(const CArr *a, GLint idx, float *out, int want) {
    const unsigned char *p;
    GLsizei stride;
    int k;
    if (!a->on || !a->p) return 0;
    if (a->type != GL_FLOAT) return -1;
    if (a->size < want) return -1;
    stride = a->stride ? a->stride : (GLsizei)(a->size * 4);
    p = a->p + (size_t)idx * (size_t)stride;
    for (k = 0; k < want; k++) memcpy(&out[k], p + k * 4, 4);
    return 1;
}

static void mc_element(GLint idx) {
    BaVert nv;
    int r;
    if (!g_mc_cur || g_mc_fail) return;
    if (!g_mc_inblock) { g_mc_fail = MC_FORMAT; return; }
    memset(&nv, 0, sizeof(nv));
    nv.col[0] = nv.col[1] = nv.col[2] = nv.col[3] = 255;

    r = mc_read3f(&g_ca_v, idx, nv.pos, 3);
    if (r == 0) { g_mc_fail = MC_NOARRAY; return; }
    if (r < 0)  { g_mc_fail = MC_FORMAT;  return; }

    {
        int hn, ht, hc;
        r = mc_read3f(&g_ca_n, idx, nv.nrm, 3);
        if (r < 0)  { g_mc_fail = MC_FORMAT;  return; }
        hn = (r == 1);
        r = mc_read3f(&g_ca_t, idx, nv.tc[0], 2);
        if (r < 0)  { g_mc_fail = MC_FORMAT;  return; }
        ht = (r == 1);
        hc = (g_ca_c.on && g_ca_c.p) ? 1 : 0;
        /* One mesh, one set of attributes: a merged draw enables arrays for
           the whole buffer, so a mesh that switched mid-way cannot merge. */
        if (!g_mc_cur->attrs_set) {
            g_mc_cur->has_n = (unsigned char)hn;
            g_mc_cur->has_t = (unsigned char)ht;
            g_mc_cur->has_c = (unsigned char)hc;
            g_mc_cur->attrs_set = 1;
        } else if (g_mc_cur->has_n != hn || g_mc_cur->has_t != ht ||
                   g_mc_cur->has_c != hc) {
            g_mc_fail = MC_FORMAT; return;
        }
    }

    if (g_ca_c.on && g_ca_c.p) {
        GLsizei stride = g_ca_c.stride;
        const unsigned char *p;
        if (g_ca_c.type == GL_UNSIGNED_BYTE && g_ca_c.size == 4) {
            if (!stride) stride = 4;
            p = g_ca_c.p + (size_t)idx * (size_t)stride;
            memcpy(nv.col, p, 4);
        } else if (g_ca_c.type == GL_FLOAT) {
            float f[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            int k;
            if (!stride) stride = (GLsizei)(g_ca_c.size * 4);
            p = g_ca_c.p + (size_t)idx * (size_t)stride;
            for (k = 0; k < g_ca_c.size && k < 4; k++) memcpy(&f[k], p + k * 4, 4);
            for (k = 0; k < 4; k++) {
                float c = f[k] < 0.0f ? 0.0f : (f[k] > 1.0f ? 1.0f : f[k]);
                nv.col[k] = (unsigned char)(c * 255.0f + 0.5f);
            }
        } else { g_mc_fail = MC_FORMAT; return; }
    }

    if (g_mc_cur->nvert >= g_mc_cur->cap) {
        unsigned want = g_mc_cur->cap ? g_mc_cur->cap * 2 : 256;
        BaVert *nb;
        if (want > MC_MAXVERT) { g_mc_fail = MC_TOOBIG; return; }
        nb = (BaVert *)realloc(g_mc_cur->v, want * sizeof(BaVert));
        if (!nb) { g_mc_fail = MC_NOMEM; return; }
        g_mc_cur->v = nb; g_mc_cur->cap = want;
    }
    g_mc_cur->v[g_mc_cur->nvert++] = nv;
}

static void mc_block_begin(GLenum mode) {
    Mesh *m = g_mc_cur;
    if (!m || g_mc_fail) return;
    if (g_mc_inblock) { g_mc_fail = MC_FORMAT; return; }
    if (m->nb >= m->capb) {
        unsigned want = m->capb ? m->capb * 2 : 16;
        GLenum   *a = (GLenum *)realloc(m->bmode, want * sizeof(GLenum));
        unsigned *f, *c;
        if (!a) { g_mc_fail = MC_NOMEM; return; }
        m->bmode = a;
        f = (unsigned *)realloc(m->bfirst, want * sizeof(unsigned));
        if (!f) { g_mc_fail = MC_NOMEM; return; }
        m->bfirst = f;
        c = (unsigned *)realloc(m->bcount, want * sizeof(unsigned));
        if (!c) { g_mc_fail = MC_NOMEM; return; }
        m->bcount = c;
        m->capb = want;
    }
    m->bmode[m->nb]  = mode;
    m->bfirst[m->nb] = m->nvert;
    m->bcount[m->nb] = 0;
    g_mc_inblock = 1;
}
static void mc_block_end(void) {
    Mesh *m = g_mc_cur;
    if (!m || g_mc_fail || !g_mc_inblock) return;
    m->bcount[m->nb] = m->nvert - m->bfirst[m->nb];
    m->nb++;
    g_mc_inblock = 0;
}

static void mc_list_begin(GLuint id) {
    Mesh *m;
    g_mc_cur = NULL; g_mc_fail = 0;
    if (!g_vrcfg.mesh_capture && !g_vrcfg.merge_group && !g_vrcfg.skip_noop) return;
    m = mc_find(id, 1);
    if (!m) { g_mc_bad[MC_NOMEM]++; return; }   /* table full */
    if (m->nvert) { m->recompiles++; g_mc_recompile_frame++; }
    m->nvert = 0; m->nblocks = 0; m->ok = 0; m->why = MC_OK;
    m->nb = 0; m->attrs_set = 0; m->mergeable = 0;
    m->has_n = m->has_t = m->has_c = 0;
    m->sets_c = 0;
    m->gen++;
    g_mc_inblock = 0;
    g_mc_cur = m;
    g_mc_state_in_list = 0;
}

static void mc_list_end(void) {
    Mesh *m = g_mc_cur;
    unsigned i;
    g_mc_cur = NULL;
    if (!m) return;
    if (!g_mc_fail && g_mc_state_in_list) g_mc_fail = MC_STATE;
    if (!g_mc_fail && g_mc_inblock) g_mc_fail = MC_FORMAT;   /* unterminated */
    g_mc_inblock = 0;
    if (g_mc_fail || !m->nvert) {
        m->ok = 0;
        m->why = (unsigned char)(g_mc_fail ? g_mc_fail : MC_NOARRAY);
        g_mc_bad[m->why]++;
        m->nvert = 0;
    } else {
        unsigned b;
        m->ok = 1; m->why = MC_OK;
        /* Triangle-family only: points and lines cannot become triangles. */
        m->mergeable = m->nb > 0;
        for (b = 0; b < m->nb; b++)
            if (m->bmode[b] < GL_TRIANGLES || m->bmode[b] > GL_POLYGON)
                m->mergeable = 0;
    }
    g_mc_verts_held = 0;
    for (i = 0; i < MC_N; i++)
        if (g_mc[i].id && g_mc[i].ok) g_mc_verts_held += g_mc[i].nvert;
}

/* Called from glCallList: how much geometry does a frame actually draw? */
static void mc_note_call(GLuint id) {
    Mesh *m = mc_find(id, 0);
    if (m && m->ok) g_mc_vert_frame += m->nvert;
    else            g_mc_miss_frame++;
}

static void mc_frame_end(void) {
    g_mcw_vert_drawn += (double)g_mc_vert_frame;
    g_mcw_recompiles += g_mc_recompile_frame;
    g_mcw_miss       += g_mc_miss_frame;
    g_mcw_frames     += 1.0;
    g_mc_vert_frame = 0; g_mc_recompile_frame = 0; g_mc_miss_frame = 0;
}

void APIENTRY hk_glArrayElement(GLint i) {
    STATE_TOUCH();
    if (g_mc_cur && !g_internal) {
        __try { mc_element(i); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_mc_fail = MC_FORMAT; }
    }
    o_glArrayElement(i);
}
void APIENTRY hk_glVertexPointer(GLint size, GLenum type, GLsizei stride,
                                 const GLvoid *p) {
    STATE_TOUCH();
    if (!g_internal) { g_ca_v.p = (const unsigned char *)p; g_ca_v.size = size;
                       g_ca_v.type = type; g_ca_v.stride = stride; }
    o_glVertexPointer(size, type, stride, p);
}
void APIENTRY hk_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) {
    STATE_TOUCH();
    if (!g_internal) { g_ca_n.p = (const unsigned char *)p; g_ca_n.size = 3;
                       g_ca_n.type = type; g_ca_n.stride = stride; }
    o_glNormalPointer(type, stride, p);
}
void APIENTRY hk_glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                                   const GLvoid *p) {
    STATE_TOUCH();
    if (!g_internal) { g_ca_t.p = (const unsigned char *)p; g_ca_t.size = size;
                       g_ca_t.type = type; g_ca_t.stride = stride; }
    o_glTexCoordPointer(size, type, stride, p);
}
void APIENTRY hk_glColorPointer(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *p) {
    STATE_TOUCH();
    if (!g_internal) { g_ca_c.p = (const unsigned char *)p; g_ca_c.size = size;
                       g_ca_c.type = type; g_ca_c.stride = stride; }
    o_glColorPointer(size, type, stride, p);
}
static void mc_client_state(GLenum cap, int on) {
    switch (cap) {
        case GL_VERTEX_ARRAY_:        g_ca_v.on = on; break;
        case GL_NORMAL_ARRAY_:        g_ca_n.on = on; break;
        case GL_TEXTURE_COORD_ARRAY_: g_ca_t.on = on; break;
        case GL_COLOR_ARRAY_:         g_ca_c.on = on; break;
        default: break;
    }
}
void APIENTRY hk_glEnableClientState(GLenum cap) {
    STATE_TOUCH();
    if (!g_internal) mc_client_state(cap, 1);
    o_glEnableClientState(cap);
}
void APIENTRY hk_glDisableClientState(GLenum cap) {
    STATE_TOUCH();
    if (!g_internal) mc_client_state(cap, 0);
    o_glDisableClientState(cap);
}

/* ---- stage 3: merge one state group -------------------------------------
   merge_group 1 = audit + offscreen pixel A/B (screen unchanged),
               2 = draw the group from one buffer. */
#define MG_CURRENT_COLOR      0x0B00
#define MG_CURRENT_NORMAL     0x0B02
#define MG_CURRENT_TEXCOORDS  0x0B03
#define MG_TEXTURE_MATRIX     0x0BA8
#define MG_TEXTURE_BINDING_2D 0x8069
#define MG_CLIENT_ARRAY_BIT   0x00000002
#define MG_FB                 0x8D40
#define MG_RB                 0x8D41
#define MG_FB_BINDING         0x8CA6
#define MG_COLOR0             0x8CE0
#define MG_DEPTH_ATT          0x8D00
#define MG_DEPTH24            0x81A6
#define MG_RGBA8              0x8058
#define MG_FB_COMPLETE        0x8CD5
#define MG_AB_W 1024
#define MG_AB_H 576

typedef struct { GLuint list; unsigned gen; float obj[16]; unsigned key; } MgInst;
#define MG_SET 8192
static struct {
    int       chosen, gave_up;
    unsigned  sig; int pass;
    BaVert   *vb; unsigned nv, capv;
    MgInst   *inst; unsigned ninst, capinst;      /* in the buffer */
    unsigned  set[MG_SET];                        /* their keys */
    int       has_n, has_t, has_c, valid;
    MgInst   *cur; unsigned ncur, capcur;         /* seen this frame */
    int       drawn;
    /* window statistics */
    unsigned  w_frames, w_matched, w_unmatched, w_merged, w_rebuilds,
              w_excluded;
} g_mg;

/* ---- choosing the group: count (signature, pass) over frames 300-599 ---- */
#define MG_SEL 1024
static struct { unsigned sig; int pass; unsigned n; } g_mg_sel[MG_SEL];
/* The window is timed from the first frame the WORLD draws, not from frame
   zero: at 160 fps flat, frames 300-599 are the loading screen and there is
   nothing to choose from. */
static unsigned g_mg_t0;                /* 0 = world not seen yet */
static unsigned g_mg_rej_blend, g_mg_rej_dtest, g_mg_rej_dmask, g_mg_took;
#define MG_T(k) (g_mg_t0 && g_frames >= g_mg_t0 + (k))
/* What the blended majority actually is. Keyed on the blend setup, alpha
   test and material diffuse alpha; each new setup is also checked once
   against GL itself, because the tracked flag is only as good as the hooks
   (glPopAttrib restores GL_BLEND without passing through glEnable). */
static void ta_note(void);   /* defined with the texture census below */
#define MG_BC 64
static struct { unsigned blend, src, dst, atest, afunc, aref, dalpha1, n;
                unsigned real_blend, checked; } g_mg_bc[MG_BC];
static unsigned g_mg_bc_n, g_mg_bc_mismatch;
static void mg_blend_note(void) {
    unsigned blend = g_gk_have[GKS_CAP(0)] && g_gk_val[GKS_CAP(0)];
    unsigned src   = g_gk_have[GKS_BSRC] ? g_gk_val[GKS_BSRC] : 1u;
    unsigned dst   = g_gk_have[GKS_BDST] ? g_gk_val[GKS_BDST] : 0u;
    unsigned atest = g_gk_have[GKS_CAP(1)] && g_gk_val[GKS_CAP(1)];
    unsigned afunc = g_sf_alpha.valid ? (unsigned)g_sf_alpha.func : 0u;
    unsigned aref  = g_sf_alpha.valid ? gk_f((GLfloat)g_sf_alpha.ref) : 0u;
    unsigned dal1  = 1;
    unsigned i;
    int k = SF_KEY(GL_DIFFUSE);
    if (SF_OK(g_sf_mat[k]) && g_sf_mat[k].pname == GL_DIFFUSE &&
        g_sf_mat[k].iv == 0x7FFFFFFF && g_sf_mat[k].v[3] < 0.999f) dal1 = 0;
    for (i = 0; i < g_mg_bc_n; i++)
        if (g_mg_bc[i].blend == blend && g_mg_bc[i].src == src &&
            g_mg_bc[i].dst == dst && g_mg_bc[i].atest == atest &&
            g_mg_bc[i].afunc == afunc && g_mg_bc[i].aref == aref &&
            g_mg_bc[i].dalpha1 == dal1) {
            g_mg_bc[i].n++;
            if (blend) ta_note();
            return;
        }
    if (g_mg_bc_n >= MG_BC) return;
    i = g_mg_bc_n++;
    g_mg_bc[i].blend = blend; g_mg_bc[i].src = src; g_mg_bc[i].dst = dst;
    g_mg_bc[i].atest = atest; g_mg_bc[i].afunc = afunc; g_mg_bc[i].aref = aref;
    g_mg_bc[i].dalpha1 = dal1; g_mg_bc[i].n = 1;
    /* (texture classified below, per draw, for blended draws) */
    /* once per setup: does GL agree with the tracked flag? */
    if (blend) ta_note();
    g_mg_bc[i].real_blend = o_glIsEnabled(0x0BE2) ? 1u : 0u;
    g_mg_bc[i].checked = 1;
    if (g_mg_bc[i].real_blend != blend) g_mg_bc_mismatch++;
}
/* ---- texture alpha: is the "blended" world actually translucent? --------
   Blending with SRC_ALPHA / ONE_MINUS_SRC_ALPHA is a no-op on a texel of
   alpha 255, so a FULLY OPAQUE texture is order-independent and can be
   merged even with blending on. A CUT-OUT (alpha 0 somewhere) is not: the
   filtering that samples it softens every hole's edge into partial alpha,
   and a reordered draw blends that edge against the wrong background. Each
   texture is read back once, level 0, and classified. */
enum { TA_OPAQUE = 0, TA_CUTOUT, TA_SOFT, TA_UNKNOWN, TA_N };
static const char *k_ta[TA_N] = { "fully opaque", "cut-out (alpha 0 and 255 only)",
                                  "soft (partial alpha)", "not readable" };
#define TA_CAP 1024
static struct { GLuint id; unsigned char cls; float partial; } g_ta[TA_CAP];
static unsigned g_ta_draws[TA_N], g_ta_tex[TA_N];
static void (APIENTRY *ta_getteximage)(GLenum, GLint, GLenum, GLenum, GLvoid *);
static void (APIENTRY *ta_getlevel)(GLenum, GLint, GLenum, GLint *);
static int ta_classify(GLuint id, float *partial) {
    GLint w = 0, h = 0;
    unsigned char *px;
    size_t n, i, z = 0, full = 0, part = 0;
    *partial = 0.0f;
    if (!ta_getteximage) {
        ta_getteximage = (void *)proxy_real_proc("glGetTexImage");
        ta_getlevel    = (void *)proxy_real_proc("glGetTexLevelParameteriv");
    }
    if (!ta_getteximage || !ta_getlevel) return TA_UNKNOWN;
    ta_getlevel(GL_TEXTURE_2D, 0, 0x1000, &w);     /* GL_TEXTURE_WIDTH */
    ta_getlevel(GL_TEXTURE_2D, 0, 0x1001, &h);     /* GL_TEXTURE_HEIGHT */
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return TA_UNKNOWN;
    n = (size_t)w * (size_t)h;
    px = (unsigned char *)malloc(n * 4);
    if (!px) return TA_UNKNOWN;
    ta_getteximage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < n; i++) {
        unsigned char a = px[i * 4 + 3];
        if (a == 0) z++; else if (a == 255) full++; else part++;
    }
    free(px);
    *partial = (float)part / (float)n;
    if (part) return TA_SOFT;
    if (z)    return TA_CUTOUT;
    return TA_OPAQUE;
}
static void ta_note(void) {
    GLint id = 0;
    unsigned i, t;
    o_glGetIntegerv(0x8069, &id);                  /* GL_TEXTURE_BINDING_2D */
    i = ((unsigned)id * 2654435761u) & (TA_CAP - 1);
    for (t = 0; t < TA_CAP; t++, i = (i + 1) & (TA_CAP - 1)) {
        if (g_ta[i].id == (GLuint)id && (id || g_ta[i].cls)) {
            g_ta_draws[g_ta[i].cls]++;
            return;
        }
        if (g_ta[i].id == 0 && g_ta[i].cls == 0 && g_ta[i].partial == 0.0f) {
            float pf;
            int c = id ? ta_classify((GLuint)id, &pf) : TA_UNKNOWN;
            g_ta[i].id = (GLuint)id; g_ta[i].cls = (unsigned char)c;
            g_ta[i].partial = id ? pf : -1.0f;
            g_ta_tex[c]++;
            g_ta_draws[c]++;
            return;
        }
    }
}
static void ta_report(void) {
    int c;
    unsigned total = 0;
    for (c = 0; c < TA_N; c++) total += g_ta_draws[c];
    kv_log("TEXTURE ALPHA: the %u BLENDED mergeable draws in the last 300 frames, by "
           "what their texture actually contains:", total);
    for (c = 0; c < TA_N; c++)
        if (g_ta_draws[c] || g_ta_tex[c])
            kv_log("TEXTURE ALPHA:   %6.1f draws/frame (%4.1f%%) on %3u "
                   "textures  %s", g_ta_draws[c] / 300.0,
                   total ? 100.0 * g_ta_draws[c] / total : 0.0, g_ta_tex[c],
                   k_ta[c]);
    kv_log("TEXTURE ALPHA: fully opaque textures blend to exactly themselves, "
           "so those draws can be merged in any order. Cut-outs and soft "
           "alpha cannot, without keeping their order.");
}

static void mg_blend_report(void) {
    unsigned i, total = 0, shown;
    for (i = 0; i < g_mg_bc_n; i++) total += g_mg_bc[i].n;
    kv_log("BLEND CENSUS: %u mergeable world draws in the last 300 frames fall into "
           "%u blend setups. Tracked blend flag disagreed with GL on %u of "
           "them (must be 0, or every figure here is suspect).",
           total, g_mg_bc_n, g_mg_bc_mismatch);
    for (shown = 0; shown < 10; shown++) {
        unsigned best = 0, bi = MG_BC;
        float ref;
        for (i = 0; i < g_mg_bc_n; i++)
            if (g_mg_bc[i].n > best) { best = g_mg_bc[i].n; bi = i; }
        if (bi == MG_BC) break;
        memcpy(&ref, &g_mg_bc[bi].aref, 4);
        kv_log("BLEND CENSUS:   %6.1f draws/frame  blend %s src 0x%04X dst "
               "0x%04X | alpha test %s func 0x%04X ref %.3f | material "
               "alpha %s", best / 300.0, g_mg_bc[bi].blend ? "ON " : "off",
               g_mg_bc[bi].src, g_mg_bc[bi].dst,
               g_mg_bc[bi].atest ? "ON " : "off", g_mg_bc[bi].afunc, ref,
               g_mg_bc[bi].dalpha1 ? "1" : "<1");
        g_mg_bc[bi].n = 0;
    }
    kv_log("BLEND CENSUS: SRC_ALPHA=0x0302 ONE_MINUS_SRC_ALPHA=0x0303 ONE=1 "
           "ZERO=0. Blending ON with material alpha 1 and alpha test ON is "
           "opaque everywhere the texture is, and only order-dependent on "
           "texels between the alpha-test threshold and fully opaque.");
}

static void mg_select_note(void) {
    unsigned h, i, t;
    /* opaque, depth-tested, depth-writing: the only kind reordering is
       invisible for */
    int blend = g_gk_have[GKS_CAP(0)] && g_gk_val[GKS_CAP(0)];
    int dtest = g_gk_have[GKS_CAP(5)] && g_gk_val[GKS_CAP(5)];
    int dmask = !g_gk_have[GKS_DMASK] || g_gk_val[GKS_DMASK];
    if (blend)  { g_mg_rej_blend++; return; }
    if (!dtest) { g_mg_rej_dtest++; return; }
    if (!dmask) { g_mg_rej_dmask++; return; }
    g_mg_took++;
    h = (g_gk_sig * 2654435761u) ^ ((unsigned)g_pass_idx * 0x9E3779B9u);
    i = h & (MG_SEL - 1);
    for (t = 0; t < MG_SEL; t++, i = (i + 1) & (MG_SEL - 1)) {
        if (g_mg_sel[i].n == 0) {
            g_mg_sel[i].sig = g_gk_sig; g_mg_sel[i].pass = g_pass_idx;
            g_mg_sel[i].n = 1; return;
        }
        if (g_mg_sel[i].sig == g_gk_sig && g_mg_sel[i].pass == g_pass_idx) {
            g_mg_sel[i].n++; return;
        }
    }
}
static void mg_choose(void) {
    unsigned i, best = 0, k;

    unsigned top[5] = { 0, 0, 0, 0, 0 };
    int      topi[5] = { -1, -1, -1, -1, -1 };
    for (i = 0; i < MG_SEL; i++) {
        unsigned n = g_mg_sel[i].n;
        if (!n) continue;
        for (k = 0; k < 5; k++)
            if (n > top[k]) {
                unsigned j;
                for (j = 4; j > k; j--) { top[j] = top[j - 1]; topi[j] = topi[j - 1]; }
                top[k] = n; topi[k] = (int)i; break;
            }
    }
    if (topi[0] < 0) {
        g_mg.gave_up = 1;
        kv_log("MERGE: no opaque, depth-writing group with mergeable meshes "
               "was found. Rejected: %u blended, %u depth test off, %u depth "
               "writes off. Nothing to merge.",
               g_mg_rej_blend, g_mg_rej_dtest, g_mg_rej_dmask);
        return;
    }
    best = (unsigned)topi[0];
    /* The client-array entry points are borrowed from block_arrays and are
       only loaded by ba_procs_ready(); with block_arrays off they are NULL. */
    if (!ba_procs_ready()) {
        g_mg.gave_up = 1;
        kv_log("MERGE: client-array entry points could not be loaded; "
               "nothing merged.");
        return;
    }
    g_mg.sig = g_mg_sel[best].sig; g_mg.pass = g_mg_sel[best].pass;
    g_mg.chosen = 1;
    kv_log("MERGE: chose state group %08X in world pass %d -- %.1f "
           "mergeable draws a frame over the 300 frames from %u. Of all "
           "mergeable draws, %u qualified and %u blended, %u depth test off, "
           "%u depth writes off were turned down. The next four:",
           g_mg.sig, g_mg.pass, top[0] / 300.0, g_mg_t0 + 100, g_mg_took,
           g_mg_rej_blend, g_mg_rej_dtest, g_mg_rej_dmask);
    for (k = 1; k < 5; k++)
        if (topi[k] >= 0)
            kv_log("MERGE:   group %08X pass %d  %.1f draws a frame",
                   g_mg_sel[topi[k]].sig, g_mg_sel[topi[k]].pass,
                   top[k] / 300.0);
}

/* ---- the buffer ---------------------------------------------------------- */
static int mg_set_has(unsigned key) {
    unsigned i = (key * 2654435761u) & (MG_SET - 1), t;
    for (t = 0; t < 64; t++, i = (i + 1) & (MG_SET - 1)) {
        if (g_mg.set[i] == key) return 1;
        if (g_mg.set[i] == 0) return 0;
    }
    return 0;
}
static void mg_set_add(unsigned key) {
    unsigned i = (key * 2654435761u) & (MG_SET - 1), t;
    for (t = 0; t < 64; t++, i = (i + 1) & (MG_SET - 1)) {
        if (g_mg.set[i] == key) return;
        if (g_mg.set[i] == 0) { g_mg.set[i] = key; return; }
    }
}
static int mg_push(const BaVert *v) {
    if (g_mg.nv >= g_mg.capv) {
        /* 24 Sep: this started at 65,536 vertices -- 2.4 MB -- for EVERY
           group however small, and doubled. 84 groups held 12.9 MB of
           vertices in 132.8 MB of copies, plus as much again in buffer
           objects, in a 32-bit process with 2 GB for everything; a merge run
           crashed the engine on an allocation. Start small, grow by half. */
        unsigned want = g_mg.capv ? g_mg.capv + g_mg.capv / 2 + 256 : 1024;
        BaVert *nb = (BaVert *)realloc(g_mg.vb, want * sizeof(BaVert));
        if (!nb) return 0;
        g_mg.vb = nb; g_mg.capv = want;
    }
    g_mg.vb[g_mg.nv++] = *v;
    return 1;
}
/* One vertex of one instance, into view-level space. Normals by the object
   matrix's inverse-transpose and NOT normalised: GL applies the view's normal
   matrix afterwards, and the product has to equal what the engine's own
   draw gave, scaled normals and all. */
static void mg_xform(BaVert *o, const BaVert *v, const float *m,
                     const float *it) {
    *o = *v;
    o->pos[0] = m[0] * v->pos[0] + m[4] * v->pos[1] + m[8]  * v->pos[2] + m[12];
    o->pos[1] = m[1] * v->pos[0] + m[5] * v->pos[1] + m[9]  * v->pos[2] + m[13];
    o->pos[2] = m[2] * v->pos[0] + m[6] * v->pos[1] + m[10] * v->pos[2] + m[14];
    o->nrm[0] = it[0] * v->nrm[0] + it[1] * v->nrm[1] + it[2] * v->nrm[2];
    o->nrm[1] = it[3] * v->nrm[0] + it[4] * v->nrm[1] + it[5] * v->nrm[2];
    o->nrm[2] = it[6] * v->nrm[0] + it[7] * v->nrm[1] + it[8] * v->nrm[2];
}
/* Inverse-transpose of the upper 3x3, rows of it[] in order. */
static int mg_invt(float *it, const float *m) {
    double a = m[0], b = m[4], c = m[8];
    double d = m[1], e = m[5], f = m[9];
    double g = m[2], h = m[6], i = m[10];
    double c00 = e * i - f * h, c01 = -(d * i - f * g), c02 = d * h - e * g;
    double c10 = -(b * i - c * h), c11 = a * i - c * g, c12 = -(a * h - b * g);
    double c20 = b * f - c * e, c21 = -(a * f - c * d), c22 = a * e - b * d;
    double det = a * c00 + b * c01 + c * c02;
    if (det > -1e-12 && det < 1e-12) return 0;
    it[0] = (float)(c00 / det); it[1] = (float)(c01 / det); it[2] = (float)(c02 / det);
    it[3] = (float)(c10 / det); it[4] = (float)(c11 / det); it[5] = (float)(c12 / det);
    it[6] = (float)(c20 / det); it[7] = (float)(c21 / det); it[8] = (float)(c22 / det);
    return 1;
}
/* Every block rewritten as plain triangles, winding preserved. */
static int mg_emit(const Mesh *me, const float *m) {
    float it[9];
    unsigned b, k;
    BaVert o;
    if (!mg_invt(it, m)) return 0;
#define MG_V(idx) do { mg_xform(&o, &me->v[f + (idx)], m, it); \
                       if (!mg_push(&o)) return 0; } while (0)
    for (b = 0; b < me->nb; b++) {
        unsigned f = me->bfirst[b], n = me->bcount[b];
        switch (me->bmode[b]) {
        case GL_TRIANGLES:
            for (k = 0; k + 2 < n; k += 3) { MG_V(k); MG_V(k + 1); MG_V(k + 2); }
            break;
        case GL_TRIANGLE_STRIP:
            for (k = 0; k + 2 < n; k++) {
                if (k & 1) { MG_V(k + 1); MG_V(k); MG_V(k + 2); }
                else       { MG_V(k); MG_V(k + 1); MG_V(k + 2); }
            }
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            for (k = 1; k + 1 < n; k++) { MG_V(0); MG_V(k); MG_V(k + 1); }
            break;
        case GL_QUADS:
            for (k = 0; k + 3 < n; k += 4) {
                MG_V(k); MG_V(k + 1); MG_V(k + 2);
                MG_V(k); MG_V(k + 2); MG_V(k + 3);
            }
            break;
        case GL_QUAD_STRIP:
            for (k = 0; k + 3 < n; k += 2) {
                MG_V(k); MG_V(k + 1); MG_V(k + 3);
                MG_V(k); MG_V(k + 3); MG_V(k + 2);
            }
            break;
        default:
            return 0;
        }
    }
#undef MG_V
    return 1;
}
static void mg_build(void) {
    unsigned i, first = 1;
    g_mg.nv = 0; g_mg.ninst = 0; g_mg.valid = 0;
    memset(g_mg.set, 0, sizeof(g_mg.set));
    for (i = 0; i < g_mg.ncur; i++) {
        const MgInst *in = &g_mg.cur[i];
        Mesh *me = mc_find(in->list, 0);
        unsigned nv0 = g_mg.nv;
        if (!me || !me->ok || !me->mergeable || me->gen != in->gen) {
            g_mg.w_excluded++; continue;
        }
        if (first) {
            g_mg.has_n = me->has_n; g_mg.has_t = me->has_t;
            g_mg.has_c = me->has_c; first = 0;
        } else if (me->has_n != g_mg.has_n || me->has_t != g_mg.has_t ||
                   me->has_c != g_mg.has_c) {
            g_mg.w_excluded++; continue;   /* draws normally */
        }
        if (!mg_emit(me, in->obj)) { g_mg.nv = nv0; g_mg.w_excluded++; continue; }
        if (g_mg.ninst >= g_mg.capinst) {
            unsigned want = g_mg.capinst ? g_mg.capinst * 2 : 1024;
            MgInst *ni = (MgInst *)realloc(g_mg.inst, want * sizeof(MgInst));
            if (!ni) break;
            g_mg.inst = ni; g_mg.capinst = want;
        }
        g_mg.inst[g_mg.ninst++] = *in;
        mg_set_add(in->key);
    }
    g_mg.valid = g_mg.ninst > 0 && g_mg.nv > 0;
    g_mg.w_rebuilds++;
}

/* ---- drawing it ----------------------------------------------------------
   Direct driver calls only. restore_engine_state() SETS g_internal to 0, so
   wrapping the per-eye loop in g_internal++ / -- would leave it at -1. */
static void mg_arrays_on(void) {
    int u;
    BaVert *vb = g_mg.vb;
    ba_pushc(MG_CLIENT_ARRAY_BIT);
    /* Every unit's texcoord array off first: an array the engine left enabled
       on another unit would be read at our vertex count, off the end of its
       own data. */
    if (ba_clientactive)
        for (u = 3; u >= 0; u--) {
            ba_clientactive(GL_TEXTURE0_ARB_ + u);
            ba_disablecs(GL_TEXTURE_COORD_ARRAY_);
        }
    ba_disablecs(GL_INDEX_ARRAY_);
    ba_disablecs(GL_EDGE_FLAG_ARRAY_);
    ba_enablecs(GL_VERTEX_ARRAY_);
    ba_vp(3, GL_FLOAT, sizeof(BaVert), vb->pos);
    if (g_mg.has_n) { ba_enablecs(GL_NORMAL_ARRAY_); ba_np(GL_FLOAT, sizeof(BaVert), vb->nrm); }
    else ba_disablecs(GL_NORMAL_ARRAY_);
    if (g_mg.has_t) { ba_enablecs(GL_TEXTURE_COORD_ARRAY_); ba_tp(2, GL_FLOAT, sizeof(BaVert), vb->tc[0]); }
    if (g_mg.has_c) { ba_enablecs(GL_COLOR_ARRAY_); ba_cp(4, GL_UNSIGNED_BYTE, sizeof(BaVert), vb->col); }
    else ba_disablecs(GL_COLOR_ARRAY_);
}
static void mg_arrays_off(void) { ba_popc(); }

static void mg_draw(void) {
    int eye, n;
    (void)eye;
    o_glMatrixMode(GL_MODELVIEW);
    o_glPushMatrix();
    o_glLoadMatrixf(g_mv[g_view_depth]);
    mg_arrays_on();
    if (dup_active()) {
        n = pass_eye_count();
        DUP_LOOP(n, o_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_mg.nv));
    } else {
        o_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_mg.nv);
    }
    mg_arrays_off();
    o_glMatrixMode(GL_MODELVIEW);
    o_glPopMatrix();
    if (g_mode != GL_MODELVIEW) o_glMatrixMode(g_mode);
    g_mg.w_merged++;
}

/* ---- the audit: is everything NOT in the key really the same? ------------ */
#define MG_AUD_N 15
static const char *k_mg_aud[MG_AUD_N] = {
    "current colour", "current normal", "current texcoord", "texture matrix",
    "texture binding", "GL_LIGHTING", "GL_COLOR_MATERIAL", "GL_FOG",
    "GL_TEXTURE_2D", "GL_BLEND", "GL_DEPTH_TEST", "GL_ALPHA_TEST",
    "GL_CULL_FACE", "GL_NORMALIZE", "depth writes" };
static float    g_mga_ref[40];
static unsigned g_mga_diff[MG_AUD_N], g_mga_members, g_mga_frames;
static int      g_mga_have_ref, g_mga_done;
static void mg_audit_member(void) {
    float v[40];
    GLint tb = 0;
    static const GLenum caps[9] = { 0x0B50, 0x0B57, 0x0B60, 0x0DE1, 0x0BE2,
                                    0x0B71, 0x0BC0, 0x0B44, 0x0BA1 };
    static const int span[MG_AUD_N] = { 4, 3, 4, 16, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    GLint dw = 0;
    int i, off = 0;
    o_glGetFloatv(MG_CURRENT_COLOR, v + 0);
    o_glGetFloatv(MG_CURRENT_NORMAL, v + 4);
    o_glGetFloatv(MG_CURRENT_TEXCOORDS, v + 7);
    o_glGetFloatv(MG_TEXTURE_MATRIX, v + 11);
    o_glGetIntegerv(MG_TEXTURE_BINDING_2D, &tb);
    v[27] = (float)tb;
    for (i = 0; i < 9; i++) v[28 + i] = o_glIsEnabled(caps[i]) ? 1.0f : 0.0f;
    o_glGetIntegerv(0x0B72, &dw);      /* GL_DEPTH_WRITEMASK */
    v[37] = (float)dw;
    g_mga_members++;
    if (!g_mga_have_ref) { memcpy(g_mga_ref, v, sizeof(v)); g_mga_have_ref = 1; return; }
    for (i = 0; i < MG_AUD_N; i++) {
        int k, d = 0;
        for (k = 0; k < span[i]; k++) if (v[off + k] != g_mga_ref[off + k]) d = 1;
        if (d) g_mga_diff[i]++;
        off += span[i];
    }
}
static void mg_audit_report(void) {
    int i, any = 0;
    kv_log("MERGE AUDIT: %u member draws over %u frames, each checked for the "
           "state the key does not cover. Meshes %s colour, %s normals, %s "
           "texcoords as arrays.", g_mga_members, g_mga_frames,
           g_mg.has_c ? "carry" : "do NOT carry",
           g_mg.has_n ? "carry" : "do NOT carry",
           g_mg.has_t ? "carry" : "do NOT carry");
    for (i = 0; i < MG_AUD_N; i++)
        if (g_mga_diff[i]) {
            /* The current normal and texcoord are overridden by the arrays
               when the meshes carry them -- and they DO differ between
               members, because executing a list leaves them at its last
               vertex. Harmless then; condemning only when not supplied. */
            int harmless = (i == 1 && g_mg.has_n) || (i == 2 && g_mg.has_t) ||
                           (i == 0 && g_mg.has_c);
            if (!harmless) any = 1;
            kv_log("MERGE AUDIT:   %-18s differs on %u of the members%s",
                   k_mg_aud[i], g_mga_diff[i],
                   harmless ? " -- harmless: the meshes supply it as an array"
                            : "");
        }
    kv_log("MERGE AUDIT: the group draws with lighting %d, colour material "
           "%d, fog %d, texture %d, BLEND %d, DEPTH TEST %d, alpha test %d, "
           "cull %d, normalize %d, DEPTH WRITES %d. Blend must be 0 and both "
           "depth figures 1 for reordering to be invisible.",
           (int)g_mga_ref[28], (int)g_mga_ref[29], (int)g_mga_ref[30],
           (int)g_mga_ref[31], (int)g_mga_ref[32], (int)g_mga_ref[33],
           (int)g_mga_ref[34], (int)g_mga_ref[35], (int)g_mga_ref[36],
           (int)g_mga_ref[37]);
    if (!any)
        kv_log("MERGE AUDIT: CLEAN -- every member drew with identical state. "
               "The key is complete for this group.");
    else
        kv_log("MERGE AUDIT: NOT clean. Anything above that the meshes do "
               "not supply as an array changes the picture when merged.");
}

/* ---- the in-frame pixel A/B ---------------------------------------------- */
static void (APIENTRY *mg_genfb)(GLsizei, GLuint *);
static void (APIENTRY *mg_bindfb)(GLenum, GLuint);
static void (APIENTRY *mg_genrb)(GLsizei, GLuint *);
static void (APIENTRY *mg_bindrb)(GLenum, GLuint);
static void (APIENTRY *mg_rbstore)(GLenum, GLenum, GLsizei, GLsizei);
static void (APIENTRY *mg_fbrb)(GLenum, GLenum, GLenum, GLuint);
static GLenum (APIENTRY *mg_fbstatus)(GLenum);
static void (APIENTRY *mg_clearcolor)(GLclampf, GLclampf, GLclampf, GLclampf);
static void (APIENTRY *mg_readpixels)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                      GLenum, GLvoid *);
static GLuint g_mg_fbo, g_mg_rbc, g_mg_rbd;
static int    g_mg_ab_done;

static void mg_bmp(const char *name, const unsigned char *rgba, int w, int h) {
    char path[MAX_PATH];
    unsigned char hdr[54];
    FILE *f;
    int y, x, row = (w * 3 + 3) & ~3;
    unsigned size = 54u + (unsigned)(row * h);
    unsigned char *line;
    GetTempPathA(MAX_PATH, path);
    strncat(path, name, MAX_PATH - strlen(path) - 1);
    f = fopen(path, "wb");
    if (!f) return;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    line = (unsigned char *)calloc(1, (size_t)row);
    if (line) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
                line[x * 3 + 0] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0];
            }
            fwrite(line, 1, (size_t)row, f);
        }
        free(line);
    }
    fclose(f);
}

static void mg_ab(void) {
    unsigned char *a, *b;
    GLint prev = 0;
    unsigned i, differ = 0, cov_a = 0, cov_b = 0, only = 0, maxd = 0;
    size_t px = (size_t)MG_AB_W * MG_AB_H;
    g_mg_ab_done = 1;
    if (!mg_genfb && o_wglGetProcAddress) {
        mg_genfb    = (void *)o_wglGetProcAddress("glGenFramebuffers");
        mg_bindfb   = (void *)o_wglGetProcAddress("glBindFramebuffer");
        mg_genrb    = (void *)o_wglGetProcAddress("glGenRenderbuffers");
        mg_bindrb   = (void *)o_wglGetProcAddress("glBindRenderbuffer");
        mg_rbstore  = (void *)o_wglGetProcAddress("glRenderbufferStorage");
        mg_fbrb     = (void *)o_wglGetProcAddress("glFramebufferRenderbuffer");
        mg_fbstatus = (void *)o_wglGetProcAddress("glCheckFramebufferStatus");
    }
    if (!mg_clearcolor) mg_clearcolor = (void *)proxy_real_proc("glClearColor");
    if (!mg_readpixels) mg_readpixels = (void *)proxy_real_proc("glReadPixels");
    if (!mg_genfb || !mg_bindfb || !mg_genrb || !mg_bindrb || !mg_rbstore ||
        !mg_fbrb || !mg_fbstatus || !mg_clearcolor || !mg_readpixels) {
        kv_log("MERGE A/B: framebuffer entry points missing; not run.");
        return;
    }
    a = (unsigned char *)malloc(px * 4);
    b = (unsigned char *)malloc(px * 4);
    if (!a || !b) { free(a); free(b); return; }

    o_glGetIntegerv(MG_FB_BINDING, &prev);
    if (!g_mg_fbo) {
        mg_genfb(1, &g_mg_fbo); mg_genrb(1, &g_mg_rbc); mg_genrb(1, &g_mg_rbd);
        mg_bindrb(MG_RB, g_mg_rbc); mg_rbstore(MG_RB, MG_RGBA8, MG_AB_W, MG_AB_H);
        mg_bindrb(MG_RB, g_mg_rbd); mg_rbstore(MG_RB, MG_DEPTH24, MG_AB_W, MG_AB_H);
        mg_bindfb(MG_FB, g_mg_fbo);
        mg_fbrb(MG_FB, MG_COLOR0, MG_RB, g_mg_rbc);
        mg_fbrb(MG_FB, MG_DEPTH_ATT, MG_RB, g_mg_rbd);
    } else {
        mg_bindfb(MG_FB, g_mg_fbo);
    }
    if (mg_fbstatus(MG_FB) != MG_FB_COMPLETE) {
        kv_log("MERGE A/B: offscreen target incomplete; not run.");
        mg_bindfb(MG_FB, (GLuint)prev);
        free(a); free(b);
        return;
    }
    /* viewport, scissor, clear colour, masks, enables: all put back after */
    o_glPushAttrib(GL_VIEWPORT_BIT | GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT |
                   GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
    o_glViewport(0, 0, MG_AB_W, MG_AB_H);
    o_glDisable(GL_SCISSOR_TEST_);
    o_glMatrixMode(GL_MODELVIEW);
    o_glPushMatrix();

    /* A: the engine's own lists, each at its full matrix */
    mg_clearcolor(0.0f, 0.0f, 0.0f, 0.0f);
    o_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (i = 0; i < g_mg.ninst; i++) {
        float full[16];
        m_mul(full, g_mv[g_view_depth], g_mg.inst[i].obj);
        o_glLoadMatrixf(full);
        o_glCallList(g_mg.inst[i].list);
    }
    mg_readpixels(0, 0, MG_AB_W, MG_AB_H, GL_RGBA, GL_UNSIGNED_BYTE, a);

    /* B: the merged buffer, under the view level */
    o_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    o_glLoadMatrixf(g_mv[g_view_depth]);
    mg_arrays_on();
    o_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_mg.nv);
    mg_arrays_off();
    mg_readpixels(0, 0, MG_AB_W, MG_AB_H, GL_RGBA, GL_UNSIGNED_BYTE, b);

    o_glMatrixMode(GL_MODELVIEW);
    o_glPopMatrix();
    if (g_mode != GL_MODELVIEW) o_glMatrixMode(g_mode);
    o_glPopAttrib();
    mg_bindfb(MG_FB, (GLuint)prev);

    for (i = 0; i < px; i++) {
        const unsigned char *pa = a + i * 4, *pb = b + i * 4;
        int ca = pa[0] | pa[1] | pa[2] | pa[3];
        int cb = pb[0] | pb[1] | pb[2] | pb[3];
        int k, d = 0;
        if (ca) cov_a++;
        if (cb) cov_b++;
        if (!ca != !cb) only++;
        for (k = 0; k < 4; k++) {
            int e = (int)pa[k] - (int)pb[k];
            if (e < 0) e = -e;
            if (e > d) d = e;
        }
        if (d) differ++;
        if ((unsigned)d > maxd) maxd = (unsigned)d;
    }
    kv_log("MERGE A/B: %u instances, %u merged vertices, drawn both ways into "
           "one offscreen target in the same instant with the same state.",
           g_mg.ninst, g_mg.nv);
    kv_log("MERGE A/B: engine's lists cover %u pixels, the merged buffer %u. "
           "%u pixels are covered by only one of them. %u pixels differ at "
           "all, largest channel difference %u of 255.",
           cov_a, cov_b, only, differ, maxd);
    kv_log("MERGE A/B: %s", differ == 0
           ? "IDENTICAL -- the merged group is pixel for pixel the engine's."
           : "they differ; the images are in %TEMP% as keflings_mg_A/B/diff.bmp.");
    mg_bmp("keflings_mg_A.bmp", a, MG_AB_W, MG_AB_H);
    mg_bmp("keflings_mg_B.bmp", b, MG_AB_W, MG_AB_H);
    /* difference, amplified so a one-step change is visible */
    for (i = 0; i < px; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            int e = (int)a[i * 4 + k] - (int)b[i * 4 + k];
            if (e < 0) e = -e;
            e *= 16; if (e > 255) e = 255;
            a[i * 4 + k] = (unsigned char)e;
        }
        a[i * 4 + 3] = 255;
    }
    mg_bmp("keflings_mg_diff.bmp", a, MG_AB_W, MG_AB_H);
    free(a); free(b);
}

/* ---- run 1: every order-independent group, from GPU buffers -------------- */
#define MGG_N    512                 /* groups, keyed (signature, pass) */
#define MGG_SET  4096                /* instance keys per group */
#define MGG_MAXM 2000                /* members above this: not merged */
#define MG_ARRAY_BUFFER          0x8892
#define MG_ARRAY_BUFFER_BINDING  0x8894
#define MG_STATIC_DRAW           0x88E4
enum { MGE_UNDECIDED = 0, MGE_YES, MGE_NO_DEPTH, MGE_NO_BLENDFUNC,
       MGE_NO_MATALPHA, MGE_NO_TEXTURE, MGE_NO_AUDIT, MGE_NO_SIZE,
       MGE_ORDER, MGE_MOVING, MGE_N };
static const char *k_mge[MGE_N] = {
    "undecided", "ELIGIBLE", "depth test or writes off",
    "a blend function other than standard alpha",
    "material alpha below 1", "a cut-out or soft-alpha texture",
    "failed its own state audit", "too many members",
    "cut-out or soft alpha: merged only in mode 7's FULL arm",
    "its members move every frame (animated): not worth a buffer" };

typedef struct {
    unsigned sig; int pass; int used;
    int      elig;
    GLuint   vbo; unsigned nv; int valid;
    int      has_n, has_t, has_c;
    unsigned *set;                   /* instance keys in the buffer */
    unsigned ninbuf;
    MgInst  *inbuf; unsigned capinbuf; /* the instances themselves */
    MgInst  *cur; unsigned ncur, capcur;
    int      drawn;
    /* self-audit, first two frames the group is seen */
    int      aud_frames, aud_have;
    float    aud_ref[22];
    /* verification (mode 4) */
    int      ab_done;
    /* change detection over EVERY member seen, not only those the buffer
       could include: a member excluded at build (different vertex
       attributes, say) made the old "count in buffer == count seen" test
       fail every frame, and the group rebuilt 300 times in 300 frames. */
    unsigned built_sum, built_n;
    unsigned rebuilds, consec;
    /* the union design -- see tools/merge_union.py */
    BaVert   *cpu; unsigned cpu_nv, cpu_cap;
    unsigned  vbo_cap;
    MgInst   *uin; unsigned *ufirst, *ucount, *ustamp;
    unsigned  nuni, capuni;
    unsigned *uhash;                 /* MGG_SET slots holding index + 1 */
    unsigned *didx; GLint *dfirst; GLsizei *dcount;
    unsigned  ndraw, capdraw, dfid;
    GLuint    movers[32]; unsigned nmovers;   /* lists caught moving */
    unsigned  appends;
    int       attrs_set;
} MgGroup;
static MgGroup  g_mgg[MGG_N];
static unsigned g_mgg_used;
static BaVert  *g_mgg_scratch; static unsigned g_mgg_scap;
static void (APIENTRY *mgg_genbuf)(GLsizei, GLuint *);
static void (APIENTRY *mgg_bindbuf)(GLenum, GLuint);
static void (APIENTRY *mgg_bufdata)(GLenum, ptrdiff_t, const GLvoid *, GLenum);
static void (APIENTRY *mgg_bufsub)(GLenum, ptrdiff_t, ptrdiff_t, const GLvoid *);
static void (APIENTRY *mgg_multidraw)(GLenum, const GLint *, const GLsizei *, GLsizei);
static unsigned g_mgg_fid = 1;       /* frame id for instance stamps */
/* OVERLAID instances (24 Sep 2026). The game draws a second layer exactly
   over most objects -- the snow on them, a highlight -- by calling the same
   list at the same matrix again in another state. A merged draw computes
   the object's positions on the CPU (world space, then the view matrix)
   where the engine lets the GPU multiply view x object, so depths differ in
   the last bits and a layer drawn exactly on top z-fights: in the snow,
   every merged tree and crystal flickered in the headset, and at the desk
   4.7% of the screen changed where two plain frames differ by 0.16%.
   While the layer is invisible the no-op skip drops it and nothing shows.
   So: an instance that is drawn again, UNMERGED, in the frame it was a
   merge member is marked overlaid, and stays out of every merge for as
   long as its layer keeps drawing -- both layers then take the engine's
   own path and meet exactly. When the layer goes invisible again the mark
   lapses after MGG_OV_HOLD frames and the object merges again. */
#define MGG_OV_N    16384
#define MGG_OV_PROBE 32
#define MGG_OV_HOLD 3
typedef struct { unsigned key, base_fid, ov_fid, fade_fid; } MgOv;
static MgOv     g_mgg_ov[MGG_OV_N];
static unsigned g_mggw_overlaid;     /* member draws kept out, this window */
static unsigned g_mggw_ov_new;       /* instances newly marked, this window */
static MgOv *mgg_ov_slot(unsigned key, int make) {
    unsigned i, h = (key * 2654435761u) & (MGG_OV_N - 1);
    MgOv *spare = NULL;
    for (i = 0; i < MGG_OV_PROBE; i++) {
        MgOv *e = &g_mgg_ov[(h + i) & (MGG_OV_N - 1)];
        if (e->key == key) return e;
        if (!spare && (e->key == 0 ||
                       (g_mgg_fid - e->base_fid > 600u && g_mgg_fid - e->ov_fid > 600u &&
                        g_mgg_fid - e->fade_fid > 600u)))
            spare = e;
    }
    if (!make || !spare) return NULL;
    spare->key = key; spare->base_fid = 0; spare->ov_fid = 0; spare->fade_fid = 0;
    return spare;
}
/* a merge member this frame, whether or not it ends up merged */
static void mgg_ov_base(unsigned key) {
    MgOv *e = mgg_ov_slot(key, 1);
    if (e) e->base_fid = g_mgg_fid;
}
/* drawn again this frame by the engine's own path */
static void mgg_ov_again(unsigned key) {
    MgOv *e = mgg_ov_slot(key, 0);
    if (e && e->base_fid == g_mgg_fid) {
        if (g_mgg_fid - e->ov_fid > MGG_OV_HOLD) g_mggw_ov_new++;
        e->ov_fid = g_mgg_fid;
    }
}
/* FADING instances (24 Sep 2026). A merged draw gives every member the
   transparency of the state it is drawn with; the engine fades objects
   in and out one by one (the far hills, after a panel closes), so a
   member caught mid-fade is drawn by the engine, alone, until its fade is
   over and it is back at full opacity. In the seasons soak the far hills
   came out a shade brighter once in 83 still checks -- merged opaque, while
   the engine blended them over the sky. */
static unsigned g_mggw_fading;       /* member draws kept out for a fade, this window */
static float    g_mggw_fade_min = 1.0f;
static unsigned g_mggw_fade_grp;
static void mgg_fade_mark(unsigned key) {
    MgOv *e = mgg_ov_slot(key, 1);
    if (e) e->fade_fid = g_mgg_fid;
}
static int mgg_fade_active(unsigned key) {
    MgOv *e = mgg_ov_slot(key, 0);
    return e && e->fade_fid && g_mgg_fid - e->fade_fid <= MGG_OV_HOLD;
}
static int mgg_ov_active(unsigned key) {
    MgOv *e;
    /* merge_offset puts merged scenery a few depth steps back, so ANY layer
       drawn exactly on top wins -- the same list or a separate snow mesh --
       and nothing needs keeping out. The exclusion is the fallback for 0. */
    if (g_vrcfg.merge_offset != 0.0f) return 0;
    e = mgg_ov_slot(key, 0);
    return e && e->ov_fid && g_mgg_fid - e->ov_fid <= MGG_OV_HOLD;
}
#define MGG_MAXU 4000                /* instances a group's buffer may hold */
static unsigned g_mggw_appends, g_mggw_movers, g_mggw_ghosts;
static int      g_mgg_ok = -1;       /* -1 untried, 0 failed, 1 ready */
static int      g_mgg_on;            /* merging active THIS frame */
static int      g_mgg_arm;           /* 0 off, 1 safe, 2 full (mode 7) */
#define MGG_ELIG(g) ((g)->elig == MGE_YES ||                      ((g)->elig == MGE_ORDER && (g_vrcfg.merge_group == 7 || g_vrcfg.merge_group == 9)))
/* window statistics */
static unsigned g_mggw_frames, g_mggw_skipped, g_mggw_merged, g_mggw_rebuilds,
                g_mggw_elig_draws, g_mggw_draws, g_mggw_notbuf;
static unsigned g_mgg_why[MGE_N];    /* groups, by verdict */
/* live A/B: frame time by arm */
static double   g_mgg_arm_ms[3]; static unsigned g_mgg_arm_n[3];
/* 24 Sep: the first version of this timer was WRONG. It switched arms on
   system uptime from process start, so whichever arm was active during the
   loading screen -- 6,000-9,700 fps, ~0.1 ms a frame -- swallowed thousands
   of near-free frames and looked far faster. Mode 3 "291 vs 172 fps" was
   that artifact; mode 6 came out inverted when loading fell in the other
   arm. Now: only frames that drew the WORLD count, the first 10 s of world
   are skipped (one-time uploads and texture readbacks land there), the 5
   frames after every switch are skipped, and flat runs alternate every
   second so a drifting menu camera cannot favour one arm. */
static unsigned g_mgg_frame_draws;       /* world draws this frame */
static DWORD    g_mgg_world_t0;          /* tick the world first drew */
static DWORD    g_mgg_dense_t0;          /* tick the DENSE world first drew:
                                            the menu is ~800 draws, a
                                            busy world 2,000-3,500 */
static unsigned g_mgg_since_switch;
static double   g_mggw_arm_ms[3]; static unsigned g_mggw_arm_n[3];  /* window */
/* desk verification tallies */
static unsigned g_mgv_tested, g_mgv_identical, g_mgv_translucent,
                g_mgv_equivalent, g_mgv_worst_step;

static int mgg_ready(void) {
    if (g_mgg_ok >= 0) return g_mgg_ok;
    g_mgg_ok = 0;
    if (!ba_procs_ready() || !o_wglGetProcAddress) return 0;
    mgg_genbuf  = (void *)o_wglGetProcAddress("glGenBuffers");
    mgg_bindbuf = (void *)o_wglGetProcAddress("glBindBuffer");
    mgg_bufdata = (void *)o_wglGetProcAddress("glBufferData");
    mgg_bufsub  = (void *)o_wglGetProcAddress("glBufferSubData");
    mgg_multidraw = (void *)o_wglGetProcAddress("glMultiDrawArrays");
    if (!mgg_genbuf || !mgg_bindbuf || !mgg_bufdata || !mgg_bufsub ||
        !mgg_multidraw) {
        kv_log("MERGE: buffer-object entry points missing; nothing merged.");
        return 0;
    }
    g_mgg_ok = 1;
    return 1;
}

static MgGroup *mgg_find(unsigned sig, int pass) {
    unsigned i = ((sig * 2654435761u) ^ ((unsigned)pass * 0x9E3779B9u)) &
                 (MGG_N - 1), t;
    for (t = 0; t < MGG_N; t++, i = (i + 1) & (MGG_N - 1)) {
        if (g_mgg[i].used && g_mgg[i].sig == sig && g_mgg[i].pass == pass)
            return &g_mgg[i];
        if (!g_mgg[i].used) {
            if (g_mgg_used >= MGG_N * 3 / 4) return NULL;
            g_mgg[i].used = 1; g_mgg[i].sig = sig; g_mgg[i].pass = pass;
            g_mgg_used++;
            return &g_mgg[i];
        }
    }
    return NULL;
}

/* A texture's alpha class, read back once per texture, not once per group. */
#define MGT_N 1024
static struct { GLuint id; int cls; } g_mgt[MGT_N];
static int mgg_texclass(GLuint id) {
    unsigned i = (id * 2654435761u) & (MGT_N - 1), t;
    for (t = 0; t < MGT_N; t++, i = (i + 1) & (MGT_N - 1)) {
        if (g_mgt[i].id == id) return g_mgt[i].cls;
        if (g_mgt[i].id == 0) {
            float pf;
            g_mgt[i].id = id;
            g_mgt[i].cls = ta_classify(id, &pf);
            return g_mgt[i].cls;
        }
    }
    return TA_UNKNOWN;
}

/* Decided once per group: what makes reordering invisible. */
static int mgg_decide(void) {
    int blend = g_gk_have[GKS_CAP(0)] && g_gk_val[GKS_CAP(0)];
    int dtest = g_gk_have[GKS_CAP(5)] && g_gk_val[GKS_CAP(5)];
    int dmask = !g_gk_have[GKS_DMASK] || g_gk_val[GKS_DMASK];
    /* The depth flags are TRACKED, and until 24 Sep had never been checked
       against GL the way the blend flag was. Ask GL once per group, say when
       they disagree, and decide on GL's answer. */
    {
        static unsigned said;
        GLint wm = 1;
        int rt = o_glIsEnabled ? (o_glIsEnabled(0x0B71) ? 1 : 0) : dtest;
        o_glGetIntegerv(0x0B72, &wm);        /* GL_DEPTH_WRITEMASK */
        if ((rt != dtest || (wm != 0) != dmask) && said < 12) {
            said++;
            kv_log("MERGE: depth flags for group %08X -- tracked test %d mask "
                   "%d, GL says test %d mask %d. Deciding on GL's answer.",
                   g_gk_sig, dtest, dmask, rt, wm != 0);
        }
        dtest = rt; dmask = wm != 0;
    }
    if (!dtest || !dmask) {
        static unsigned said2;
        if (said2 < 12) {
            said2++;
            kv_log("MERGE: group %08X is genuinely depth test %d, writes %d "
                   "(blend %d, src 0x%04X dst 0x%04X)", g_gk_sig, dtest, dmask,
                   blend, g_gk_val[GKS_BSRC], g_gk_val[GKS_BDST]);
        }
        return MGE_NO_DEPTH;
    }
    if (!blend) return MGE_YES;
    if (g_gk_val[GKS_BSRC] != 0x0302 || g_gk_val[GKS_BDST] != 0x0303)
        return MGE_NO_BLENDFUNC;
    {
        int k = SF_KEY(GL_DIFFUSE);
        if (SF_OK(g_sf_mat[k]) && g_sf_mat[k].pname == GL_DIFFUSE &&
            g_sf_mat[k].iv == 0x7FFFFFFF && g_sf_mat[k].v[3] < 0.999f)
            return MGE_NO_MATALPHA;
    }
    {
        GLint id = 0;
        o_glGetIntegerv(0x8069, &id);
        if (!id) return MGE_NO_TEXTURE;
        if (mgg_texclass((GLuint)id) != TA_OPAQUE) return MGE_ORDER;
    }
    return MGE_YES;
}

/* DIAGNOSTIC (diagnostics = 1): the whole fixed-function state that could
   differ between two draws of one merged group, read from GL on EVERY member
   draw and compared with what the group looked like the first time. The
   self-audit checks five items for two frames; in the snow (24 Sep 2026)
   merged objects lost their snow, so something per object changed later
   that the group key and the audit do not see. This names it. */
#define PS_N 96
static const char *k_ps_name[PS_N];
static float    g_ps_ref[MGG_N][PS_N];
static unsigned char g_ps_have[MGG_N];
static unsigned g_ps_fid[MGG_N];         /* frame the reference was taken in */
static unsigned g_ps_diff[PS_N];         /* mismatching member draws, per field */
static unsigned g_ps_draws, g_ps_logged;
static void (APIENTRY *ps_getenviv)(GLenum, GLenum, GLint *);
static void (APIENTRY *ps_getenvfv)(GLenum, GLenum, GLfloat *);
static void (APIENTRY *ps_getgeniv)(GLenum, GLenum, GLint *);
static int ps_read(float *v) {
    static char names[PS_N][24];
    int n = 0, u, l;
    GLint act = 0x84C0, iv = 0;
    GLfloat f[16];
#define PSV(nm, x) do { if (!k_ps_name[n]) { strcpy(names[n], nm); k_ps_name[n] = names[n]; } v[n++] = (float)(x); } while (0)
    if (!ps_getenviv) {
        ps_getenviv = (void *)proxy_real_proc("glGetTexEnviv");
        ps_getenvfv = (void *)proxy_real_proc("glGetTexEnvfv");
        ps_getgeniv = (void *)proxy_real_proc("glGetTexGeniv");
    }
    if (!ps_getenviv || !ps_getenvfv || !ps_getgeniv || !o_glGetMaterialfv) return 0;
    o_glGetFloatv(0x0B00, f);
    PSV("colour r", f[0]); PSV("colour g", f[1]); PSV("colour b", f[2]); PSV("colour a", f[3]);
    /* the current normal and texcoord: a mesh WITHOUT that array is drawn
       with whatever the previous draw left, per member in the engine's
       order, but once for the whole group when merged */
    o_glGetFloatv(0x0B02, f);
    PSV("normal x", f[0]); PSV("normal y", f[1]); PSV("normal z", f[2]);
    o_glGetFloatv(0x0B03, f);
    PSV("texcoord s", f[0]); PSV("texcoord t", f[1]);
    o_glGetMaterialfv(GL_FRONT, 0x1200, f);
    PSV("mat ambient r", f[0]); PSV("mat ambient g", f[1]); PSV("mat ambient b", f[2]); PSV("mat ambient a", f[3]);
    o_glGetMaterialfv(GL_FRONT, 0x1201, f);
    PSV("mat diffuse r", f[0]); PSV("mat diffuse g", f[1]); PSV("mat diffuse b", f[2]); PSV("mat diffuse a", f[3]);
    o_glGetMaterialfv(GL_FRONT, 0x1600, f);
    PSV("mat emission r", f[0]); PSV("mat emission g", f[1]); PSV("mat emission b", f[2]);
    o_glGetMaterialfv(GL_FRONT, 0x1202, f);
    PSV("mat specular r", f[0]); PSV("mat specular g", f[1]); PSV("mat specular b", f[2]);
    o_glGetMaterialfv(GL_FRONT, 0x1601, f);
    PSV("mat shininess", f[0]);
    PSV("lighting", o_glIsEnabled(0x0B50)); PSV("colour material", o_glIsEnabled(0x0B57));
    o_glGetIntegerv(0x0B56, &iv); PSV("colmat mode", iv);
    PSV("fog", o_glIsEnabled(0x0B60)); PSV("alpha test", o_glIsEnabled(0x0BC0));
    PSV("blend", o_glIsEnabled(0x0BE2)); PSV("normalize", o_glIsEnabled(0x0BA1));
    PSV("cull face", o_glIsEnabled(0x0B44));
    o_glGetFloatv(0x0B53, f);
    PSV("scene ambient r", f[0]); PSV("scene ambient g", f[1]); PSV("scene ambient b", f[2]);
    o_glGetIntegerv(0x0B65, &iv); PSV("fog mode", iv);
    o_glGetFloatv(0x0B62, f); PSV("fog density", f[0]);
    o_glGetFloatv(0x0B63, f); PSV("fog start", f[0]);
    o_glGetFloatv(0x0B64, f); PSV("fog end", f[0]);
    o_glGetFloatv(0x0B66, f); PSV("fog colour r", f[0]); PSV("fog colour g", f[1]);
    o_glGetIntegerv(0x8450, &iv); PSV("fog coord source", iv);
    PSV("fog coord array", o_glIsEnabled(0x8457));
    PSV("colour sum", o_glIsEnabled(0x8458));
    PSV("secondary colour array", o_glIsEnabled(0x845E));
    o_glGetIntegerv(0x0C50, &iv); PSV("fog hint", iv);
    PSV("fog rescale normal", o_glIsEnabled(0x803A));   /* 'fog' prefix: logged per group */
    o_glGetIntegerv(0x0B51, &iv); PSV("fog lm local viewer", iv);
    o_glGetIntegerv(0x0B52, &iv); PSV("fog lm two side", iv);
    o_glGetIntegerv(0x81F8, &iv); PSV("fog lm colour control", iv);
    PSV("fog normalize", o_glIsEnabled(0x0BA1));
    {   /* the member's object scale: column lengths of its object matrix */
        const float *om = g_ob[g_mv_sp];
        PSV("fog obj scale x", sqrt(om[0] * om[0] + om[1] * om[1] + om[2] * om[2]));
        PSV("fog obj scale y", sqrt(om[4] * om[4] + om[5] * om[5] + om[6] * om[6]));
        PSV("fog obj scale z", sqrt(om[8] * om[8] + om[9] * om[9] + om[10] * om[10]));
    }
    for (l = 0; l < 4; l++) {
        char nm[24];
        sprintf(nm, "light%d on", l); PSV(nm, o_glIsEnabled(0x4000 + l));
        o_glGetLightfv(0x4000 + l, 0x1201, f);
        sprintf(nm, "light%d diffuse r", l); PSV(nm, f[0]);
        o_glGetLightfv(0x4000 + l, 0x1200, f);
        sprintf(nm, "light%d ambient r", l); PSV(nm, f[0]);
    }
    o_glGetIntegerv(0x84E0, &act);
    for (u = 0; u < 3; u++) {
        char nm[24]; int k; float s = 0;
        if (sf_active_next) sf_active_next(0x84C0 + u);
        sprintf(nm, "u%d texture2D", u); PSV(nm, o_glIsEnabled(0x0DE1));
        o_glGetIntegerv(0x8069, &iv); sprintf(nm, "u%d binding", u); PSV(nm, iv);
        ps_getenviv(0x2300, 0x2200, &iv); sprintf(nm, "u%d env mode", u); PSV(nm, iv);
        ps_getenvfv(0x2300, 0x2201, f);
        sprintf(nm, "u%d env colour r", u); PSV(nm, f[0]);
        sprintf(nm, "u%d env colour a", u); PSV(nm, f[3]);
        ps_getenviv(0x2300, 0x8571, &iv); sprintf(nm, "u%d combine rgb", u); PSV(nm, iv);
        sprintf(nm, "u%d texgen S", u); PSV(nm, o_glIsEnabled(0x0C60));
        sprintf(nm, "u%d texgen T", u); PSV(nm, o_glIsEnabled(0x0C61));
        ps_getgeniv(0x2000, 0x2500, &iv); sprintf(nm, "u%d texgen S mode", u); PSV(nm, iv);
        o_glGetFloatv(0x0BA8, f);
        for (k = 0; k < 16; k++) s += f[k] * (float)(k + 1);
        sprintf(nm, "u%d tex matrix", u); PSV(nm, s);
    }
    if (sf_active_next) sf_active_next((GLenum)act);
#undef PSV
    return n;
}
static void mgg_state_probe(const MgGroup *g) {
    float v[PS_N];
    int gi = (int)(g - g_mgg), n, k;
    g_internal++;
    n = ps_read(v);
    g_internal--;
    if (!n) return;
    g_ps_draws++;
    /* The merged draw runs with the state of the group's FIRST member of
       THIS frame, so that is what every later member must match; a state
       that drifts across frames (the day's light) is not a fault. */
    if (g_ps_have[gi] && g_ps_fid[gi] != g_mgg_fid) {
        memcpy(g_ps_ref[gi], v, sizeof(float) * n);
        g_ps_fid[gi] = g_mgg_fid;
        return;
    }
    if (!g_ps_have[gi]) {
        g_ps_fid[gi] = g_mgg_fid;
        static unsigned said;
        memcpy(g_ps_ref[gi], v, sizeof(float) * n); g_ps_have[gi] = 1;
        if (said < 24) {   /* the fog and extra-array state each group is drawn with */
            char line[512]; int len = 0;
            said++;
            for (k = 0; k < n && len < 480; k++)
                if (strncmp(k_ps_name[k], "fog", 3) == 0 || strstr(k_ps_name[k], "array") ||
                    strcmp(k_ps_name[k], "colour sum") == 0)
                    len += sprintf(line + len, "%s=%g ", k_ps_name[k], v[k]);
            kv_log("STATE PROBE: group %08X first member (arrays: normals %d, "
                   "texcoords %d, colours %d): %s", g->sig, g->has_n, g->has_t,
                   g->has_c, line);
        }
        return;
    }
    for (k = 0; k < n; k++) {
        float d = v[k] - g_ps_ref[gi][k];
        if (d > 1e-4f || d < -1e-4f) {
            g_ps_diff[k]++;
            if (g_ps_logged < 40) {
                g_ps_logged++;
                kv_log("STATE PROBE: merged group %08X, frame %u: '%s' was %g on the "
                       "group's first member THIS FRAME, is %g on this member", g->sig, g_frames,
                       k_ps_name[k], g_ps_ref[gi][k], v[k]);
            }
        }
    }
}
static float ps_val(unsigned gi, const char *nm) {
    int k;
    for (k = 0; k < PS_N && k_ps_name[k]; k++)
        if (strcmp(k_ps_name[k], nm) == 0) return g_ps_ref[gi][k];
    return -1.0f;
}
static void mgg_state_report(void) {
    int k, any = 0;
    {   /* which merged groups draw WITHOUT their own normals / texcoords /
           colours, and so depend on what the previous draw left */
        unsigned gi; int said = 0;
        for (gi = 0; gi < MGG_N; gi++) {
            const MgGroup *g = &g_mgg[gi];
            if (!g->used || !g->valid || !g_ps_have[gi]) continue;
            if (said++ >= 16) break;
            kv_log("      STATE PROBE: group %08X, %u instances: normals %d texcoords %d "
                   "colours %d | lighting %g colour material %g texture u0 %g",
                   g->sig, g->nuni, g->has_n, g->has_t, g->has_c,
                   ps_val(gi, "lighting"), ps_val(gi, "colour material"), ps_val(gi, "u0 texture2D"));
        }
    }
    if (!g_ps_draws) return;
    for (k = 0; k < PS_N; k++)
        if (g_ps_diff[k]) {
            if (!any) kv_log("      STATE PROBE: of %u merged-member draws, these fields "
                             "differed from the group's first member:", g_ps_draws);
            any = 1;
            kv_log("      STATE PROBE:   %-20s %u draws", k_ps_name[k], g_ps_diff[k]);
        }
    if (!any) kv_log("      STATE PROBE: %u merged-member draws, every field matched "
                     "the group's first member", g_ps_draws);
    memset(g_ps_diff, 0, sizeof(g_ps_diff)); g_ps_draws = 0;
}

/* The state the meshes do not supply as arrays, for the self-audit. */
static void mgg_audit_read(float *v) {
    static const GLenum caps[4] = { 0x0B50, 0x0B57, 0x0B60, 0x0DE1 };
    o_glGetFloatv(MG_CURRENT_COLOR, v + 0);
    o_glGetFloatv(MG_TEXTURE_MATRIX, v + 4);
    v[20] = (o_glIsEnabled(caps[0]) ? 1.0f : 0.0f) + (o_glIsEnabled(caps[1]) ? 2.0f : 0.0f);
    v[21] = (o_glIsEnabled(caps[2]) ? 1.0f : 0.0f) + (o_glIsEnabled(caps[3]) ? 2.0f : 0.0f);
}

static int mgg_ulookup(const MgGroup *g, unsigned key) {
    unsigned i = (key * 2654435761u) & (MGG_SET - 1), t;
    if (!g->uhash) return -1;
    for (t = 0; t < 64; t++, i = (i + 1) & (MGG_SET - 1)) {
        unsigned v = g->uhash[i];
        if (!v) return -1;
        if (g->uin[v - 1].key == key) return (int)(v - 1);
    }
    return -1;
}
static void mgg_uinsert(MgGroup *g, unsigned key, unsigned idx) {
    unsigned i = (key * 2654435761u) & (MGG_SET - 1), t;
    for (t = 0; t < 64; t++, i = (i + 1) & (MGG_SET - 1))
        if (!g->uhash[i]) { g->uhash[i] = idx + 1; return; }
}
static int mgg_is_mover(const MgGroup *g, GLuint list) {
    unsigned i;
    for (i = 0; i < g->nmovers; i++) if (g->movers[i] == list) return 1;
    return 0;
}
/* Emit one new instance into the group's CPU copy. -1 = cannot merge this
   one, -2 = the group is full. Uploading is the caller's, once per frame. */
static int mgg_uappend(MgGroup *g, const MgInst *in) {
    Mesh *me = mc_find(in->list, 0);
    unsigned nv0 = g->cpu_nv, idx;
    int ok;
    if (!me || !me->ok || !me->mergeable || me->gen != in->gen) return -1;
    if (g->nuni >= MGG_MAXU) return -2;
    if (!g->attrs_set) {
        g->has_n = me->has_n; g->has_t = me->has_t; g->has_c = me->has_c;
        g->attrs_set = 1;
    } else if (me->has_n != g->has_n || me->has_t != g->has_t ||
               me->has_c != g->has_c) return -1;
    if (!g->uhash) {
        g->uhash = (unsigned *)calloc(MGG_SET, sizeof(unsigned));
        if (!g->uhash) return -1;
    }
    if (g->nuni >= g->capuni) {
        unsigned want = g->capuni ? g->capuni * 2 : 64;
        MgInst   *a = (MgInst *)realloc(g->uin, want * sizeof(MgInst));
        unsigned *b, *c, *d;
        if (!a) return -1;
        g->uin = a;
        b = (unsigned *)realloc(g->ufirst, want * sizeof(unsigned));
        if (!b) return -1;
        g->ufirst = b;
        c = (unsigned *)realloc(g->ucount, want * sizeof(unsigned));
        if (!c) return -1;
        g->ucount = c;
        d = (unsigned *)realloc(g->ustamp, want * sizeof(unsigned));
        if (!d) return -1;
        g->ustamp = d;
        g->capuni = want;
    }
    /* the stage 3 emitter writes to g_mg.vb: lend it this group's copy */
    g_mg.vb = g->cpu; g_mg.nv = g->cpu_nv; g_mg.capv = g->cpu_cap;
    ok = mg_emit(me, in->obj);
    g->cpu = g_mg.vb; g->cpu_cap = g_mg.capv;
    g->cpu_nv = ok ? g_mg.nv : nv0;
    g_mg.vb = NULL; g_mg.nv = 0; g_mg.capv = 0;
    if (!ok || g->cpu_nv == nv0) return -1;
    idx = g->nuni++;
    g->uin[idx] = *in;
    g->ufirst[idx] = nv0;
    g->ucount[idx] = g->cpu_nv - nv0;
    g->ustamp[idx] = 0;
    mgg_uinsert(g, in->key, idx);
    g->appends++; g_mggw_appends++;
    return (int)idx;
}
/* Everything appended since nv0 reaches the GPU in one call; a buffer that
   has outgrown its allocation is reallocated at double and sent whole. */
static void mgg_upload_from(MgGroup *g, unsigned nv0) {
    GLint prev = 0;
    if (g->cpu_nv <= nv0) return;
    if (!g->vbo) mgg_genbuf(1, &g->vbo);
    o_glGetIntegerv(MG_ARRAY_BUFFER_BINDING, &prev);
    mgg_bindbuf(MG_ARRAY_BUFFER, g->vbo);
    if (g->cpu_nv > g->vbo_cap) {
        /* grow by half, not double, and from the size actually needed */
        unsigned cap = g->cpu_nv + g->cpu_nv / 2 + 1024;
        mgg_bufdata(MG_ARRAY_BUFFER, (ptrdiff_t)(cap * sizeof(BaVert)), NULL,
                    MG_STATIC_DRAW);
        mgg_bufsub(MG_ARRAY_BUFFER, 0, (ptrdiff_t)(g->cpu_nv * sizeof(BaVert)),
                   g->cpu);
        g->vbo_cap = cap;
    } else {
        mgg_bufsub(MG_ARRAY_BUFFER, (ptrdiff_t)(nv0 * sizeof(BaVert)),
                   (ptrdiff_t)((g->cpu_nv - nv0) * sizeof(BaVert)),
                   g->cpu + nv0);
    }
    mgg_bindbuf(MG_ARRAY_BUFFER, (GLuint)prev);
}

static int mgg_set_has(const MgGroup *g, unsigned key) {
    unsigned i = (key * 2654435761u) & (MGG_SET - 1), t;
    if (!g->set) return 0;
    for (t = 0; t < 64; t++, i = (i + 1) & (MGG_SET - 1)) {
        if (g->set[i] == key) return 1;
        if (g->set[i] == 0) return 0;
    }
    return 0;
}
static void mgg_set_add(MgGroup *g, unsigned key) {
    unsigned i = (key * 2654435761u) & (MGG_SET - 1), t;
    for (t = 0; t < 64; t++, i = (i + 1) & (MGG_SET - 1)) {
        if (g->set[i] == key) return;
        if (g->set[i] == 0) { g->set[i] = key; return; }
    }
}

/* Rebuild a group's GPU buffer from this frame's members. Reuses the stage
   3 emitter, which writes into g_mg.vb; its contents are then uploaded. */
static void mgg_build(MgGroup *g) {
    unsigned i, first = 1, n = 0;
    GLint prev = 0;
    g->valid = 0; g->ninbuf = 0; g->nv = 0;
    if (!g->set) g->set = (unsigned *)calloc(MGG_SET, sizeof(unsigned));
    if (!g->set) return;
    memset(g->set, 0, MGG_SET * sizeof(unsigned));
    g_mg.nv = 0;
    for (i = 0; i < g->ncur; i++) {
        const MgInst *in = &g->cur[i];
        Mesh *me = mc_find(in->list, 0);
        unsigned nv0 = g_mg.nv;
        if (!me || !me->ok || !me->mergeable || me->gen != in->gen) continue;
        if (first) {
            g->has_n = me->has_n; g->has_t = me->has_t; g->has_c = me->has_c;
            first = 0;
        } else if (me->has_n != g->has_n || me->has_t != g->has_t ||
                   me->has_c != g->has_c) continue;
        if (!mg_emit(me, in->obj)) { g_mg.nv = nv0; continue; }
        if (n >= g->capinbuf) {
            unsigned want = g->capinbuf ? g->capinbuf * 2 : 64;
            MgInst *nb = (MgInst *)realloc(g->inbuf, want * sizeof(MgInst));
            if (!nb) { g_mg.nv = nv0; break; }
            g->inbuf = nb; g->capinbuf = want;
        }
        g->inbuf[n] = *in;
        mgg_set_add(g, in->key);
        n++;
    }
    if (n < 2 || !g_mg.nv) return;              /* nothing worth merging */
    if (!g->vbo) mgg_genbuf(1, &g->vbo);
    o_glGetIntegerv(MG_ARRAY_BUFFER_BINDING, &prev);
    mgg_bindbuf(MG_ARRAY_BUFFER, g->vbo);
    mgg_bufdata(MG_ARRAY_BUFFER, (ptrdiff_t)(g_mg.nv * sizeof(BaVert)),
                g_mg.vb, MG_STATIC_DRAW);
    mgg_bindbuf(MG_ARRAY_BUFFER, (GLuint)prev);
    g->nv = g_mg.nv; g->ninbuf = n; g->valid = 1;
    g_mggw_rebuilds++;
}

/* Client state for a draw from the group's buffer: every other array off,
   pointers as offsets into it. The caller restores. */
static void mgg_arrays_on(const MgGroup *g, GLint *prev) {
    int u;
    o_glGetIntegerv(MG_ARRAY_BUFFER_BINDING, prev);
    ba_pushc(MG_CLIENT_ARRAY_BIT);
    if (ba_clientactive)
        for (u = 3; u >= 0; u--) {
            ba_clientactive(GL_TEXTURE0_ARB_ + u);
            ba_disablecs(GL_TEXTURE_COORD_ARRAY_);
        }
    ba_disablecs(GL_INDEX_ARRAY_);
    ba_disablecs(GL_EDGE_FLAG_ARRAY_);
    mgg_bindbuf(MG_ARRAY_BUFFER, g->vbo);
    ba_enablecs(GL_VERTEX_ARRAY_);
    ba_vp(3, GL_FLOAT, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, pos));
    if (g->has_n) { ba_enablecs(GL_NORMAL_ARRAY_);
                    ba_np(GL_FLOAT, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, nrm)); }
    else ba_disablecs(GL_NORMAL_ARRAY_);
    if (g->has_t) { ba_enablecs(GL_TEXTURE_COORD_ARRAY_);
                    ba_tp(2, GL_FLOAT, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, tc)); }
    if (g->has_c) { ba_enablecs(GL_COLOR_ARRAY_);
                    ba_cp(4, GL_UNSIGNED_BYTE, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, col)); }
    else ba_disablecs(GL_COLOR_ARRAY_);
}
static void mgg_arrays_off(GLint prev) {
    ba_popc();
    mgg_bindbuf(MG_ARRAY_BUFFER, (GLuint)prev);
}

static void mgg_draw(const MgGroup *g) {
    int eye, n;
    GLint prev = 0;
    (void)eye;
    o_glMatrixMode(GL_MODELVIEW);
    o_glPushMatrix();
    o_glLoadMatrixf(g_mv[g_view_depth]);
    mgg_arrays_on(g, &prev);
    if (g_vrcfg.merge_offset != 0.0f) {
        static void (APIENTRY *po)(GLfloat, GLfloat);
        if (!po) po = (void *)proxy_real_proc("glPolygonOffset");
        /* the engine's own offset state is not tracked: save and restore it */
        o_glPushAttrib(GL_POLYGON_BIT);
        o_glEnable(GL_POLYGON_OFFSET_FILL);
        /* Constant steps only, no slope factor. Measured 24 Sep on a
           snowy save, merged frame against unmerged, beside two unmerged
           frames as the control: 4.7% before; slope 1 + 1 step 0.36/0.19
           (object edges moved on the ground); 1 step 1.37/0.43; 4 steps
           0.28/0.15; 16 steps 0.13/0.21 -- below the control. On a
           no-snow quicksave 16 steps: 0.078/0.062. 16 steps is about a
           hundredth of a game unit at 1000 units away. */
        if (po) po(0.0f, g_vrcfg.merge_offset);
    }
    if (g_vrcfg.diagnostics == 2) {
        /* DIAGNOSTIC: paint every merged draw flat magenta, to see exactly
           which pixels the merge owns ("find the painter first") */
        o_glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
        o_glDisable(GL_TEXTURE_2D); o_glDisable(GL_LIGHTING);
        o_glDisable(0x0B57); o_glDisable(0x0B60); o_glDisable(0x0BE2);
        o_glDisable(0x0BC0);
        o_glDisableClientState(GL_COLOR_ARRAY);
        o_glColor4ub(255, 0, 255, 255);
    }
    if (dup_active()) {
        n = pass_eye_count();
        DUP_LOOP(n, mgg_multidraw(GL_TRIANGLES, g->dfirst, g->dcount,
                                  (GLsizei)g->ndraw));
    } else {
        mgg_multidraw(GL_TRIANGLES, g->dfirst, g->dcount, (GLsizei)g->ndraw);
    }
    if (g_vrcfg.diagnostics == 2) o_glPopAttrib();
    if (g_vrcfg.merge_offset != 0.0f) o_glPopAttrib();
    mgg_arrays_off(prev);
    o_glMatrixMode(GL_MODELVIEW);
    o_glPopMatrix();
    if (g_mode != GL_MODELVIEW) o_glMatrixMode(g_mode);
    g_mggw_merged++;
}

/* Mode 4: the stage 3 pixel A/B, for one group, from its GPU buffer. Also
   checks the group is genuinely OPAQUE: into a target cleared to alpha 0,
   standard alpha blending leaves alpha = src^2, so a covered pixel below
   255 means a translucent fragment got through the eligibility rule. */
static void mgg_verify(MgGroup *g) {
    unsigned char *a, *b;
    GLint prev = 0, pab = 0;
    unsigned i, differ = 0, cov = 0, trans = 0;
    size_t px = (size_t)MG_AB_W * MG_AB_H;
    int blend = g_gk_have[GKS_CAP(0)] && g_gk_val[GKS_CAP(0)];
    g->ab_done = 1;
    if (!mg_genfb && o_wglGetProcAddress) {
        mg_genfb    = (void *)o_wglGetProcAddress("glGenFramebuffers");
        mg_bindfb   = (void *)o_wglGetProcAddress("glBindFramebuffer");
        mg_genrb    = (void *)o_wglGetProcAddress("glGenRenderbuffers");
        mg_bindrb   = (void *)o_wglGetProcAddress("glBindRenderbuffer");
        mg_rbstore  = (void *)o_wglGetProcAddress("glRenderbufferStorage");
        mg_fbrb     = (void *)o_wglGetProcAddress("glFramebufferRenderbuffer");
        mg_fbstatus = (void *)o_wglGetProcAddress("glCheckFramebufferStatus");
    }
    if (!mg_clearcolor) mg_clearcolor = (void *)proxy_real_proc("glClearColor");
    if (!mg_readpixels) mg_readpixels = (void *)proxy_real_proc("glReadPixels");
    if (!mg_genfb || !mg_bindfb || !mg_genrb || !mg_bindrb || !mg_rbstore ||
        !mg_fbrb || !mg_fbstatus || !mg_clearcolor || !mg_readpixels) return;
    a = (unsigned char *)malloc(px * 4);
    b = (unsigned char *)malloc(px * 4);
    if (!a || !b) { free(a); free(b); return; }
    o_glGetIntegerv(MG_FB_BINDING, &prev);
    if (!g_mg_fbo) {
        mg_genfb(1, &g_mg_fbo); mg_genrb(1, &g_mg_rbc); mg_genrb(1, &g_mg_rbd);
        mg_bindrb(MG_RB, g_mg_rbc); mg_rbstore(MG_RB, MG_RGBA8, MG_AB_W, MG_AB_H);
        mg_bindrb(MG_RB, g_mg_rbd); mg_rbstore(MG_RB, MG_DEPTH24, MG_AB_W, MG_AB_H);
        mg_bindfb(MG_FB, g_mg_fbo);
        mg_fbrb(MG_FB, MG_COLOR0, MG_RB, g_mg_rbc);
        mg_fbrb(MG_FB, MG_DEPTH_ATT, MG_RB, g_mg_rbd);
    } else mg_bindfb(MG_FB, g_mg_fbo);
    if (mg_fbstatus(MG_FB) != MG_FB_COMPLETE) {
        mg_bindfb(MG_FB, (GLuint)prev); free(a); free(b); return;
    }
    o_glPushAttrib(GL_VIEWPORT_BIT | GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT |
                   GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
    o_glViewport(0, 0, MG_AB_W, MG_AB_H);
    o_glDisable(GL_SCISSOR_TEST_);
    o_glMatrixMode(GL_MODELVIEW);
    o_glPushMatrix();
    mg_clearcolor(0.0f, 0.0f, 0.0f, 0.0f);
    o_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* A: exactly the instances in the draw list, the engine's way */
    for (i = 0; i < g->ndraw; i++) {
        float full[16];
        const MgInst *in = &g->uin[g->didx[i]];
        m_mul(full, g_mv[g_view_depth], in->obj);
        o_glLoadMatrixf(full);
        o_glCallList(in->list);
    }
    mg_readpixels(0, 0, MG_AB_W, MG_AB_H, GL_RGBA, GL_UNSIGNED_BYTE, a);
    o_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    o_glLoadMatrixf(g_mv[g_view_depth]);
    mgg_arrays_on(g, &pab);
    mgg_multidraw(GL_TRIANGLES, g->dfirst, g->dcount, (GLsizei)g->ndraw);
    mgg_arrays_off(pab);
    mg_readpixels(0, 0, MG_AB_W, MG_AB_H, GL_RGBA, GL_UNSIGNED_BYTE, b);
    o_glMatrixMode(GL_MODELVIEW);
    o_glPopMatrix();
    if (g_mode != GL_MODELVIEW) o_glMatrixMode(g_mode);
    o_glPopAttrib();
    mg_bindfb(MG_FB, (GLuint)prev);
    {
        /* Split the differences: a pixel covered by only one of the two is
           an EDGE that moved (float rounding in the transform); a pixel both
           cover but in different colours is a COLOUR change (lighting,
           texture or a coplanar surface winning differently). */
        static unsigned worst;
        unsigned edge = 0, colour = 0, maxd = 0;
        for (i = 0; i < px; i++) {
            const unsigned char *pa = a + i * 4, *pb = b + i * 4;
            int ca = pa[0] | pa[1] | pa[2] | pa[3];
            int cb = pb[0] | pb[1] | pb[2] | pb[3];
            int k, d = 0;
            if (ca) { cov++; if (blend && pa[3] < 255) trans++; }
            for (k = 0; k < 4; k++) {
                int e = (int)pa[k] - (int)pb[k];
                if (e < 0) e = -e;
                if (e > d) d = e;
            }
            if (d) {
                differ++;
                if (!ca != !cb) edge++; else colour++;
                if ((unsigned)d > maxd) maxd = (unsigned)d;
            }
        }
        if (differ)
            kv_log("MERGE VERIFY:   group %08X: %u edge pixels moved, %u "
                   "pixels changed colour (largest channel step %u of 255)",
                   g->sig, edge, colour, maxd);
        /* EQUIVALENT: exactly the same coverage, and no pixel more than 8
           steps of 255 away. Measured 24 Sep: every difference was 1-5
           steps, scattered single pixels -- the CPU-side normal transform
           rounding a hair away from the GPU's. Anything that moves an edge
           still fails. */
        /* 24 Sep, union design: one group moved ONE edge pixel of tens of
           thousands covered -- a triangle edge rasterising a hair across a
           pixel centre, the same float rounding showing at an edge. So an
           edge flip is equivalent up to 0.1% of covered pixels; colour is
           judged only where both images cover the pixel. */
        {
            unsigned cmax = 0;
            for (i = 0; i < px; i++) {
                const unsigned char *pa = a + i * 4, *pb = b + i * 4;
                int k;
                if (!(pa[0] | pa[1] | pa[2] | pa[3]) ||
                    !(pb[0] | pb[1] | pb[2] | pb[3])) continue;
                for (k = 0; k < 4; k++) {
                    int e = (int)pa[k] - (int)pb[k];
                    if (e < 0) e = -e;
                    if ((unsigned)e > cmax) cmax = (unsigned)e;
                }
            }
            if (edge * 1000u <= cov && cmax <= 8) g_mgv_equivalent++;
            if (cmax > g_mgv_worst_step) g_mgv_worst_step = cmax;
            maxd = cmax;
        }

        if (differ > worst) {
            worst = differ;
            mg_bmp("keflings_mgv_A.bmp", a, MG_AB_W, MG_AB_H);
            mg_bmp("keflings_mgv_B.bmp", b, MG_AB_W, MG_AB_H);
            for (i = 0; i < px; i++) {
                int k, d = 0;
                for (k = 0; k < 4; k++) {
                    int e = (int)a[i * 4 + k] - (int)b[i * 4 + k];
                    if (e < 0) e = -e;
                    if (e > d) d = e;
                }
                /* differing pixels white, the covered scene faint grey */
                a[i * 4 + 0] = a[i * 4 + 1] = a[i * 4 + 2] =
                    d ? 255 : ((a[i * 4 + 3] | b[i * 4 + 3]) ? 40 : 0);
                a[i * 4 + 3] = 255;
            }
            mg_bmp("keflings_mgv_diff.bmp", a, MG_AB_W, MG_AB_H);
        }
    }
    free(a); free(b);
    g_mgv_tested++;
    if (!differ) g_mgv_identical++;
    if (trans) g_mgv_translucent++;
    if (trans)
        kv_log("MERGE VERIFY: group %08X pass %d (%u instances, %u vertices) "
               "-- %u translucent pixels: the eligibility rule let a "
               "translucent group through. FAILS.", g->sig, g->pass,
               g->ninbuf, g->nv, trans);
}

/* mode 8: what ARE the depth-writes-off groups? Draw the members of the
   largest ones alone into the offscreen target, the engine's way, and save
   the picture. Two-thirds of the world's draws sit in these groups. */
#define MGP_MAX 6
static struct { unsigned sig; int pass; unsigned n; } g_mgp_count[256];
static unsigned g_mgp_ncount, g_mgp_done;
static GLuint   g_mgp_lists[4096]; static float g_mgp_mv[4096][16];
static unsigned g_mgp_nm, g_mgp_target_sig; static int g_mgp_target_pass;
static int      g_mgp_state;   /* 0 counting, 1 collecting, 2 drawing */
static void mgp_portrait(void) {
    unsigned char *a;
    GLint prev = 0;
    unsigned i;
    size_t px = (size_t)MG_AB_W * MG_AB_H;
    char name[64];
    if (!mg_genfb && o_wglGetProcAddress) {
        mg_genfb    = (void *)o_wglGetProcAddress("glGenFramebuffers");
        mg_bindfb   = (void *)o_wglGetProcAddress("glBindFramebuffer");
        mg_genrb    = (void *)o_wglGetProcAddress("glGenRenderbuffers");
        mg_bindrb   = (void *)o_wglGetProcAddress("glBindRenderbuffer");
        mg_rbstore  = (void *)o_wglGetProcAddress("glRenderbufferStorage");
        mg_fbrb     = (void *)o_wglGetProcAddress("glFramebufferRenderbuffer");
        mg_fbstatus = (void *)o_wglGetProcAddress("glCheckFramebufferStatus");
    }
    if (!mg_clearcolor) mg_clearcolor = (void *)proxy_real_proc("glClearColor");
    if (!mg_readpixels) mg_readpixels = (void *)proxy_real_proc("glReadPixels");
    if (!mg_genfb || !mg_readpixels || !mg_clearcolor) return;
    {
        /* The state these draws are made under -- read at the group's own
           first member, where it is the engine's. Colour mask off with depth
           writes off and matched front/back pairs is a stencil shadow pass. */
        GLint cm[4] = { -1, -1, -1, -1 }, sf = 0, sref = 0, sfail = 0, szf = 0,
              szp = 0, cfm = 0, df = 0, swm = 0, tex = 0;
        o_glGetIntegerv(0x0C23, cm);             /* GL_COLOR_WRITEMASK */
        o_glGetIntegerv(0x0B92, &sf);            /* GL_STENCIL_FUNC */
        o_glGetIntegerv(0x0B97, &sref);          /* GL_STENCIL_REF */
        o_glGetIntegerv(0x0B94, &sfail);         /* GL_STENCIL_FAIL */
        o_glGetIntegerv(0x0B95, &szf);           /* GL_STENCIL_PASS_DEPTH_FAIL */
        o_glGetIntegerv(0x0B96, &szp);           /* GL_STENCIL_PASS_DEPTH_PASS */
        o_glGetIntegerv(0x0B98, &swm);           /* GL_STENCIL_WRITEMASK */
        o_glGetIntegerv(0x0B45, &cfm);           /* GL_CULL_FACE_MODE */
        o_glGetIntegerv(0x0B74, &df);            /* GL_DEPTH_FUNC */
        o_glGetIntegerv(0x8069, &tex);
        {
            GLfloat cc[4] = { -1, -1, -1, -1 }, md[4] = { -1, -1, -1, -1 };
            GLfloat aref = -1.0f; GLint afunc = 0, envmode = 0, crgb = 0, calpha = 0;
            GLint src0a = 0, src1a = 0, op0a = 0;
            o_glGetFloatv(0x0B00, cc);                       /* GL_CURRENT_COLOR */
            if (o_glGetMaterialfv) o_glGetMaterialfv(GL_FRONT, GL_DIFFUSE, md);
            o_glGetIntegerv(0x0BC1, &afunc);                 /* GL_ALPHA_TEST_FUNC */
            o_glGetFloatv(0x0BC2, &aref);                    /* GL_ALPHA_TEST_REF */
            {
                void (APIENTRY *gtev)(GLenum, GLenum, GLint *) =
                    (void *)proxy_real_proc("glGetTexEnviv");
                if (gtev) {
                    gtev(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &envmode);
                    gtev(GL_TEXTURE_ENV, 0x8571, &crgb);     /* COMBINE_RGB */
                    gtev(GL_TEXTURE_ENV, 0x8572, &calpha);   /* COMBINE_ALPHA */
                    gtev(GL_TEXTURE_ENV, 0x8588, &src0a);    /* SOURCE0_ALPHA */
                    gtev(GL_TEXTURE_ENV, 0x8589, &src1a);    /* SOURCE1_ALPHA */
                    gtev(GL_TEXTURE_ENV, 0x8598, &op0a);     /* OPERAND0_ALPHA */
                }
            }
            kv_log("DEPTH-OFF ALPHA %u: lighting %d, current colour %.3f %.3f "
                   "%.3f %.3f, material diffuse %.3f %.3f %.3f %.3f, alpha "
                   "test %d func 0x%04X ref %.4f, texenv mode 0x%04X combine "
                   "rgb 0x%04X alpha 0x%04X src0a 0x%04X src1a 0x%04X op0a "
                   "0x%04X, colour material %d",
                   g_mgp_done, o_glIsEnabled(0x0B50) ? 1 : 0,
                   cc[0], cc[1], cc[2], cc[3], md[0], md[1], md[2], md[3],
                   o_glIsEnabled(0x0BC0) ? 1 : 0, afunc, aref, envmode, crgb,
                   calpha, src0a, src1a, op0a, o_glIsEnabled(0x0B57) ? 1 : 0);
        }
        kv_log("DEPTH-OFF STATE %u: colour mask %d%d%d%d | stencil test %d "
               "func 0x%04X ref %d writemask 0x%X ops fail 0x%04X zfail 0x%04X "
               "zpass 0x%04X | cull %d mode 0x%04X | depth func 0x%04X | "
               "blend %d | texture %d",
               g_mgp_done, cm[0], cm[1], cm[2], cm[3],
               o_glIsEnabled(0x0B90) ? 1 : 0, sf, sref, swm, sfail, szf, szp,
               o_glIsEnabled(0x0B44) ? 1 : 0, cfm, df,
               o_glIsEnabled(0x0BE2) ? 1 : 0, tex);
    }
    a = (unsigned char *)malloc(px * 4);
    if (!a) return;
    o_glGetIntegerv(MG_FB_BINDING, &prev);
    if (!g_mg_fbo) {
        mg_genfb(1, &g_mg_fbo); mg_genrb(1, &g_mg_rbc); mg_genrb(1, &g_mg_rbd);
        mg_bindrb(MG_RB, g_mg_rbc); mg_rbstore(MG_RB, MG_RGBA8, MG_AB_W, MG_AB_H);
        mg_bindrb(MG_RB, g_mg_rbd); mg_rbstore(MG_RB, MG_DEPTH24, MG_AB_W, MG_AB_H);
        mg_bindfb(MG_FB, g_mg_fbo);
        mg_fbrb(MG_FB, MG_COLOR0, MG_RB, g_mg_rbc);
        mg_fbrb(MG_FB, MG_DEPTH_ATT, MG_RB, g_mg_rbd);
    } else mg_bindfb(MG_FB, g_mg_fbo);
    o_glPushAttrib(GL_VIEWPORT_BIT | GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT |
                   GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
    o_glViewport(0, 0, MG_AB_W, MG_AB_H);
    o_glDisable(GL_SCISSOR_TEST_);
    o_glMatrixMode(GL_MODELVIEW);
    o_glPushMatrix();
    mg_clearcolor(0.15f, 0.15f, 0.15f, 1.0f);   /* grey, so translucency shows */
    o_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* blending and alpha test OFF: show the geometry itself, whatever its
       alpha -- the question is what these draws ARE */
    o_glDisable(0x0BE2);
    o_glDisable(0x0BC0);
    for (i = 0; i < g_mgp_nm; i++) {
        o_glLoadMatrixf(g_mgp_mv[i]);
        o_glCallList(g_mgp_lists[i]);
    }
    mg_readpixels(0, 0, MG_AB_W, MG_AB_H, GL_RGBA, GL_UNSIGNED_BYTE, a);
    o_glMatrixMode(GL_MODELVIEW);
    o_glPopMatrix();
    if (g_mode != GL_MODELVIEW) o_glMatrixMode(g_mode);
    o_glPopAttrib();
    mg_bindfb(MG_FB, (GLuint)prev);
    sprintf(name, "keflings_depthoff_%u.bmp", g_mgp_done);
    mg_bmp(name, a, MG_AB_W, MG_AB_H);
    free(a);
    kv_log("DEPTH-OFF PORTRAIT %u: group %08X pass %d, %u members drawn alone "
           "-> %%TEMP%%\%s", g_mgp_done, g_mgp_target_sig, g_mgp_target_pass,
           g_mgp_nm, name);
}

static unsigned g_mgp_top_sig[MGP_MAX]; static int g_mgp_top_pass[MGP_MAX];
static unsigned g_mgp_top_n[MGP_MAX], g_mgp_ntop, g_mgp_collect_frame;
static int      g_mgp_drawn_this_frame;
static void mgp_pick(void) {
    if (g_mgp_done >= g_mgp_ntop) { g_mgp_state = 3; return; }
    g_mgp_target_sig = g_mgp_top_sig[g_mgp_done];
    g_mgp_target_pass = g_mgp_top_pass[g_mgp_done];
    g_mgp_nm = 0; g_mgp_state = 1; g_mgp_collect_frame = g_frames + 1;
}
static void mgp_note(const MgGroup *g, GLuint list) {
    DWORD dt;
    unsigned i;
    /* every group, not only depth-off: the largest ones include visible
       terrain, which is the positive control for the portrait itself */
    if (!g_mgg_world_t0 || g_mgp_state == 3) return;
    dt = GetTickCount() - g_mgg_world_t0;
    if (g_mgp_state == 0) {
        if (dt > 15000 && dt < 20000) {
            for (i = 0; i < g_mgp_ncount; i++)
                if (g_mgp_count[i].sig == g->sig && g_mgp_count[i].pass == g->pass) {
                    g_mgp_count[i].n++; return;
                }
            if (g_mgp_ncount < 256) {
                g_mgp_count[g_mgp_ncount].sig = g->sig;
                g_mgp_count[g_mgp_ncount].pass = g->pass;
                g_mgp_count[g_mgp_ncount].n = 1; g_mgp_ncount++;
            }
        } else if (dt >= 20000) {
            unsigned k;
            for (k = 0; k < MGP_MAX; k++) {
                unsigned best = 0, bi = 256;
                for (i = 0; i < g_mgp_ncount; i++)
                    if (g_mgp_count[i].n > best) { best = g_mgp_count[i].n; bi = i; }
                if (bi == 256) break;
                g_mgp_top_sig[k] = g_mgp_count[bi].sig;
                g_mgp_top_pass[k] = g_mgp_count[bi].pass;
                g_mgp_top_n[k] = best;
                {
                    const MgGroup *pg = mgg_find(g_mgp_count[bi].sig, g_mgp_count[bi].pass);
                    kv_log("DEPTH-OFF: portrait %u = group %08X pass %d, %.1f "
                           "draws a frame, verdict: %s", k, g_mgp_count[bi].sig,
                           g_mgp_count[bi].pass, best / 300.0,
                           pg ? k_mge[pg->elig] : "?");
                }
                g_mgp_count[bi].n = 0;
                g_mgp_ntop = k + 1;
            }
            mgp_pick();
        }
        return;
    }
    if (g->sig != g_mgp_target_sig || g->pass != g_mgp_target_pass) return;
    if (g_mgp_state == 1 && g_frames == g_mgp_collect_frame && g_mgp_nm < 4096) {
        g_mgp_lists[g_mgp_nm] = list;
        memcpy(g_mgp_mv[g_mgp_nm], g_mv[g_mv_sp], 64);
        g_mgp_nm++;
    } else if (g_mgp_state == 2 && !g_mgp_drawn_this_frame) {
        g_mgp_drawn_this_frame = 1;
        mgp_portrait();          /* the group's own state is in force now */
        g_mgp_done++;
        mgp_pick();
    }
}

/* ---- skip_noop: draws that provably change nothing ------------------------ */
static void   (APIENTRY *nq_gen)(GLsizei, GLuint *);
static void   (APIENTRY *nq_begin)(GLenum, GLuint);
static void   (APIENTRY *nq_end)(GLenum);
static void   (APIENTRY *nq_get)(GLuint, GLenum, GLuint *);
#define NQ_N 16
static GLuint   g_nq[NQ_N]; static unsigned g_nq_pending[NQ_N]; /* 1 = awaiting */
static unsigned g_nq_next;
static int nq_ready(void) {
    static int tried, ok;
    if (tried) return ok;
    tried = 1;
    if (!o_wglGetProcAddress) return 0;
    nq_gen   = (void *)o_wglGetProcAddress("glGenQueries");
    nq_begin = (void *)o_wglGetProcAddress("glBeginQuery");
    nq_end   = (void *)o_wglGetProcAddress("glEndQuery");
    nq_get   = (void *)o_wglGetProcAddress("glGetQueryObjectuiv");
    ok = nq_gen && nq_begin && nq_end && nq_get;
    if (ok) nq_gen(NQ_N, g_nq);
    else kv_log("SKIP NO-OP: occlusion queries unavailable -- skipping stays off.");
    return ok;
}
/* Is this draw provably a no-op? Only state tracked EXACTLY; unknown = no. */
/* Why a draw is not a candidate -- counted so coverage can be improved
   without loosening the rule blindly. */
enum { NR_OK = 0, NR_NOMESH, NR_LIGHT, NR_ATEST, NR_CMAT, NR_AFUNC, NR_ENV,
       NR_MAT, NR_CC, NR_HASC, NR_ALPHA, NR_N };
static const char *k_nr[NR_N] = { "CANDIDATE", "list not captured whole",
    "lighting unknown or off", "alpha test unknown or off",
    "colour material unknown", "alpha func not GREATER>=0 / unknown",
    "texenv unknown or not MODULATE", "material diffuse unknown",
    "current colour unknown", "mesh has a colour array", "alpha above zero" };
static unsigned g_nr[NR_N];
static int nop_reason(GLuint list) {
    GLfloat a;
    int u;
    Mesh *me0 = mc_find(list, 0);
    if (!me0 || !me0->ok || !me0->nvert) return NR_NOMESH;
    if (g_nx.light != 1) return NR_LIGHT;
    if (g_nx.atest != 1) return NR_ATEST;
    if (g_nx.cmat < 0) return NR_CMAT;
    if (!g_nx.af_ok || g_nx.af_func != GL_GREATER || g_nx.af_ref < 0.0f)
        return NR_AFUNC;
    /* every ENABLED unit must MODULATE: a REPLACE could bring alpha back */
    for (u = 0; u < NX_UNITS; u++) {
        if (g_nx.tex[u] < 0) return NR_ENV;
        if (g_nx.tex[u] == 1 && g_nx.env[u] != 1) return NR_ENV;
    }
    if (g_nx.cmat == 0) {
        if (!g_nx.dif_ok) return NR_MAT;
        a = g_nx.dif_a;
    } else {
        if (!g_cc_valid) return NR_CC;
        if (me0->has_c) return NR_HASC;           /* a colour array supplies alpha */
        a = g_cc[3];
    }
    return a == 0.0f ? NR_OK : NR_ALPHA;
}
static int nop_candidate(GLuint list) {
    int r = nop_reason(list);
    g_nr[r]++;
    return r == NR_OK;
}
/* The probe: the draw inside a query with the depth test OFF, so nothing
   behind other geometry can hide its samples, and colour and depth writes
   OFF, so it cannot change a pixel. The alpha test is left exactly as the
   engine set it -- that is what is being proved. */
static void (APIENTRY *nq_colormask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void nq_probe(GLuint list, unsigned q) {
    if (!nq_colormask) nq_colormask = (void *)proxy_real_proc("glColorMask");
    o_glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
    if (nq_colormask) nq_colormask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    o_glDepthMask(GL_FALSE);
    o_glDisable(GL_DEPTH_TEST);
    nq_begin(0x8914 /* GL_SAMPLES_PASSED */, g_nq[q]);
    o_glCallList(list);
    nq_end(0x8914);
    o_glPopAttrib();
    g_nq_pending[q] = 1;
}
/* Collect finished queries. Any sample means the rule is wrong: off for good. */
static void nq_collect(int wait) {
    unsigned i;
    for (i = 0; i < NQ_N; i++) {
        GLuint avail = 0, samples = 0;
        if (!g_nq_pending[i]) continue;
        if (!wait) { nq_get(g_nq[i], 0x8867 /* RESULT_AVAILABLE */, &avail); if (!avail) continue; }
        nq_get(g_nq[i], 0x8866 /* QUERY_RESULT */, &samples);
        g_nq_pending[i] = 0;
        g_nop_checked++;
        if (samples) {
            g_nop_trust = -1;
            kv_log("SKIP NO-OP: a draw the rule called a no-op wrote %u samples "
                   "-- skipping is OFF for this session.", samples);
        } else if (g_nop_trust == 0 && g_nop_checked >= 200) {
            g_nop_trust = 1;
            kv_log("SKIP NO-OP: 200 candidate draws measured by the GPU, every "
                   "one wrote 0 samples -- skipping them from now on.");
        }
    }
}
/* Called per world list draw. 1 = skip it. */
static int nop_member(GLuint list) {
    unsigned q;
    if (g_nop_trust < 0 || !nq_ready()) return 0;
    if (!nop_candidate(list)) return 0;
    g_nop_cand++;
    if (g_nop_trust == 1) {
        DWORD now = GetTickCount();
        if (now - g_nop_lastcheck >= 5000u) {  /* the periodic re-check */
            g_nop_lastcheck = now;
            q = g_nq_next; g_nq_next = (g_nq_next + 1) % NQ_N;
            if (g_nq_pending[q]) nq_collect(1);
            nq_probe(list, q);
            return 0;                       /* and it draws normally too */
        }
        if (!g_nop_on) return 0;
        g_nop_skip++;
        return 1;
    }
    /* verifying: draw it inside a query, normally, for both eyes */
    q = g_nq_next; g_nq_next = (g_nq_next + 1) % NQ_N;
    if (g_nq_pending[q]) nq_collect(1);
    nq_probe(list, q);
    return 0;
}
static LARGE_INTEGER g_nop_last;
static void nop_frame_end(void) {
    LARGE_INTEGER now;
    double ms = 0.0;
    DWORD period = vr_stereo_active() ? 5000u : 1000u;
    int next;
    if (!g_vrcfg.skip_noop) return;
    if (nq_ready()) nq_collect(0);
    QueryPerformanceCounter(&now);
    if (g_nop_last.QuadPart && g_qpf.QuadPart)
        ms = 1000.0 * (double)(now.QuadPart - g_nop_last.QuadPart) / (double)g_qpf.QuadPart;
    g_nop_last = now;
    if (g_nop_frame_world >= 50 && !g_nop_t0) g_nop_t0 = GetTickCount();
    if (g_vrcfg.skip_noop == 2 && g_nop_trust == 1 && g_nop_since_switch >= 5 &&
        g_nop_frame_world >= 50 && ms > 0.0 && ms < 200.0 && g_nop_t0 &&
        GetTickCount() - g_nop_t0 > 10000u) {
        g_nop_arm_ms[g_nop_on] += ms; g_nop_arm_n[g_nop_on]++;
    }
    g_nop_frames++;
    g_nop_since_switch++;
    g_nop_frame_world = 0;
    /* 3 = follow the merge's arm: skipping on whenever merging is on, so
       mode 7's three states become off / safe+skip / full+skip. */
    next = g_vrcfg.skip_noop == 1 ? 1 :
           g_vrcfg.skip_noop == 2 ? (((GetTickCount() / period) & 1u) == 0) :
           g_vrcfg.skip_noop == 3 ? (g_mgg_on ? 1 : 0) : 0;
    if (next != g_nop_on) g_nop_since_switch = 0;
    g_nop_on = next;
}
static void nop_report(void) {
    double f = g_nop_frames ? (double)g_nop_frames : 1.0;
    kv_log("      SKIP NO-OP: %s, %u GPU checks. %.0f candidate draws a frame, "
           "%.0f skipped a frame (both eyes each).",
           g_nop_trust > 0 ? "TRUSTED" : g_nop_trust < 0 ? "OFF (a check failed)" : "verifying",
           g_nop_checked, g_nop_cand / f, g_nop_skip / f);
    if (g_vrcfg.skip_noop == 2 && g_nop_arm_n[0] && g_nop_arm_n[1])
        kv_log("      SKIP NO-OP A/B, world frames only: skipping %.3f ms (%.1f fps, "
               "%u) | drawing them %.3f ms (%.1f fps, %u) -- saves %.3f ms",
               g_nop_arm_ms[1] / g_nop_arm_n[1], 1000.0 * g_nop_arm_n[1] / g_nop_arm_ms[1],
               g_nop_arm_n[1], g_nop_arm_ms[0] / g_nop_arm_n[0],
               1000.0 * g_nop_arm_n[0] / g_nop_arm_ms[0], g_nop_arm_n[0],
               g_nop_arm_ms[0] / g_nop_arm_n[0] - g_nop_arm_ms[1] / g_nop_arm_n[1]);
    {
        int i;
        for (i = 0; i < NR_N; i++)
            if (g_nr[i])
                kv_log("      SKIP NO-OP:   %7.1f a frame  %s", g_nr[i] / f, k_nr[i]);
        memset(g_nr, 0, sizeof(g_nr));
    }
    g_nop_cand = 0; g_nop_skip = 0; g_nop_frames = 0;
}

static int mgg_member(GLuint list) {
    MgGroup *g;
    Mesh *me;
    MgInst in;
    unsigned h = 2166136261u;
    const unsigned char *p;
    int i;
    if (!mgg_ready()) return 0;
    if (g_view_depth < 0 || !g_ob_view[g_mv_sp]) return 0;
    me = mc_find(list, 0);
    if (!me || !me->ok || !me->mergeable) return 0;
    /* the census reads the texture binding back per draw: diagnostics only */
    if (g_vrcfg.diagnostics) mg_blend_note();
    g_mggw_draws++;
    g_mgg_frame_draws++;
    g = mgg_find(g_gk_sig, g_pass_idx);
    if (!g) return 0;
    if (g->elig == MGE_UNDECIDED) { g->elig = mgg_decide(); g_mgg_why[g->elig]++; }
    if (g_vrcfg.merge_group == 8) { mgp_note(g, list); return 0; }

    in.list = list; in.gen = me->gen;
    memcpy(in.obj, g_ob[g_mv_sp], 64);
    p = (const unsigned char *)in.obj;
    for (i = 0; i < 64; i++) h = (h ^ p[i]) * 16777619u;
    h = (h ^ (unsigned)list) * 16777619u;
    h = (h ^ in.gen) * 16777619u;
    in.key = h ? h : 1u;

    if (!MGG_ELIG(g)) {
        if (g_vrcfg.diagnostics) {
            /* which layers are drawn over merged objects, and with which
               depth test: an EQUAL layer fails against any offset */
            MgOv *e = mgg_ov_slot(in.key, 0);
            static unsigned said;
            if (e && e->base_fid == g_mgg_fid && said < 40) {
                GLint df = 0, dm = 0;
                said++;
                o_glGetIntegerv(0x0B74, &df);          /* GL_DEPTH_FUNC */
                o_glGetIntegerv(0x0B72, &dm);          /* GL_DEPTH_WRITEMASK */
                kv_log("LAYER: list %u drawn again over its merged self, group %08X "
                       "(verdict %d): depth func 0x%04X, depth writes %d, blend %d "
                       "src 0x%04X dst 0x%04X", list, g->sig, g->elig, df, dm,
                       o_glIsEnabled(0x0BE2) ? 1 : 0, g_gk_val[GKS_BSRC], g_gk_val[GKS_BDST]);
            }
        }
        mgg_ov_again(in.key);          /* a layer over a merged object? */
        return 0;
    }

    /* self-audit: the group's first two frames */
    if (g->aud_frames < 2) {
        float v[22];
        memset(v, 0, sizeof(v));
        mgg_audit_read(v);
        if (!g->aud_have) { memcpy(g->aud_ref, v, sizeof(v)); g->aud_have = 1; }
        else if (memcmp(v, g->aud_ref, sizeof(v)) != 0) {
            g_mgg_why[g->elig]--; g->elig = MGE_NO_AUDIT; g_mgg_why[MGE_NO_AUDIT]++;
            return 0;
        }
    }

    mgg_ov_base(in.key);
    if (g_vrcfg.diagnostics) mgg_state_probe(g);
    {   /* the transparency this member is drawn with, from the exact
           trackers; -1 when unknown, and then nothing changes */
        float a = -1.0f;
        if (me->has_c) a = 1.0f;                        /* per vertex, in the buffer */
        else if (g_nx.light == 1 && g_nx.cmat == 0) a = g_nx.dif_ok ? g_nx.dif_a : -1.0f;
        else if (g_cc_valid) a = g_cc[3];
        if (a >= 0.0f && a < 0.9999f) {
            mgg_fade_mark(in.key);
            if (a < g_mggw_fade_min) { g_mggw_fade_min = a; g_mggw_fade_grp = g->sig; }
        }
        if (mgg_fade_active(in.key)) { g_mggw_fading++; return 0; }
    }
    if (mgg_ov_active(in.key)) {       /* layered: the engine draws it */
        g_mggw_overlaid++;
        return 0;
    }
    if (g->ncur >= g->capcur) {
        unsigned want = g->capcur ? g->capcur * 2 : 64;
        MgInst *nc;
        if (want > MGG_MAXM) {
            g_mgg_why[g->elig]--; g->elig = MGE_NO_SIZE; g_mgg_why[MGE_NO_SIZE]++;
            return 0;
        }
        nc = (MgInst *)realloc(g->cur, want * sizeof(MgInst));
        if (!nc) return 0;
        g->cur = nc; g->capcur = want;
    }
    g->cur[g->ncur++] = in;
    g_mggw_elig_draws++;

    if (g_vrcfg.merge_group == 4) {
        if (!g->ab_done && g->valid && g->ndraw >= 2 && g->aud_frames >= 2 &&
            g->ncur == 1)
            mgg_verify(g);
        return 0;
    }
    if (!g_mgg_on || !g->valid || g->aud_frames < 2) return 0;
    if (g->elig == MGE_ORDER && g_mgg_arm < 2) return 0;   /* safe arm only */
    if (mgg_is_mover(g, list)) return 0;
    {
        /* in the draw list = present last frame (stamped with its id) */
        int idx = mgg_ulookup(g, in.key);
        if (idx < 0 || g->ustamp[idx] != g->dfid) { g_mggw_notbuf++; return 0; }
    }
    g_mggw_skipped++;
    if (!g->drawn) { mgg_draw(g); g->drawn = 1; }
    return 1;
}

/* SOAK CHECK (mode 7, after the ten file dumps): every 30 s the same
   three-frame comparison as the dumps -- last merged frame, first normal
   frame, the next normal frame as the control -- done in memory and
   LOGGED, so a run through all four seasons checks itself without
   hundreds of 11 MB files. Frames where merged differs from normal by more
   than three times the control (and over 1%) are written out for a look. */
static unsigned char *g_sk_buf[3];
static int      g_sk_w, g_sk_h;
static unsigned g_sk_n, g_sk_flagged;
static int mgg_grab(int slot) {
    GLint vp[4];
    if (!mg_readpixels) mg_readpixels = (void *)proxy_real_proc("glReadPixels");
    if (!mg_readpixels) return 0;
    o_glGetIntegerv(GL_VIEWPORT, vp);
    if (g_eng_vp[2] > 0 && g_eng_vp[3] > 0) {
        vp[0] = 0; vp[1] = 0; vp[2] = g_eng_vp[2]; vp[3] = g_eng_vp[3];
    }
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] > 8192 || vp[3] > 8192) return 0;
    if (vp[2] != g_sk_w || vp[3] != g_sk_h) {
        int k;
        for (k = 0; k < 3; k++) { free(g_sk_buf[k]); g_sk_buf[k] = NULL; }
        g_sk_w = vp[2]; g_sk_h = vp[3];
    }
    if (!g_sk_buf[slot]) g_sk_buf[slot] = (unsigned char *)malloc((size_t)g_sk_w * g_sk_h * 4);
    if (!g_sk_buf[slot]) return 0;
    o_glReadBuffer(GL_BACK);
    mg_readpixels(0, 0, g_sk_w, g_sk_h, GL_RGBA, GL_UNSIGNED_BYTE, g_sk_buf[slot]);
    return 1;
}
static double mgg_sk_diff(const unsigned char *a, const unsigned char *b) {
    size_t i, n = (size_t)g_sk_w * g_sk_h, d = 0;
    for (i = 0; i < n; i++) {
        const unsigned char *p = a + i * 4, *q = b + i * 4;
        int s = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
        if (s > 24) d++;
    }
    return n ? 100.0 * (double)d / (double)n : 0.0;
}
static void mgg_sk_judge(void) {
    double on, ctl;
    if (!g_sk_buf[0] || !g_sk_buf[1] || !g_sk_buf[2]) return;
    on  = mgg_sk_diff(g_sk_buf[0], g_sk_buf[1]);
    ctl = mgg_sk_diff(g_sk_buf[1], g_sk_buf[2]);
    g_sk_n++;
    {   /* every 2 minutes a quarter-size picture of the normal frame, to
           see which season each check fell in (a GDI screen grab of this
           window shows a stale splash; the back buffer is the truth) */
        static DWORD lastshot;
        if (!lastshot || GetTickCount() - lastshot >= 120000u) {
            int w4 = g_sk_w / 4, h4 = g_sk_h / 4, x, y;
            unsigned char *q = (unsigned char *)malloc((size_t)w4 * h4 * 4);
            if (q) {
                char nm[64];
                for (y = 0; y < h4; y++)
                    for (x = 0; x < w4; x++)
                        memcpy(q + ((size_t)y * w4 + x) * 4,
                               g_sk_buf[1] + ((size_t)(y * 4) * g_sk_w + x * 4) * 4, 4);
                sprintf(nm, "keflings_season_%03u.bmp", g_sk_n);
                mg_bmp(nm, q, w4, h4);
                free(q);
            }
            lastshot = GetTickCount();
        }
    }
    {
        int bad = on > 1.0 && on > 3.0 * ctl;
        kv_log("SOAK CHECK %u (%u s into the world): merged vs normal %.3f%%, control "
               "(two normal frames) %.3f%% -- %s", g_sk_n,
               (GetTickCount() - g_mgg_dense_t0) / 1000u, on, ctl,
               bad ? "DIFFERENT, frames saved" : "same");
        if (bad && g_sk_flagged < 10) {
            char nm[64];
            g_sk_flagged++;
            sprintf(nm, "keflings_flag_%u_ON.bmp", g_sk_n);   mg_bmp(nm, g_sk_buf[0], g_sk_w, g_sk_h);
            sprintf(nm, "keflings_flag_%u_OFF.bmp", g_sk_n);  mg_bmp(nm, g_sk_buf[1], g_sk_w, g_sk_h);
            sprintf(nm, "keflings_flag_%u_OFF2.bmp", g_sk_n); mg_bmp(nm, g_sk_buf[2], g_sk_w, g_sk_h);
        }
    }
}

/* The back buffer, read just before it is presented. */
static void mgg_dump_screen(const char *name) {
    GLint vp[4];
    unsigned char *px;
    if (!mg_readpixels) mg_readpixels = (void *)proxy_real_proc("glReadPixels");
    if (!mg_readpixels) return;
    o_glGetIntegerv(GL_VIEWPORT, vp);
    if (g_eng_vp[2] > 0 && g_eng_vp[3] > 0) {
        vp[0] = 0; vp[1] = 0; vp[2] = g_eng_vp[2]; vp[3] = g_eng_vp[3];
    }
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] > 8192 || vp[3] > 8192) return;
    px = (unsigned char *)malloc((size_t)vp[2] * vp[3] * 4);
    if (!px) return;
    o_glReadBuffer(GL_BACK);
    mg_readpixels(0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px);
    mg_bmp(name, px, vp[2], vp[3]);
    free(px);
}

static void mgg_frame_end(double frame_ms) {
    unsigned i, k;
    int arm;
    g_mgp_drawn_this_frame = 0;
    if (g_mgp_state == 1 && g_frames > g_mgp_collect_frame) g_mgp_state = 2;
    g_mggw_frames++;
    arm = (g_vrcfg.merge_group == 7 || g_vrcfg.merge_group == 9) ?
          g_mgg_arm : (g_mgg_on ? 1 : 0);
    if (g_mgg_frame_draws >= 50 && !g_mgg_world_t0) g_mgg_world_t0 = GetTickCount();
    if (g_mgg_frame_draws >= 1500 && !g_mgg_dense_t0) g_mgg_dense_t0 = GetTickCount();
    /* timed only in the dense world, 10 s after it appears */
    if (g_mgg_frame_draws >= 1500 && g_mgg_dense_t0 &&
        GetTickCount() - g_mgg_dense_t0 > 10000u &&
        g_mgg_since_switch >= 5 && frame_ms > 0.0 && frame_ms < 200.0) {
        g_mgg_arm_ms[arm] += frame_ms;  g_mgg_arm_n[arm]++;
        g_mggw_arm_ms[arm] += frame_ms; g_mggw_arm_n[arm]++;
    }
    g_mgg_frame_draws = 0;
    g_mgg_since_switch++;
    for (i = 0; i < MGG_N; i++) {
        MgGroup *g = &g_mgg[i];
        int same;
        if (!g->used || !MGG_ELIG(g)) { if (g->used) g->ncur = 0; continue; }
        if (g->ncur && g->aud_frames < 2) { g->aud_frames++; g->aud_have = 0; }
        (void)same;
        {
            unsigned nv0 = g->cpu_nv, full = 0;
            GLuint newlists[64]; int newidx[64]; unsigned nnew = 0;
            /* 1. every member present: find it, or append it */
            for (k = 0; k < g->ncur; k++) {
                int idx;
                if (mgg_is_mover(g, g->cur[k].list)) continue;
                idx = mgg_ulookup(g, g->cur[k].key);
                if (idx < 0) {
                    idx = mgg_uappend(g, &g->cur[k]);
                    if (idx == -2) { full = 1; break; }
                    if (idx >= 0 && nnew < 64) {
                        newlists[nnew] = g->cur[k].list; newidx[nnew] = idx; nnew++;
                    }
                }
                if (idx >= 0) g->ustamp[idx] = g_mgg_fid;
            }
            if (full) {
                g_mgg_why[g->elig]--; g->elig = MGE_NO_SIZE;
                g_mgg_why[MGE_NO_SIZE]++; g->valid = 0;
                g->ncur = 0; g->drawn = 0;
                continue;
            }
            mgg_upload_from(g, nv0);
            /* 2. movers: drawn from the buffer this frame but NOT here, and
               the same list DID appear at a new matrix CLOSE BY -- it moved.
               24 Sep: the first version asked only "same list", and in the
               headset every head turn swapped one copy of a tree out of view
               and another into it, which read as a tree jumping -- 69 lists
               wrongly thrown out of their buffers in one session. A real
               mover reappears within a step of where it was; nothing in this
               game moves 16 units in one frame. A plain disappearance is
               culling: harmless for one frame. */
            for (k = 0; k < g->ndraw; k++) {
                unsigned di = g->didx[k], j;
                const float *op;
                if (g->ustamp[di] == g_mgg_fid) continue;
                g_mggw_ghosts++;
                op = g->uin[di].obj;
                for (j = 0; j < nnew; j++) {
                    const float *np;
                    float dx, dy, dz;
                    if (newlists[j] != g->uin[di].list) continue;
                    np = g->uin[newidx[j]].obj;
                    dx = np[12] - op[12]; dy = np[13] - op[13]; dz = np[14] - op[14];
                    if (dx * dx + dy * dy + dz * dz > 16.0f * 16.0f) continue;
                    if (g->nmovers < 32 && !mgg_is_mover(g, newlists[j])) {
                        g->movers[g->nmovers++] = newlists[j];
                        g_mggw_movers++;
                    }
                    break;
                }
            }
            /* 3. next frame's draw list: this frame's members, in the
               engine's own order, as ranges of the buffer */
            if (g->ncur > g->capdraw) {
                unsigned want = g->ncur * 2;
                unsigned *a = (unsigned *)realloc(g->didx, want * sizeof(unsigned));
                GLint    *b;
                GLsizei  *c;
                if (a) g->didx = a;
                b = (GLint *)realloc(g->dfirst, want * sizeof(GLint));
                if (b) g->dfirst = b;
                c = (GLsizei *)realloc(g->dcount, want * sizeof(GLsizei));
                if (c) g->dcount = c;
                if (a && b && c) g->capdraw = want;
            }
            g->ndraw = 0;
            for (k = 0; k < g->ncur && g->ndraw < g->capdraw; k++) {
                int idx;
                if (mgg_is_mover(g, g->cur[k].list)) continue;
                /* a layer was found over it this frame: from the next frame
                   the engine draws it, so the merge must not as well */
                if (mgg_ov_active(g->cur[k].key)) continue;
                idx = mgg_ulookup(g, g->cur[k].key);
                if (idx < 0 || g->ustamp[idx] != g_mgg_fid) continue;
                g->didx[g->ndraw]   = (unsigned)idx;
                g->dfirst[g->ndraw] = (GLint)g->ufirst[idx];
                g->dcount[g->ndraw] = (GLsizei)g->ucount[idx];
                g->ndraw++;
            }
            g->dfid = g_mgg_fid;
            g->valid = g->ndraw >= 2;
            g->nv = g->cpu_nv; g->ninbuf = g->nuni;
        }
        g->ncur = 0; g->drawn = 0;
    }
    g_mgg_fid++;
    /* The real screen, both sides of one ON -> OFF switch: two consecutive
       frames, the camera barely moved, so anything the merged draw failed
       to put on screen is plain to see. The offscreen verify only proves
       the buffer; this proves the draw that reaches the screen. */
    {
        static int dumped, pending;
        DWORD period = vr_stereo_active() ? 5000u : 1000u;
        /* 9 is mode 7's FULL arm all the time */
        int next_arm = g_vrcfg.merge_group == 7 ?
                       (int)((GetTickCount() / period) % 3u) :
                       g_vrcfg.merge_group == 9 ? 2 : 0;
        int next = g_vrcfg.merge_group == 7 ? (next_arm > 0) :
                   (g_vrcfg.merge_group == 2 || g_vrcfg.merge_group == 9) ? 1 :
                   (g_vrcfg.merge_group == 3 || g_vrcfg.merge_group == 5 ||
                    g_vrcfg.merge_group == 6) ?
                       (((GetTickCount() / period) & 1u) == 0) : 0;
        if ((g_vrcfg.merge_group == 3 || g_vrcfg.merge_group == 5 ||
             g_vrcfg.merge_group == 6 || g_vrcfg.merge_group == 7) &&
            dumped < 10 && g_mggw_draws && g_mgg_dense_t0 &&
            GetTickCount() - g_mgg_dense_t0 > 20000u + 4000u * (DWORD)dumped) {
            /* Up to ten triples, 4 s apart, so a harness that walks the
               camera between them compares the two paths at ten viewpoints
               (the snow flicker of 24 Sep changed with every head move).
               The first triple keeps the old names. */
            char nm[64];
            const char *sfx = dumped ? "_" : "";
            char idx[8];
            if (dumped) sprintf(idx, "%d", dumped); else idx[0] = 0;
            if (pending == 0 && g_mgg_on && !next) {
                sprintf(nm, "keflings_screen_ON%s%s.bmp", sfx, idx);
                mgg_dump_screen(nm);                         /* last ON frame */
                pending = 1;
            } else if (pending == 1) {
                sprintf(nm, "keflings_screen_OFF%s%s.bmp", sfx, idx);
                mgg_dump_screen(nm);                         /* first OFF frame */
                pending = 2;
            } else if (pending == 2) {
                /* the CONTROL: the next frame, also unmerged. OFF vs OFF2 is
                   how much the drifting camera alone changes a frame; ON vs
                   OFF has to be no bigger than that. */
                sprintf(nm, "keflings_screen_OFF2%s%s.bmp", sfx, idx);
                mgg_dump_screen(nm);
                pending = 0;
                dumped++;
                kv_log("MERGE: screen dump %d across an ON -> OFF switch, plus "
                       "the next unmerged frame as a control.", dumped);
            }
        } else if (g_vrcfg.merge_group == 7 && dumped >= 10 && g_mgg_dense_t0) {
            static DWORD last;
            static int sk;
            if (sk == 0 && g_mgg_on && !next && GetTickCount() - last > 30000u) {
                if (mgg_grab(0)) sk = 1;
            } else if (sk == 1) {
                sk = mgg_grab(1) ? 2 : 0;
            } else if (sk == 2) {
                if (mgg_grab(2)) mgg_sk_judge();
                sk = 0; last = GetTickCount();
            }
        }
        if (next != g_mgg_on || next_arm != g_mgg_arm) g_mgg_since_switch = 0;
        g_mgg_on = next;
        g_mgg_arm = next_arm;
    }
}

static void mgg_report(void) {
    double f = g_mggw_frames ? (double)g_mggw_frames : 1.0;
    /* Every allocation the merge makes, itemised: a 32-bit process that is
       not large-address-aware has 2 GB for the engine, the driver, VR and
       us, and merge on measured +200 MB of address space. */
    {
        double mesh = 0, cpu = 0, uni = 0, hash = 0, cur = 0, drw = 0, vbo = 0;
        unsigned i, groups = 0;
        for (i = 0; i < MC_N; i++)
            if (g_mc[i].id)
                mesh += (double)g_mc[i].cap * sizeof(BaVert) +
                        (double)g_mc[i].capb * (sizeof(GLenum) + 2 * sizeof(unsigned)) +
                        (g_mc[i].vbo ? (double)g_mc[i].nvert * sizeof(BaVert) : 0.0);
        for (i = 0; i < MGG_N; i++) {
            const MgGroup *g = &g_mgg[i];
            if (!g->used) continue;
            groups++;
            cpu  += (double)g->cpu_cap * sizeof(BaVert);
            uni  += (double)g->capuni * (sizeof(MgInst) + 3 * sizeof(unsigned));
            hash += g->uhash ? (double)MGG_SET * sizeof(unsigned) : 0.0;
            hash += g->set ? (double)MGG_SET * sizeof(unsigned) : 0.0;
            cur  += (double)g->capcur * sizeof(MgInst) +
                    (double)g->capinbuf * sizeof(MgInst);
            drw  += (double)g->capdraw * (sizeof(unsigned) + sizeof(GLint) + sizeof(GLsizei));
            vbo  += (double)g->vbo_cap * sizeof(BaVert);
        }
        kv_log("      MERGE MEMORY: %u groups. Mesh copies %.1f MB, group CPU "
               "copies %.1f MB, union tables %.1f MB, hash tables %.1f MB, "
               "member lists %.1f MB, draw lists %.2f MB -- %.1f MB of heap. "
               "Buffer objects allocated %.1f MB (the driver may shadow these "
               "in system memory too).",
               groups, mesh / 1048576.0, cpu / 1048576.0, uni / 1048576.0,
               hash / 1048576.0, cur / 1048576.0, drw / 1048576.0,
               (mesh + cpu + uni + hash + cur + drw) / 1048576.0,
               vbo / 1048576.0);
        {   /* the whole process: a 32-bit game without large-address-aware
               has 2 GB of address space, and a slow climb over a long
               session is what ends it */
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms))
                kv_log("      PROCESS MEMORY: %.0f MB of %.0f MB address space in use.",
                       (double)(ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0,
                       (double)ms.ullTotalVirtual / 1048576.0);
        }
    }
    unsigned i, groups_valid = 0, verts = 0;
    for (i = 0; i < MGG_N; i++)
        if (g_mgg[i].used && g_mgg[i].valid) { groups_valid++; verts += g_mgg[i].nv; }
    kv_log("      MERGE: %.0f mergeable world draws a frame, %.0f of them in "
           "eligible groups; %.0f skipped into %.1f merged draws a frame "
           "(%.0f not yet in a buffer). %u groups have buffers, %u vertices, "
           "%.1f MB on the GPU. %u rebuilds.",
           g_mggw_draws / f, g_mggw_elig_draws / f, g_mggw_skipped / f,
           g_mggw_merged / f, g_mggw_notbuf / f, groups_valid, verts,
           verts * (double)sizeof(BaVert) / 1048576.0, g_mggw_rebuilds);
    kv_log("      MERGE: this window %u instances appended, %.1f buffered "
           "instances a frame absent (drawn one frame late -- culling), %u "
           "lists caught moving and taken out of their buffers.",
           g_mggw_appends, g_mggw_ghosts / f, g_mggw_movers);
    kv_log("      MERGE: LAYERED objects (snow, a highlight -- drawn again on top "
           "of themselves) kept out of the merge: %.1f a frame, %u newly found "
           "this window. Non-zero in the snow is expected; they merge again "
           "when the layer goes invisible.",
           g_mggw_overlaid / f, g_mggw_ov_new);
    if (g_mggw_fading)
        kv_log("      MERGE: FADING objects kept out of the merge: %.1f a frame; the most "
               "transparent was %.3f, in group %08X. They merge again at full opacity.",
               g_mggw_fading / f, g_mggw_fade_min, g_mggw_fade_grp);
    g_mggw_fading = 0; g_mggw_fade_min = 1.0f;
    g_mggw_appends = 0; g_mggw_ghosts = 0; g_mggw_overlaid = 0; g_mggw_ov_new = 0;
    mgg_state_report();
    {
        unsigned worst = 0, wi = MGG_N;
        for (i = 0; i < MGG_N; i++)
            if (g_mgg[i].used && g_mgg[i].rebuilds > worst) {
                worst = g_mgg[i].rebuilds; wi = i;
            }
        if (wi < MGG_N && worst > 3)
            kv_log("      MERGE: most-rebuilt group %08X pass %d -- %u rebuilds "
                   "this session, %u members, %u in its buffer. A static world "
                   "should rebuild a handful of times at most.",
                   g_mgg[wi].sig, g_mgg[wi].pass, worst, g_mgg[wi].built_n,
                   g_mgg[wi].ninbuf);
    }
    kv_log("      MERGE: groups by verdict so far --");
    for (i = 0; i < MGE_N; i++)
        if (g_mgg_why[i])
            kv_log("      MERGE:   %4u  %s", g_mgg_why[i], k_mge[i]);
    if (g_vrcfg.merge_group == 7 && g_mgg_arm_n[0] && g_mgg_arm_n[1] &&
        g_mgg_arm_n[2]) {
        double off  = g_mgg_arm_ms[0] / g_mgg_arm_n[0];
        double safe = g_mgg_arm_ms[1] / g_mgg_arm_n[1];
        double full = g_mgg_arm_ms[2] / g_mgg_arm_n[2];
        kv_log("      MERGE 3-WAY, world frames only, session: OFF %.3f ms "
               "(%.1f fps, %u) | SAFE %.3f ms (%.1f fps, %u) saves %.3f | "
               "FULL %.3f ms (%.1f fps, %u) saves %.3f",
               off, 1000.0 / off, g_mgg_arm_n[0],
               safe, 1000.0 / safe, g_mgg_arm_n[1], off - safe,
               full, 1000.0 / full, g_mgg_arm_n[2], off - full);
    }
    if ((g_vrcfg.merge_group == 3 || g_vrcfg.merge_group == 5 ||
         g_vrcfg.merge_group == 6) && g_mgg_arm_n[0] && g_mgg_arm_n[1])
    {
        kv_log("      MERGE LIVE A/B, world frames only, session: ON %.3f ms "
               "(%.1f fps, %u frames) | OFF %.3f ms (%.1f fps, %u frames) -- "
               "ON saves %.3f ms a frame",
               g_mgg_arm_ms[1] / g_mgg_arm_n[1],
               1000.0 * g_mgg_arm_n[1] / g_mgg_arm_ms[1], g_mgg_arm_n[1],
               g_mgg_arm_ms[0] / g_mgg_arm_n[0],
               1000.0 * g_mgg_arm_n[0] / g_mgg_arm_ms[0], g_mgg_arm_n[0],
               g_mgg_arm_ms[0] / g_mgg_arm_n[0] - g_mgg_arm_ms[1] / g_mgg_arm_n[1]);
        if (g_mggw_arm_n[0] && g_mggw_arm_n[1])
            kv_log("      MERGE LIVE A/B, this window: ON %.3f ms (%u) | OFF "
                   "%.3f ms (%u)", g_mggw_arm_ms[1] / g_mggw_arm_n[1],
                   g_mggw_arm_n[1], g_mggw_arm_ms[0] / g_mggw_arm_n[0],
                   g_mggw_arm_n[0]);
    }
    g_mggw_arm_ms[0] = g_mggw_arm_ms[1] = g_mggw_arm_ms[2] = 0.0;
    g_mggw_arm_n[0] = g_mggw_arm_n[1] = g_mggw_arm_n[2] = 0;
    if (g_vrcfg.merge_group == 4)
        kv_log("      MERGE VERIFY: %u groups tested offscreen: %u pixel-"
               "identical, %u equivalent (same coverage, no step above 8/255), "
               "%u FAILED; %u showed translucent pixels. Largest colour step "
               "seen anywhere: %u of 255.",
               g_mgv_tested, g_mgv_identical, g_mgv_equivalent,
               g_mgv_tested - g_mgv_equivalent, g_mgv_translucent,
               g_mgv_worst_step);
    g_mggw_frames = 0; g_mggw_skipped = 0; g_mggw_merged = 0;
    g_mggw_rebuilds = 0; g_mggw_elig_draws = 0; g_mggw_draws = 0;
    g_mggw_notbuf = 0;
}

/* ---- mode 5: every list from its own GPU buffer, IN PLACE --------------
   No merging and no reordering: at the moment the engine calls a list, the
   same geometry is drawn from a buffer object instead, block by block in its
   ORIGINAL primitive modes, under the engine's own modelview and state. The
   hypothesis it tests: the 40% saved by merging a third of the world came
   from the driver replaying glArrayElement lists vertex by vertex on the
   CPU, not from the draw count -- and if so, this gets it for every list,
   cut-outs and soft alpha included, with the draw order untouched.

   Executing a list also leaves the current normal, texcoord and colour at
   its last vertex, and the engine inherits them (the stage 3 audit saw the
   current normal differ between members for exactly this reason). After a
   draw from arrays those are undefined, so the list's side effect is put
   back by hand. */
static void mlv_upload(Mesh *me) {
    GLint prev = 0;
    if (!me->vbo) mgg_genbuf(1, &me->vbo);
    o_glGetIntegerv(MG_ARRAY_BUFFER_BINDING, &prev);
    mgg_bindbuf(MG_ARRAY_BUFFER, me->vbo);
    mgg_bufdata(MG_ARRAY_BUFFER, (ptrdiff_t)(me->nvert * sizeof(BaVert)),
                me->v, MG_STATIC_DRAW);
    mgg_bindbuf(MG_ARRAY_BUFFER, (GLuint)prev);
    me->vbo_gen = me->gen; me->vbo_ok = 1;
}
static void mlv_arrays_on(const Mesh *me, GLint *prev) {
    int u;
    o_glGetIntegerv(MG_ARRAY_BUFFER_BINDING, prev);
    ba_pushc(MG_CLIENT_ARRAY_BIT);
    if (ba_clientactive)
        for (u = 3; u >= 0; u--) {
            ba_clientactive(GL_TEXTURE0_ARB_ + u);
            ba_disablecs(GL_TEXTURE_COORD_ARRAY_);
        }
    ba_disablecs(GL_INDEX_ARRAY_);
    ba_disablecs(GL_EDGE_FLAG_ARRAY_);
    mgg_bindbuf(MG_ARRAY_BUFFER, me->vbo);
    ba_enablecs(GL_VERTEX_ARRAY_);
    ba_vp(3, GL_FLOAT, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, pos));
    if (me->has_n) { ba_enablecs(GL_NORMAL_ARRAY_);
                     ba_np(GL_FLOAT, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, nrm)); }
    else ba_disablecs(GL_NORMAL_ARRAY_);
    if (me->has_t) { ba_enablecs(GL_TEXTURE_COORD_ARRAY_);
                     ba_tp(2, GL_FLOAT, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, tc)); }
    if (me->has_c) { ba_enablecs(GL_COLOR_ARRAY_);
                     ba_cp(4, GL_UNSIGNED_BYTE, sizeof(BaVert), (const GLvoid *)offsetof(BaVert, col)); }
    else ba_disablecs(GL_COLOR_ARRAY_);
}
static void mlv_blocks(const Mesh *me) {
    unsigned b;
    for (b = 0; b < me->nb; b++)
        o_glDrawArrays(me->bmode[b], (GLint)me->bfirst[b], (GLsizei)me->bcount[b]);
}
static int mlv_member(GLuint list) {
    Mesh *me;
    GLint prev = 0;
    int eye, n;
    (void)eye;
    if (!mgg_ready()) return 0;
    me = mc_find(list, 0);
    if (!me || !me->ok || !me->nb || !me->nvert) return 0;
    g_mggw_draws++;
    if (g_vrcfg.merge_group == 5) g_mgg_frame_draws++;
    if (!g_mgg_on) return 0;
    if (!me->vbo_ok || me->vbo_gen != me->gen) mlv_upload(me);
    mlv_arrays_on(me, &prev);
    if (dup_active()) {
        n = pass_eye_count();
        DUP_LOOP(n, mlv_blocks(me));
    } else {
        mlv_blocks(me);
    }
    mgg_arrays_off(prev);
    {   /* the list's side effect on the current attributes */
        const BaVert *lv = &me->v[me->nvert - 1];
        if (me->has_n) o_glNormal3f(lv->nrm[0], lv->nrm[1], lv->nrm[2]);
        if (me->has_t) o_glTexCoord2f(lv->tc[0][0], lv->tc[0][1]);
        if (me->has_c) o_glColor4ub(lv->col[0], lv->col[1], lv->col[2], lv->col[3]);
    }
    g_mggw_skipped++;
    return 1;
}

/* ---- per member draw ---------------------------------------------------- */
static int mg_member(GLuint list) {
    Mesh *me;
    MgInst in;
    unsigned h = 2166136261u;
    const unsigned char *p;
    int i;
    if (g_vrcfg.merge_group == 5) return mlv_member(list);
    if (g_vrcfg.merge_group == 6) {
        /* The order-safe groups merge; any list that did not -- a cut-out,
           soft alpha, an unmatched or unaudited member -- is still drawn
           from its own buffer, in its original slot. Draw order is only
           ever changed where the census proved it cannot show. */
        if (mgg_member(list)) return 1;
        g_mggw_draws--;                /* mlv_member counts it again */
        return mlv_member(list);
    }
    if (g_vrcfg.merge_group >= 2) return mgg_member(list);
    if (g_mg.gave_up) return 0;
    if (g_view_depth < 0 || !g_ob_view[g_mv_sp]) return 0;
    me = mc_find(list, 0);
    if (!me || !me->ok || !me->mergeable) return 0;
    /* rolling: the census has to see the PLAYER's scene, not the menu the world
       first appears in */
    /* the census reads the texture binding back per draw: diagnostics only */
    if (g_vrcfg.diagnostics) mg_blend_note();
    if (!g_mg.chosen) {
        if (!g_mg_t0) g_mg_t0 = g_frames;       /* the world has started */
        if (MG_T(100) && !MG_T(400)) mg_select_note();
        return 0;
    }
    if (g_gk_sig != g_mg.sig || g_pass_idx != g_mg.pass) return 0;

    /* record this member */
    in.list = list; in.gen = me->gen;
    memcpy(in.obj, g_ob[g_mv_sp], 64);
    p = (const unsigned char *)in.obj;
    for (i = 0; i < 64; i++) h = (h ^ p[i]) * 16777619u;
    h = (h ^ (unsigned)list) * 16777619u;
    h = (h ^ in.gen) * 16777619u;
    in.key = h ? h : 1u;
    if (g_mg.ncur >= g_mg.capcur) {
        unsigned want = g_mg.capcur ? g_mg.capcur * 2 : 1024;
        MgInst *nc = (MgInst *)realloc(g_mg.cur, want * sizeof(MgInst));
        if (!nc) return 0;
        g_mg.cur = nc; g_mg.capcur = want;
    }
    g_mg.cur[g_mg.ncur++] = in;

    if (g_vrcfg.merge_group == 1) {
        if (!g_mga_done && MG_T(500)) mg_audit_member();
        if (!g_mg_ab_done && MG_T(600) && g_mg.valid && g_mg.ncur == 1)
            mg_ab();
        return 0;                      /* audit only: draw it normally */
    }
    if (!g_mg.valid) return 0;
    if (!mg_set_has(in.key)) { g_mg.w_unmatched++; return 0; }
    g_mg.w_matched++;
    if (!g_mg.drawn) { mg_draw(); g_mg.drawn = 1; }
    return 1;
}

static LARGE_INTEGER g_mgg_last;
static void mg_frame_end(void) {
    unsigned i;
    int same;
    g_pass_idx = 0;
    if (g_vrcfg.merge_group >= 2) {
        LARGE_INTEGER now;
        double ms = 0.0;
        QueryPerformanceCounter(&now);
        if (g_mgg_last.QuadPart && g_qpf.QuadPart)
            ms = 1000.0 * (double)(now.QuadPart - g_mgg_last.QuadPart) /
                 (double)g_qpf.QuadPart;
        g_mgg_last = now;
        mgg_frame_end(ms);
        return;
    }
    if (!g_vrcfg.merge_group || g_mg.gave_up) return;
    if (!g_mg.chosen) {
        if (g_mg_t0 && g_frames == g_mg_t0 + 400) mg_choose();
        return;
    }
    g_mg.w_frames++;
    if (g_vrcfg.merge_group == 1 && !g_mga_done && MG_T(500)) {
        if (++g_mga_frames >= 2) { g_mga_done = 1; mg_audit_report(); }
        else g_mga_have_ref = 0;       /* each frame's members against its own first */
    }
    /* rebuild if the set of members is not exactly the buffer's */
    same = g_mg.valid && g_mg.ncur == g_mg.ninst;
    for (i = 0; same && i < g_mg.ncur; i++)
        if (!mg_set_has(g_mg.cur[i].key)) same = 0;
    if (!same && g_mg.ncur) mg_build();
    if (!g_mg.ncur) g_mg.valid = 0;
    g_mg.ncur = 0; g_mg.drawn = 0;
}

/* engine read-backs, timed in the glGet hooks (tools/get_timing.py) */
static LONGLONG g_get_ticks, g_get_max;
static unsigned g_get_n;
static unsigned g_get_pn[8]; static unsigned g_get_pc[8]; static LONGLONG g_get_pt[8];

/* ---- WGL -------------------------------------------------------------- */
static void report(void) {
    DWORD now = GetTickCount();
    double secs = (now - g_report_tick) / 1000.0;
    kv_log("--- frame %u | %.1f fps ---", g_frames,
           secs > 0.0 ? 300.0 / secs : 0.0);
    kv_log("  matmode P=%u MV=%u ident=%u | multmat f=%u d=%u frustum=%u ortho=%u",
           g_s.matmode_proj, g_s.matmode_mv, g_s.loadidentity, g_s.multmatf,
           g_s.multmatd, g_s.frustum, g_s.ortho);
    kv_log("  draws: glBegin=%u glDrawArrays=%u glDrawElements=%u clear=%u",
           g_s.begin, g_s.drawarrays, g_s.drawelements, g_s.clear);
    kv_log("  engine asked for its viewport %u times", g_n_getviewport);
    kv_log("  stereo: scene dups=%u hud dups=%u | makeCurrent=%u",
           g_s.dup_scene, g_s.dup_hud, g_makecurrent);
    kv_log("  engine camera: fovy=%.2f deg aspect=%.4f  (gluPerspective has "
           "already run by the time we patch, so this staying at 45 is "
           "EXPECTED and proves nothing either way)", g_eng_fovy, g_eng_aspect);
    kv_log("  >>> THE MEASUREMENT: draws per frame = %.1f.  If widening the "
           "engine camera widened its culling, this goes UP.",
           g_s.swap ? (double)(g_s.begin + g_s.drawelements) / g_s.swap : 0.0);
    fov_report();
    kv_log("  draw distance: engine far plane now %.0f (shipped %.0f, x%.1f)",
           g_far, g_far_original, g_vrcfg.draw_distance);
    kv_log("  engine camera widened to %.1f deg; head is %.1f deg off the "
           "camera axis%s", g_last_engine_fov, g_head_dev_deg,
           g_fov_clamped ? "  <<< CLAMPED: turned too far, edges will still be "
                           "culled" : "  (fully covered)");
    vr_cull_coverage_report(g_near);
    kv_log("  head-driven culling: the view was turned on %u of %u frames%s",
           g_headcull_hits, g_s.swap,
           (g_vrcfg.head_cull && g_headcull_hits * 2 < g_s.swap)
               ? "  <<< the camera is being sized as if the view follows the "
                 "head, and it mostly does not -- expect holes at the edges"
               : "");
    g_headcull_hits = 0;
    kv_log("  engine read back GL matrices: projection=%u modelview=%u "
           "(non-zero means it may derive its culling from them, which we "
           "could then control)", g_n_getproj, g_n_getmv);
    if (g_n_drawpixels | g_n_bitmap | g_n_copypixels | g_n_copytex)
        kv_log("  >>> UNCOVERED DRAW PATHS: glDrawPixels=%u glBitmap=%u "
               "glCopyPixels=%u glCopyTex*=%u -- these are NOT duplicated per "
               "eye, so anything drawn through them lands in neither eye",
               g_n_drawpixels, g_n_bitmap, g_n_copypixels, g_n_copytex);
    kv_log("  2D pass: %u draws last frame -> %s panel (menu_draws=%d)",
           g_hud_last, g_menu_up ? "MENU" : "HUD", g_vrcfg.menu_draws);
    if (g_qpf.QuadPart && g_s.swap)
        kv_log("  duplication cost per frame: immediate-mode/display-list %.2f ms, "
               "vertex-array %.2f ms",
               1000.0 * (double)g_t_list / (double)g_qpf.QuadPart / g_s.swap,
               1000.0 * (double)g_t_elem / (double)g_qpf.QuadPart / g_s.swap);
    if (g_qpf.QuadPart && g_s.swap) {
        double k = 1000.0 / (double)g_qpf.QuadPart / (double)g_s.swap;
        kv_log("      glCallList %.2f ms over %u calls, glCallLists %.2f ms "
               "over %u -- these two were never timed before and are most of "
               "the duplication",
               (double)g_t_cl * k,  g_n_cl / g_s.swap,
               (double)g_t_cls * k, g_n_cls / g_s.swap);
    }
    kv_log("      over %u immediate-mode blocks (glBegin/glEnd replay) and %u "
           "vertex-array draws per frame (a zero cost with a zero count means "
           "the path never ran)",
           g_n_lists / (g_s.swap ? g_s.swap : 1),
           g_n_elems / (g_s.swap ? g_s.swap : 1));
    /* Where the duplication goes.  Setup and restore are timed inside the
       two helpers, so they span EVERY path; the per-path figures above span
       their own path only.  The first version of this subtracted one from the
       other to get "re-issuing the draw" and that was arithmetic across two
       different populations -- 3368 loops against 439 -- so it is gone rather
       than corrected.  What is printed now is only what was measured.

       The loop count is the honest total: exactly one restore per duplicated
       draw, whichever path issued it.  If the four per-path counts do not sum
       to it, a path is still unaccounted for. */
    /* Every pass-through that can force a threaded driver to stop and meet
       the game: anything named glGet* or glIs*, plus glGetError, glFinish
       and glReadPixels. Counted by the thunks already; listed per frame. */
    if (g_s.swap) {
        static unsigned last[512];
        int i, n = g_real_n < 512 ? g_real_n : 512, any = 0;
        for (i = 0; i < n; i++) {
            const char *nm = k_names_px_[i];
            unsigned d = g_real_count[i] - last[i];
            last[i] = g_real_count[i];
            if (!d || !nm) continue;
            if (!strncmp(nm, "glGet", 5) || !strncmp(nm, "glIs", 4) ||
                !strcmp(nm, "glFinish") || !strcmp(nm, "glReadPixels") ||
                !strcmp(nm, "glFlush")) {
                kv_log("      SYNC POINTS: %-28s %.1f a frame (pass-through)",
                       nm, (double)d / g_s.swap);
                any = 1;
            }
        }
        if (!any) kv_log("      SYNC POINTS: no pass-through query reached the driver");
        kv_log("      READ-BACK SHADOWS: modelview %s (%u checks), projection %s "
               "(%u), viewport %s (%u); %u constant reads answered from cache "
               "this window.",
               g_rb_mv.trust > 0 ? "ANSWERING" : g_rb_mv.trust < 0 ? "OFF" : "verifying",
               g_rb_mv.checks,
               g_rb_pj.trust > 0 ? "ANSWERING" : g_rb_pj.trust < 0 ? "OFF" : "verifying",
               g_rb_pj.checks,
               g_rb_vp.trust > 0 ? "ANSWERING" : g_rb_vp.trust < 0 ? "OFF" : "verifying",
               g_rb_vp.checks, g_rb_const_hits);
        {
            int i;
            HMODULE exe = GetModuleHandleA(NULL);
            for (i = 0; i < 8 && g_ge[i].ra; i++)
                kv_log("      glGetError CALLER exe+0x%06X: %u calls, %u errors, "
                       "first 0x%04X at frame %u",
                       (unsigned)((char *)g_ge[i].ra - (char *)exe), g_ge[i].n,
                       g_ge[i].errs, g_ge[i].first, g_ge[i].first_frame);
        }
        kv_log("      READ-BACK SHADOWS: scissor enable %s (%u), current colour %s "
               "(%u); the port's own reads that still reached the driver: "
               "%.1f a frame",
               g_rb_sc.trust > 0 ? "ANSWERING" : g_rb_sc.trust < 0 ? "OFF" : "verifying",
               g_rb_sc.checks,
               g_rb_cc.trust > 0 ? "ANSWERING" : g_rb_cc.trust < 0 ? "OFF" : "verifying",
               g_rb_cc.checks, g_s.swap ? (double)g_isync_n / g_s.swap : 0.0);
        g_isync_n = 0;
        kv_log("      READ-BACK SHADOWS: glGetError %s (%u), material %s (%u), "
               "light %s (%u)",
               g_rb_err.trust > 0 ? "ANSWERING" : g_rb_err.trust < 0 ? "OFF" : "verifying",
               g_rb_err.checks,
               g_rb_mat.trust > 0 ? "ANSWERING" : g_rb_mat.trust < 0 ? "OFF" : "verifying",
               g_rb_mat.checks,
               g_rb_lt.trust > 0 ? "ANSWERING" : g_rb_lt.trust < 0 ? "OFF" : "verifying",
               g_rb_lt.checks);
        g_rb_const_hits = 0;
    }
    if (g_qpf.QuadPart && g_s.swap && g_get_n) {
        double us = 1e6 / (double)g_qpf.QuadPart;
        int i;
        kv_log("      READ-BACKS: the engine made %.1f GL read-backs a frame that "
               "reached the driver, %.3f ms a frame in total, %.2f us each on "
               "average, slowest %.1f us. Around a microsecond each means the "
               "driver runs on the game's thread; hundreds mean it is already "
               "threading and stalling on every read.",
               (double)g_get_n / g_s.swap,
               (double)g_get_ticks * us / 1000.0 / g_s.swap,
               (double)g_get_ticks * us / g_get_n, (double)g_get_max * us);
        for (i = 0; i < 8 && g_get_pc[i]; i++)
            kv_log("      READ-BACKS:   pname 0x%04X  %.1f a frame, %.2f us each",
                   g_get_pn[i], (double)g_get_pc[i] / g_s.swap,
                   (double)g_get_pt[i] * us / g_get_pc[i]);
        g_get_ticks = 0; g_get_max = 0; g_get_n = 0;
        memset(g_get_pc, 0, sizeof(g_get_pc));
        memset(g_get_pt, 0, sizeof(g_get_pt));
    }
    if (g_qpf.QuadPart && g_s.swap) {
        double k = 1000.0 / (double)g_qpf.QuadPart / (double)g_s.swap;
        unsigned loops = g_n_restore / g_s.swap;
        unsigned named = (g_n_cl + g_n_cls + g_n_lists + g_n_elems) / g_s.swap;
        kv_log("      state cost across ALL paths: per-eye setup %.2f ms (%u "
               "calls/frame), restore %.2f ms (%u/frame) -- %.2f ms of pure "
               "GL state per frame%s",
               (double)g_t_eyestate * k, g_n_eyestate / g_s.swap,
               (double)g_t_restore * k,  loops,
               ((double)g_t_eyestate + (double)g_t_restore) * k,
               g_vrcfg.dup_profile ? "" : "  (times need dup_profile = 1)");
        kv_log("      block arrays: %u blocks/frame, %u vertices/frame "
               "(largest block %u), multitexture calls inside blocks %u, "
               "lost %u (block_arrays=%d)",
               g_ba_blocks / g_s.swap, g_ba_verts / g_s.swap, g_ba_maxverts,
               g_ba_mtc_calls / g_s.swap, g_ba_lost, g_vrcfg.block_arrays);
        g_ba_blocks = 0; g_ba_verts = 0; g_ba_mtc_calls = 0;
        if (g_vrcfg.merge_group && g_mg_bc_n) {
            unsigned i;
            mg_blend_report();
            ta_report();
            for (i = 0; i < g_mg_bc_n; i++) g_mg_bc[i].n = 0;
            for (i = 0; i < TA_N; i++) g_ta_draws[i] = 0;
        }
        if (g_vrcfg.merge_group == 1 && !g_mg.chosen && !g_mg.gave_up) {
            unsigned i, held = 0, merg = 0, blocks = 0;
            for (i = 0; i < MC_N; i++)
                if (g_mc[i].id && g_mc[i].ok) {
                    held++; blocks += g_mc[i].nb;
                    if (g_mc[i].mergeable) merg++;
                }
            kv_log("      MERGE: waiting to choose -- %u meshes held, %u of them "
                   "mergeable (%u primitive blocks recorded). World first "
                   "drawn at frame %u%s.", held, merg, blocks, g_mg_t0,
                   g_mg_t0 ? "" : " -- NOT YET, so no clock is running");
        }
        if (g_vrcfg.merge_group >= 2) mgg_report();
        if (g_vrcfg.skip_noop) nop_report();
        if (g_vrcfg.merge_group == 1 && g_mg.chosen && g_mg.w_frames) {
            double f = (double)g_mg.w_frames;
            kv_log("      MERGE: group %08X pass %d -- buffer holds %u "
                   "instances, %u vertices (%u KB). Per frame: %.1f members "
                   "matched, %.1f not in the buffer (drawn normally), %.2f "
                   "merged draws issued. %u rebuilds, %u instances excluded "
                   "(mode %d).",
                   g_mg.sig, g_mg.pass, g_mg.ninst, g_mg.nv,
                   (unsigned)((g_mg.nv * sizeof(BaVert)) / 1024),
                   g_mg.w_matched / f, g_mg.w_unmatched / f,
                   g_mg.w_merged / f, g_mg.w_rebuilds, g_mg.w_excluded,
                   g_vrcfg.merge_group);
            g_mg.w_frames = 0; g_mg.w_matched = 0; g_mg.w_unmatched = 0;
            g_mg.w_merged = 0; g_mg.w_rebuilds = 0; g_mg.w_excluded = 0;
        }
        if (g_vrcfg.mv_shadow && g_mvcw_frames > 0.0) {
            double pct = g_mvcw_seen > 0.0 ? 100.0 * g_mvcw_same / g_mvcw_seen
                                           : 0.0;
            kv_log("      STATIC CENSUS: %.0f%% of %.0f world draws a frame "
                   "were the SAME mesh at the SAME matrix as the previous "
                   "frame. That is the share of a merged buffer that would "
                   "not need rebuilding.",
                   pct, g_mvcw_seen / g_mvcw_frames);
            kv_log("      STATIC CENSUS control: %.0f distinct placements a "
                   "frame against %.0f draws. These must be close -- a key "
                   "too coarse makes everything match and reports a "
                   "triumphant figure above that means nothing.",
                   g_mvcw_distinct / g_mvcw_frames,
                   g_mvcw_seen / g_mvcw_frames);
            g_mvcw_distinct = 0;
            kv_log("      STATIC CENSUS: %u multiplies at the camera's own "
                   "stack level AFTER objects had started. Must be 0: anything "
                   "else means the engine moves the base between objects and "
                   "the view/object split cannot be trusted.", g_view_late);
            g_view_late = 0;
            kv_log("      STATIC CENSUS: %.0f draws a frame came from a stack "
                   "level holding no view, so their placement could not be "
                   "judged and they are left out of the figure above.",
                   g_mvcw_noview / g_mvcw_frames);
            g_mvcw_noview = 0;
            kv_log("      STATIC CENSUS control: %.0f%% of draws called a "
                   "mesh that was also drawn last frame. This one must be "
                   "near 100%% -- if it is not, the census above is "
                   "measuring nothing.",
                   g_mvlw_seen > 0.0 ? 100.0 * g_mvlw_same / g_mvlw_seen : 0.0);
            kv_log("      MODELVIEW SHADOW: largest difference from GL %s; "
                   "stack overflow %u, underflow %u.",
                   g_mv_maxdiff < 0.0 ? "not yet checked" : "see the line above",
                   g_mv_overflow, g_mv_underflow);
            g_mvcw_same = 0; g_mvcw_seen = 0; g_mvcw_frames = 0;
            g_mvlw_same = 0; g_mvlw_seen = 0;
        }
        if (g_vrcfg.mesh_capture && g_mcw_frames > 0.0) {
            unsigned i, ok = 0, bad = 0;
            for (i = 0; i < MC_N; i++)
                if (g_mc[i].id) { if (g_mc[i].ok) ok++; else bad++; }
            kv_log("      MESH CAPTURE: %u meshes held, %u vertices, %u KB. "
                   "%u lists could NOT be captured.",
                   ok, g_mc_verts_held,
                   (unsigned)((g_mc_verts_held * sizeof(BaVert)) / 1024), bad);
            kv_log("      MESH CAPTURE: %.0f vertices drawn per frame across "
                   "the world pass; %.1f lists re-compiled per frame; %.1f "
                   "draws a frame called a list we do not hold.",
                   g_mcw_vert_drawn / g_mcw_frames,
                   (double)g_mcw_recompiles / g_mcw_frames,
                   (double)g_mcw_miss / g_mcw_frames);
            for (i = 1; i < MC_REASONS; i++)
                if (g_mc_bad[i])
                    kv_log("      MESH CAPTURE:   %u x %s", g_mc_bad[i],
                           k_mc_why[i]);
            kv_log("      MESH CAPTURE: vertices per frame is the number that "
                   "decides the CPU transform cost, and re-compiles per frame "
                   "decides whether a mesh cache keyed on list id can stand.");
            g_mcw_vert_drawn = 0; g_mcw_frames = 0;
            g_mcw_recompiles = 0; g_mcw_miss = 0;
        }
        if (g_vrcfg.group_census && g_gcw_frames) {
            double f = (double)g_gcw_frames;
            double draws  = g_gcw_draws  / f;
            double states = g_gcw_states / f;
            double lists  = g_gcw_lists  / f;
            double single = g_gcw_single / f;
            kv_log("      GROUP CENSUS: %.0f world draws a frame fall into "
                   "%.0f distinct STATE groups and %.0f distinct display "
                   "lists. Largest state group %u draws, most-repeated list "
                   "%u times.",
                   draws, states, lists, g_gcw_max, g_gcw_listmax);
            kv_log("      GROUP CENSUS: %.0f draws (%.0f%%) sit alone in a "
                   "group of one and can never be merged with anything. "
                   "Sorting by state would cut %.0f draws to %.0f, a factor "
                   "of %.1f.",
                   single, draws > 0 ? 100.0 * single / draws : 0.0,
                   draws, states, states > 0 ? draws / states : 0.0);
            kv_log("      GROUP CENSUS: this is a LOWER bound on the group "
                   "count -- the key covers only the state this proxy hooks, "
                   "so two draws differing in anything else look identical. "
                   "A bad number here is genuinely bad; a good one needs a "
                   "wider key before it is believed.%s",
                   g_gc_full ? "  WARNING: the table saturated, so the counts "
                               "are short." : "");
            g_gcw_states = 0; g_gcw_lists = 0; g_gcw_draws = 0;
            g_gcw_single = 0; g_gcw_frames = 0;
            g_gcw_max = 0; g_gcw_listmax = 0;
        }
        if (g_runs) {
            run_close();
            kv_log("      BATCHABILITY: %u list calls a frame arrive in %u "
                   "runs of consecutive draws with no state change between "
                   "them -- mean %.1f per run, longest %u. Runs of 1: %u, "
                   "2: %u, 3-4: %u, 5-8: %u, 9-16: %u, 17+: %u. Batching the "
                   "second eye is worth doing only if the mean is well above "
                   "1.",
                   g_run_lists / (g_s.swap ? g_s.swap : 1),
                   g_runs / (g_s.swap ? g_s.swap : 1),
                   g_runs ? (double)g_run_lists / g_runs : 0.0, g_run_max,
                   g_run_hist[0], g_run_hist[1], g_run_hist[2],
                   g_run_hist[3], g_run_hist[4], g_run_hist[5]);
            g_runs = 0; g_run_lists = 0; g_run_max = 0;
            memset(g_run_hist, 0, sizeof(g_run_hist));
        }
        if (g_vrcfg.diagnostics && g_lst_count) {
            int i, n = g_real_n < 512 ? g_real_n : 512;
            kv_log("      INSIDE LISTS: %u lists compiled. What they record "
                   "(anything other than geometry means a state cache goes "
                   "stale when the list is CALLED):", g_lst_count);
            for (;;) {
                int best = -1; unsigned bv = 0;
                for (i = 0; i < n; i++)
                    if (g_lst_hits[i] > bv) { bv = g_lst_hits[i]; best = i; }
                if (best < 0) break;
                kv_log("      INSIDE LISTS:   %-26s %.2f per list",
                       k_names_px_[best], (double)bv / g_lst_count);
                g_lst_hits[best] = 0;
            }
            g_lst_count = 0;
        }
        if (g_vrcfg.diagnostics && g_gap_n) {
            int i, n = g_real_n < 512 ? g_real_n : 512;
            kv_log("      BETWEEN DRAWS: %.2f calls in the average gap "
                   "between two list calls. Gaps of 0:%u 1:%u 2:%u 3:%u "
                   "4:%u 5-8:%u 9-16:%u 17+:%u",
                   (double)g_gap_total / g_gap_n, g_gap_hist[0],
                   g_gap_hist[1], g_gap_hist[2], g_gap_hist[3],
                   g_gap_hist[4], g_gap_hist[5], g_gap_hist[6],
                   g_gap_hist[7]);
            for (;;) {
                int best = -1; unsigned bv = 0;
                for (i = 0; i < n; i++)
                    if (g_gap_hits[i] > bv) { bv = g_gap_hits[i]; best = i; }
                if (best < 0) break;
                kv_log("      BETWEEN DRAWS:   %-28s %.2f per draw",
                       k_names_px_[best], (double)bv / g_gap_n);
                g_gap_hits[best] = 0;
            }
            g_gap_n = 0; g_gap_total = 0;
            memset(g_gap_hist, 0, sizeof(g_gap_hist));
        }
        if (g_sf_seen) {
            kv_log("      STATE FILTER: %u of %u state calls a frame were "
                   "already set and were skipped (%.0f%%). state_filter=%d; "
                   "F5 toggles it.",
                   g_sf_skipped / (g_s.swap ? g_s.swap : 1),
                   g_sf_seen / (g_s.swap ? g_s.swap : 1),
                   100.0 * g_sf_skipped / g_sf_seen, g_vrcfg.state_filter);
            g_sf_skipped = 0; g_sf_seen = 0;
        }
        if (g_finish_calls)
            kv_log("      glFinish: the engine called it %u times over these "
                   "%u frames and %u were skipped (skip_glfinish=%d). Each "
                   "one it does make stops the CPU until the GPU has "
                   "drained.",
                   g_finish_calls, g_s.swap, g_finish_skipped,
                   g_vrcfg.skip_glfinish);
        g_finish_calls = 0; g_finish_skipped = 0;
        kv_log("      eye switches %u/frame, deferred restores flushed "
               "%u/frame (dup_alternate=%d: one switch per draw and a flush "
               "only when the engine looks)",
               g_dup_switches / g_s.swap, g_dup_flushes / g_s.swap,
               g_vrcfg.dup_alternate);
        g_dup_switches = 0; g_dup_flushes = 0;
        kv_log("      %u duplicated draws per frame, %u of them named above%s",
               loops, named,
               (loops == named || g_vrcfg.stereo_pass_list ||
                g_vrcfg.dup_alternate)
                   ? (g_vrcfg.dup_alternate
                          ? " (restores are per pass with dup_alternate)"
                          : " -- the parts add up")
                   : " -- MISMATCH, a path is still untimed");
    }
    if (g_vrcfg.stereo_pass_list && g_qpf.QuadPart && g_s.swap) {
        double k = 1000.0 / (double)g_qpf.QuadPart / (double)g_s.swap;
        kv_log("      pass-list stereo: %u passes/frame in %u segments, %u "
               "draws/frame replayed for the other eye in %.2f ms/frame "
               "(overflows %u, GL errors %u)%s",
               g_pl_passes / g_s.swap, g_pl_segs / g_s.swap,
               g_pl_draws_total / g_s.swap, (double)g_pl_t_replay * k,
               g_pl_overflow, g_pl_errors,
               g_pl_passes ? "" : "  <<< NOTHING recorded: the per-draw path "
                                  "is carrying every draw");
        g_pl_passes = 0; g_pl_segs = 0; g_pl_draws_total = 0;
        g_pl_t_replay = 0;
    }
    g_t_list = 0; g_t_elem = 0; g_n_lists = 0; g_n_elems = 0;
    g_t_cl = 0; g_t_cls = 0; g_n_cl = 0; g_n_cls = 0;
    g_t_eyestate = 0; g_t_restore = 0; g_n_eyestate = 0; g_n_restore = 0;
    vr_input_report();
    kv_log("  mirror: %u presents, %u skipped (target %.0f fps) -- presenting "
           "every frame is what pins the game to the monitor",
           g_presents, g_present_skips, g_vrcfg.mirror_fps);
    g_presents = 0; g_present_skips = 0;
    if (g_qpf.QuadPart && g_s.swap) {
        double k = 1000.0 / (double)g_qpf.QuadPart / (double)g_s.swap;
        double whole = g_t_frame * k;
        double fin = g_t_finish * k, mir = g_t_mirror * k;
        double wait = g_t_wait * k, inp = g_t_input * k;
        kv_log("  where the frame goes (ms/frame of %.2f): eyes+submit %.2f, "
               "desktop window %.2f, waiting on the compositor %.2f, input "
               "%.2f, UNACCOUNTED %.2f",
               whole, fin, mir, wait, inp,
               whole - fin - mir - wait - inp);
        kv_log("      UNACCOUNTED is the game drawing its own frame, us "
               "duplicating it, and the driver.  Waiting on the compositor is "
               "SLACK, not cost: a large figure there means the game is inside "
               "its budget and something else is setting the rate.");
        {
            double acq = 0, wt = 0, bl = 0, en = 0;
            int ew = 0, eh = 0, ww = 0, wh = 0;
            vr_finish_breakdown(g_s.swap, &acq, &wt, &bl, &en);
            vr_eye_and_window(&ew, &eh, &ww, &wh);
            kv_log("      eyes+submit splits into: acquire %.2f, waiting for "
                   "the image %.2f, our blit %.2f, xrEndFrame %.2f  "
                   "(eye %dx%d, window %dx%d)",
                   acq, wt, bl, en, ew, eh, ww, wh);
            kv_log("      Read it like this: WAITING FOR THE IMAGE means the "
                   "GPU or the compositor is behind and we are queued behind "
                   "it. OUR BLIT means the copy itself. XR_END_FRAME means "
                   "the runtime. They need different fixes and only one of "
                   "them is mine to make faster.");
        }
    }
    g_t_finish = g_t_mirror = g_t_wait = g_t_input = g_t_frame = 0;
    /* The size the ENGINE thinks it is drawing.  The eye textures are cut
       from the game window, so this is what sets the VR resolution -- and
       if the engine is clamping to the desktop rather than to what
       settings.ini asked for, a bigger window is worth trying. */
    kv_log("  the engine last set its viewport to %d,%d %dx%d",
           g_eng_vp[0], g_eng_vp[1], g_eng_vp[2], g_eng_vp[3]);
    if (g_vrcfg.hud_ab) {
        double both = g_arm_fps_n[0] ? g_arm_fps_sum[0] / g_arm_fps_n[0] : 0.0;
        double one = g_arm_fps_n[1] ? g_arm_fps_sum[1] / g_arm_fps_n[1] : 0.0;
        kv_log("  HUD A/B: 2D in both eyes %.1f fps over %u frames | 2D in "
               "the left eye only %.1f fps over %u | difference %+.1f fps. "
               "This is a running mean over every screen visited, and the "
               "two arms only saw the same ones by luck -- the paired "
               "per-window comparison put the sign at -10 to +15, so read "
               "tools/hud_ab_paired.py before believing this number.",
               both, g_arm_fps_n[0], one, g_arm_fps_n[1], one - both);
        g_hud_arm = !g_hud_arm;
        kv_log("  HUD A/B: next 300 frames draw the 2D in %s",
               g_hud_arm ? "the LEFT EYE ONLY" : "BOTH EYES");
    }
    if (g_dim_frames)
        kv_log("  menu dim: the game was dimming on %u of %u frames",
               g_dim_frames, g_s.swap);
    else if (g_dim_best_ok)
        kv_log("  menu dim: nothing to match -- the darkest translucent colour "
               "the 2D pass used was %.2f %.2f %.2f at alpha %.2f, which is a "
               "texture being modulated rather than a dimming quad. Expected "
               "on screens the game leaves bright; a near-black colour here "
               "instead would mean the search is missing one.",
               g_dim_best[0], g_dim_best[1], g_dim_best[2], g_dim_best[3]);
    else
        kv_log("  menu dim: nothing to match; the 2D pass drew nothing "
               "translucent at all.");
    g_dim_frames = 0;
    g_dim_best_ok = 0;
    if (g_qpf.QuadPart && g_s.swap) {
        double k = 1000.0 / (double)g_qpf.QuadPart / (double)g_s.swap;
        kv_log("  the ENGINE's own frame: %.2f ms before its first draw call "
               "(simulation, or waiting), %.2f ms submitting draws. A static "
               "menu that is mostly the first figure is the engine pacing "
               "itself, not the draw calls costing.",
               g_t_idle * k, g_t_draw * k);
    }
    g_t_idle = g_t_draw = 0;
    {
        double g = vr_gpu_ms();
        if (g >= 0.0) {
            double k = g_qpf.QuadPart && g_s.swap
                       ? 1000.0 / (double)g_qpf.QuadPart / (double)g_s.swap
                       : 0.0;
            double cpu = g_t_draw * k;
            kv_log("  GPU %.2f ms/frame against %.2f ms of CPU submitting -- "
                   "%s", g, cpu,
                   g > cpu * 0.9
                       ? "the card is at least as busy as the CPU, so it is the "
                         "limit here and resolution or fill is worth cutting"
                       : "the card is idle next to the CPU, so it is NOT the "
                         "limit and lowering resolution would buy nothing");
        }
    }
    kv_log("  draws by pass: unclaimed %u, world %u, 2D %u%s",
           g_mode_draws[0], g_mode_draws[1], g_mode_draws[2],
           g_mode_draws[0] > 200
               ? "  <<< the unclaimed ones are drawn once, with the ENGINE's "
                 "viewport, so they land at canvas size in a corner of the eye"
               : "");
    if (g_mode_draws[0] > 200)
        kv_log("      the viewport those went to was %d,%d %dx%d, against an "
               "eye of %dx%d", g_orphan_vp[0], g_orphan_vp[1], g_orphan_vp[2],
               g_orphan_vp[3], vr_eye_width(), vr_eye_height());
    g_mode_draws[0] = g_mode_draws[1] = g_mode_draws[2] = 0;
    if (g_stall_total) {
        unsigned i;
        kv_log("  STALLS: %u frame(s) over %.0f ms in this window. Where each "
               "went -- a stall that is mostly 'compositor' is the headset "
               "link, one that is mostly 'rest' is the game or the driver:",
               g_stall_total, g_vrcfg.stall_ms);
        for (i = 0; i < g_nstall; i++)
            kv_log("     frame %u: %.1f ms = eyes+submit %.1f + compositor "
                   "%.1f + mirror %.1f + rest %.1f  (%u draws)",
                   g_stall[i].frame, g_stall[i].ms, g_stall[i].submit,
                   g_stall[i].wait, g_stall[i].mirror,
                   g_stall[i].ms - g_stall[i].submit - g_stall[i].wait -
                   g_stall[i].mirror, g_stall[i].draws);
        g_nstall = 0; g_stall_total = 0;
    }
    kv_log("  pacing: worst frame %.1f ms, %u frames over 25 ms",
           g_worst_ms, g_stalls);
    g_worst_ms = 0.0;
    g_stalls = 0;
    memset(&g_s, 0, sizeof(g_s));
    g_draws_prev = 0;
    g_report_tick = now;
}


/* The game dims the world behind a menu by drawing a dark quad into its 2D
   canvas -- a NAVY one, measured, not black: see g_dim_rgb -- which in here
   is a panel of a set angular size, so the dimming
   stops at the panel's edge and the world is bright beyond it.  Carry it out to
   the edge of the eye.

   Drawn at the END of the frame: a frame holds several world passes and several
   2D passes, so anything painted at a transition is either overdrawn by the
   next world pass or applied twice by the next menu pass.

   Everything here sets its own state.  The engine leaves blending, depth, the
   colour mask, texturing and the scissor however its last draw wanted them, and
   an overlay that inherits any of those is the classic way to paint nothing at
   all and look like a logic bug. */

/* Called on the first few draws of each 2D pass.  A near-black colour at
   partial alpha, with blending on, is the backdrop the game lays over the
   world behind a menu -- and its alpha is the number the surround has to
   match. */
static void note_backdrop_colour(void) {
    GLfloat c[4];
    /* Normally we stop at the first backdrop found. A trace wants the whole
       population, or it is the same partial view that caused this. */
    if (g_dim_seen && !g_vrcfg.dim_trace) return;
    /* EVERY draw of the 2D pass, not one in sixteen.  The backdrop is a
       single draw, so sampling made the surround appear and disappear between
       openings of the same menu as the element count shifted it in and out of
       the sampled set -- reported as "sometimes large, sometimes small".
       The thinning was sized for the world pass; this runs in the 2D pass,
       which is 44 draws in game and about 233 with a menu up. The cap is two
       orders of magnitude above that and exists only so a pathological pass
       cannot run away. */
    g_dim_draws++;
    /* The cap was 1024, and the blueprint DETAILS page draws 2915: it is
       rendered ON TOP of the flowchart, so the flowchart's own 1200 draws
       come first and the details backdrop lands past the cap and is never
       looked at. The flowchart alone (1201 draws) squeaked under it, which
       is why one screen dimmed and the other did not.

       There is no real cost to raising it: the search stops the moment it
       finds a backdrop, and GL_CURRENT_COLOR is a client-side query with
       no pipeline sync. The number now exists only so a pathological pass
       cannot run away. */
    if (g_dim_checks >= 16384) return;
    g_dim_checks++;
    g_internal++;
    if (g_cc_valid && g_rb_cc.trust == 1) {
        c[0] = g_cc[0]; c[1] = g_cc[1]; c[2] = g_cc[2]; c[3] = g_cc[3];
    } else {
        o_glGetFloatv(GL_CURRENT_COLOR, c);
        g_isync_n++;
        if (g_cc_valid && g_rb_cc.trust == 0) {
            /* ub colours round-trip through 1/255: compare in those steps */
            float a[4], b[4]; int k;
            for (k = 0; k < 4; k++) {
                a[k] = (float)(int)(g_cc[k] * 255.0f + 0.5f);
                b[k] = (float)(int)(c[k] * 255.0f + 0.5f);
            }
            rb_check(&g_rb_cc, a, b, 4);
        }
        g_cc[0] = c[0]; g_cc[1] = c[1]; g_cc[2] = c[2]; g_cc[3] = c[3];
        g_cc_valid = 1;
    }
    g_internal--;
    if (c[3] > 0.15f && c[3] < 0.95f &&
        c[0] < 0.25f && c[1] < 0.25f && c[2] < 0.25f) {
        g_dim_seen = 1;
        g_dim_alpha = c[3];
        g_dim_rgb[0] = c[0]; g_dim_rgb[1] = c[1]; g_dim_rgb[2] = c[2];
        g_dim_by_cover = 0;
        return;
    }
    /* Not a dark colour -- but a translucent primitive spanning essentially
       the whole canvas is a backdrop whatever colour it carries.  The
       blueprint details page draws a dark TEXTURE modulated by white, so its
       colour is unreadable and its SIZE is the only honest signal. An icon
       cannot pass this: the in-game HUD's translucent white at the same alpha
       covers a few percent of the canvas, not ninety. */
    if (g_vrcfg.menu_dim_wide_search &&
        c[3] > 0.15f && c[3] < 0.95f && g_dim_cover >= 0.90f) {
        g_dim_seen = 1;
        g_dim_alpha = c[3];
        g_dim_rgb[0] = c[0]; g_dim_rgb[1] = c[1]; g_dim_rgb[2] = c[2];
        g_dim_by_cover = 1;
        return;
    }
    /* Keep the darkest translucent thing seen, so a failure can say what it
       was looking at instead of only that it failed. */
    if (c[3] < 0.99f && (!g_dim_best_ok ||
                         c[0] + c[1] + c[2] < g_dim_best[0] + g_dim_best[1] +
                                              g_dim_best[2])) {
        g_dim_best[0] = c[0]; g_dim_best[1] = c[1];
        g_dim_best[2] = c[2]; g_dim_best[3] = c[3];
        g_dim_best_ok = 1;
    }
    if (g_vrcfg.dim_trace && g_dim_trace_armed && c[3] < 0.99f &&
        g_dim_traced < 400 && g_dim_trace_total < 4000) {
        g_dim_trace_total++;
        g_dim_traced++;
        kv_log("  dim trace: pass %d %-5s draw %-3d via %-13s colour "
               "%.2f %.2f %.2f alpha %.2f covers %3.0f%%",
               g_pass_no,
               g_dup_mode == DUP_HUD ? "2D" :
                   (g_dup_mode == DUP_SCENE ? "scene" : "none"),
               g_dim_checks, g_dim_path, c[0], c[1], c[2], c[3],
               g_dim_cover * 100.0f);
    }
    if (c[3] > 0.05f && c[3] < 0.99f && g_dim_cover > g_dim_bestcover)
        g_dim_bestcover = g_dim_cover;
    if (c[3] < 0.99f && (!g_dim_bestf_ok ||
                         c[0] + c[1] + c[2] < g_dim_bestf[0] + g_dim_bestf[1] +
                                              g_dim_bestf[2])) {
        g_dim_bestf[0] = c[0]; g_dim_bestf[1] = c[1];
        g_dim_bestf[2] = c[2]; g_dim_bestf[3] = c[3];
        g_dim_bestf_ok = 1;
    }
}

static void (APIENTRY *d_push)(GLbitfield);
static void (APIENTRY *d_pop)(void);
static void (APIENTRY *d_pushm)(void);
static void (APIENTRY *d_popm)(void);
static void (APIENTRY *d_col)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *d_blend)(GLenum, GLenum);
static void (APIENTRY *d_rect)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *d_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
/* For compositing the captured 2D panel. Resolved on first use beside the
   dim painter's own, because they are plain passthroughs the port never
   intercepts. */
static void (APIENTRY *d_texcoord)(GLfloat, GLfloat);
static void (APIENTRY *d_bindtex)(GLenum, GLuint);
static void (APIENTRY *d_texenvi)(GLenum, GLenum, GLint);
static void (APIENTRY *d_vertex2f)(GLfloat, GLfloat);
static int  d_failed;

/* ---- measuring the dim, rather than recognising it -------------------- */

/* Three points near the edges of the viewport, where a menu's own panel
   rarely reaches but a full-screen backdrop always does.  Sampled before the
   2D pass and after it: whatever the engine drew in between, the difference
   is what it did to the world. */
static void (APIENTRY *d_readpx)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                 GLenum, void *);
static float g_dim_before[3];
static int   g_dim_have_before;
static unsigned g_dim_sample_frame;

#define DIM_SAMPLES 4

/* One brightness per sample point, kept separate: the whole discrimination
   depends on comparing the points against each other, and averaging them
   throws away exactly that. */
static int sample_edges(float *out) {
    GLint vp[4];
    int k;
    if (!d_readpx) {
        d_readpx = (void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                      GLenum, void *))
                   proxy_real_proc("glReadPixels");
        if (!d_readpx) return 0;
    }
    g_internal++;
    o_glGetIntegerv(GL_VIEWPORT, vp);
    g_internal--;
    if (vp[2] < 64 || vp[3] < 64) return 0;
    for (k = 0; k < DIM_SAMPLES; k++) {
        GLubyte px[3];
        /* Along the BOTTOM strip. The mid-height points were reading the
           pause menu's own parchment panel rather than the world, which
           is why one of the three disagreed with the other two -- the
           measurement was partly of the menu, not of what the menu did
           to the world. The bottom band is world in every screen seen so
           far, in game and with a menu up. */
        GLint x = vp[0] + vp[2] * (2 * k + 1) / (2 * DIM_SAMPLES);
        GLint y = vp[1] + vp[3] / 14;
        px[0] = px[1] = px[2] = 0;
        g_internal++;
        d_readpx(x, y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        g_internal--;
        out[k] = (px[0] + px[1] + px[2]) / (3.0f * 255.0f);
    }
    return 1;
}

/* Once every few frames.  A rolling baseline of the BRIGHTEST recent reading
   -- instant to rise, very slow to fall -- and anything materially below it
   is the game dimming the view.  Deliberately independent of the menu
   classifier, which has been shown to both overshoot and undershoot. */
static void dim_sample_after(void) {
    float cur[DIM_SAMPLES], drop[DIM_SAMPLES];
    float lo, hi, mean;
    int k, usable = 0;
    static float base[DIM_SAMPLES];
    static int   have_base;

    /* Before every gate, because three silent early returns in a row is
       how this has failed all evening. */
    if (g_vrcfg.dim_trace && (g_frames % 180) == 0)
        kv_log("dim measured: gates -- measure=%d in_level=%d near=%.1f "
               "since_last=%u", g_vrcfg.menu_dim_measure, kv_in_level(),
               g_near, g_frames - g_dim_sample_frame);
    if (!g_vrcfg.menu_dim_measure) return;
    if (!kv_in_level()) { g_dim_measured = 0.0f; return; }
    if (g_frames - g_dim_sample_frame < 6) return;
    g_dim_sample_frame = g_frames;
    if (!sample_edges(cur)) {
        if (g_vrcfg.dim_trace)
            kv_log("dim measured: could not read the framebuffer at all");
        return;
    }
    if (!have_base) {
        for (k = 0; k < DIM_SAMPLES; k++) base[k] = cur[k];
        have_base = 1;
        return;
    }

    /* Each point against its own brightest-recent reading. */
    lo = 2.0f; hi = -2.0f; mean = 0.0f;
    for (k = 0; k < DIM_SAMPLES; k++) {
        if (cur[k] >= base[k]) base[k] = cur[k];
        else base[k] *= 0.9995f;        /* barely decays, so a long menu
                                           cannot erode the reference */
        if (base[k] < 0.04f) { drop[k] = 0.0f; continue; }
        drop[k] = 1.0f - cur[k] / base[k];
        usable++;
        if (drop[k] < lo) lo = drop[k];
        if (drop[k] > hi) hi = drop[k];
        mean += drop[k];
    }
    if (usable < DIM_SAMPLES) { g_dim_confirm = 0; g_dim_measured = 0.0f; return; }
    mean /= DIM_SAMPLES;

    if (g_vrcfg.dim_trace)
        kv_log("dim measured: drops %.0f%% %.0f%% %.0f%% %.0f%% -- spread "
               "%.0f points. An overlay darkens every point by the same "
               "fraction; a camera moving over different ground does not.",
               drop[0] * 100.0f, drop[1] * 100.0f, drop[2] * 100.0f,
               drop[3] * 100.0f, (hi - lo) * 100.0f);

    /* Measured here: playing gives drops like 12/34/61 -- a spread of 49
       points -- while the pause menu gives 46/46/46. It is the AGREEMENT
       that identifies an overlay, not the size of the drop. */
    if (mean < 0.15f || mean > 0.95f || (hi - lo) > 0.08f) {
        g_dim_confirm = 0;
        g_dim_measured = 0.0f;
        return;
    }
    if (g_dim_confirm < 2) { g_dim_confirm++; return; }
    g_dim_measured = mean;
}

static void dim_around_panel(void) {
    int eye, n;
    float m[16];
    float aspect, alpha;
    /* Are we painting the canvas as well as the bands?  Only while standing
       in for a backdrop the game has stopped drawing, and only when the 2D
       pass is a texture that will be composited on top of us. */
    int fill_canvas;
    if (g_vrcfg.menu_dim <= 0.0f || !vr_stereo_active()) return;
    /* Only when there is a world behind the panel to dim.  The splash screens
       have no world camera and were being dimmed too -- they carry a dark
       translucent element of their own, which is enough to satisfy the
       backdrop detector, and there is nothing behind them to darken. */
    if (!kv_in_level()) return;
    /* ONLY when the game is measurably dimming, and only by as much as it
       is.  Firing on the port's menu classification instead was tried, to
       catch panels whose backdrop is a texture the colour detector cannot
       see -- and it darkened the blueprint menu and the flow chart, which the
       game deliberately leaves bright.  A menu dimmed that the game did not
       dim is worse than one left alone. */
    /* The HELD state is a reason to draw, exactly like a backdrop that was
       seen. Without this the surround stays away while the canvas fill paints
       on its own, which is a dark box in a bright world -- and before the
       fill existed, it is why holding showed nothing whatsoever. */
    if (!g_dim_seen && !g_dim_holding && !g_vrcfg.menu_dim_hold_force) {
        /* Measured, not recognised: this game draws every 2D element with a
           white vertex colour, so there is no dark quad for a colour test to
           find, and the draws that matter are display lists whose extent is
           not visible either. What CAN be seen is what the pass did to the
           view. */
        if (g_vrcfg.menu_dim_measure && g_dim_measured > 0.0f) {
            g_dim_alpha = g_dim_measured;
        } else if (g_vrcfg.menu_dim_alpha_only && g_menu_up && g_dim_bestf_ok &&
                   g_dim_bestf[3] >= 0.15f && g_dim_bestf[3] <= 0.95f) {
            g_dim_alpha = g_dim_bestf[3];
        } else {
            return;
        }
    }
    fill_canvas = (g_vrcfg.menu_dim_hold &&
                   (g_dim_holding || g_vrcfg.menu_dim_hold_force) &&
                   g_menu_up && g_vrcfg.panel_once && g_panel_have);
    /* The assertion, not a theory: painting our fill on a frame where the
       game drew its own is the double dim, and it must never happen. */
    if (fill_canvas && g_dim_was_real && !g_vrcfg.menu_dim_hold_force)
        g_dim_both_frames++;
    alpha = g_dim_alpha * g_vrcfg.menu_dim;
    /* Same fallback as the canvas fill: on a screen the game never dims there
       is no measured alpha, and the control would test nothing. */
    if (alpha <= 0.0f && g_vrcfg.menu_dim_hold_force)
        alpha = 0.49f * g_vrcfg.menu_dim;
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    if (!dim_procs_ready()) return;
    aspect = (g_hud_h > 0.0f) ? (g_hud_w / g_hud_h) : 1.7778f;
    n = vr_eye_count();
    g_internal = 1;
    d_push(GL_ALL_ATTRIB_BITS);
    for (eye = 0; eye < n; eye++) {
        float x0, x1, y0, y1;
        int vx, vy, vw, vh;
        vr_eye_viewport(eye, &vx, &vy, &vw, &vh);
        o_glViewport(vx, vy, vw, vh);
        /* world_lock 1: the same matrix the 2D itself is drawn with, so
           the surround is locked to the panel rather than to the head. */
        vr_hud_projection(eye, aspect, g_vrcfg.menu_size, 1, m);
        /* Nothing to compute: the rectangles below are in CANVAS space and
           go through this same matrix, so they sit around the panel wherever
           it is. */
        x0 = -1.0f; x1 = 1.0f; y0 = -1.0f; y1 = 1.0f;

        o_glMatrixMode(GL_PROJECTION);
        d_pushm();
        o_glLoadMatrixf(m);          /* the panel's own space */
        o_glMatrixMode(GL_MODELVIEW);
        d_pushm();
        o_glLoadIdentity();

        o_glDisable(GL_DEPTH_TEST);
        o_glDisable(GL_TEXTURE_2D);
        o_glDisable(GL_LIGHTING);
        o_glDisable(GL_CULL_FACE);
        o_glDisable(GL_SCISSOR_TEST_);
        o_glDisable(GL_ALPHA_TEST);
        d_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        o_glEnable(GL_BLEND);
        d_blend(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (g_vrcfg.menu_dim_probe)
            d_col(0.0f, 0.25f, 1.0f, alpha);
        else
            d_col(g_dim_rgb[0], g_dim_rgb[1], g_dim_rgb[2], alpha);
        if (g_vrcfg.menu_dim_probe && eye == 0) {
            static float said[4];
            if (said[0] != x0 || said[1] != x1 ||
                said[2] != y0 || said[3] != y1) {
                said[0] = x0; said[1] = x1; said[2] = y0; said[3] = y1;
                kv_log("DIM: the game is laying %.2f %.2f %.2f at alpha %.2f "
                       "behind this panel; matching it outside, in CANVAS "
                       "space, out to 8x the canvas (about 81 degrees from "
                       "the anchor). Larger values silently draw nothing at "
                       "all.", g_dim_rgb[0], g_dim_rgb[1], g_dim_rgb[2],
                       g_dim_alpha);
            }
        }

        /* Four bands AROUND the canvas, in canvas coordinates, reaching K
           times past it -- far enough to leave the view in every direction,
           which is the whole point: the original complaint was that the game's
           own dimming stopped at the panel edge.

           Around and not over: the panel already carries the game's dimming,
           and covering it again would darken the menu by the square of the
           factor. */
        {
            /* 8, and NOT larger.  The blue probe is unambiguous: at 8 the
               bands draw, and at 40 and 500 nothing reaches the screen at
               all -- quads hundreds of metres across at 1.5 m depth do not
               survive the pipeline.  Both enlargements silently switched the
               feature OFF, and because the surround is black that is
               indistinguishable from it working, which is why two rounds of
               "nothing changed" were literally true.

               8 reaches about 81 degrees from the anchor.  A flat plane cannot
               exceed 90 whatever the number, so the remaining few degrees were
               never worth having -- and chasing them cost the whole
               feature. */
            /* 25 Sep: bands in the panel's plane stop short once the panel
               leans (menu_tilt), and the dim's edges showed like the edges of a
               TV screen. So now: mark the panel's own area in depth (depth
               0 there, nothing drawn), then fill the WHOLE eye wherever the
               depth is not 0. Every direction is covered, whatever the
               panel's tilt or height, and the panel itself still is not. The
               bands remain as the fallback if the depth calls are missing. */
            static void (APIENTRY *p_range)(GLclampd, GLclampd);
            static int p_tried;
            if (!p_tried) {
                p_tried = 1;
                p_range = (void (APIENTRY *)(GLclampd, GLclampd))proxy_real_proc("glDepthRange");
            }
            if (p_range) {
                /* an offset would move the slanted panel's depth off 0 by a
                   different amount than the flat fill's (merged scenery uses
                   one; it must not be inherited here) */
                o_glDisable(GL_POLYGON_OFFSET_FILL);
                d_mask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                o_glEnable(GL_DEPTH_TEST);
                o_glDepthFunc(GL_ALWAYS);
                o_glDepthMask(GL_TRUE);
                p_range(0.0, 0.0);
                d_rect(x0, y0, x1, y1);  /* the panel: depth 0 */
                d_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                o_glDepthMask(GL_FALSE);
                o_glDepthFunc(GL_NOTEQUAL);
                o_glMatrixMode(GL_PROJECTION);
                o_glLoadIdentity();
                d_rect(-1.0f, -1.0f, 1.0f, 1.0f);   /* the eye, at depth 0 */
                o_glLoadMatrixf(m);
                o_glMatrixMode(GL_MODELVIEW);
                p_range(0.0, 1.0);
                o_glDisable(GL_DEPTH_TEST);
            } else {
                const float K = 8.0f;
                d_rect(-K, y1, K, K);        /* above */
                d_rect(-K, -K, K, y0);       /* below */
                d_rect(-K, y0, x0, y1);      /* left */
                d_rect(x1, y0, K, y1);       /* right */
            }

            /* And the canvas itself, while we are standing in for a backdrop
               the game has stopped drawing.  This is painted HERE, at the
               swap, rather than at the start of the 2D pass, for two
               reasons that are the whole of the transient:

               - Here the frame's own answer is known.  g_dim_holding was
                 computed from THIS frame's detection a few lines above, so
                 a frame in which the game drew its own backdrop cannot also
                 get ours.  At pass start the decision had to be made from
                 the previous frame, and entering a building's details -- the
                 frame the game resumes drawing its backdrop -- got both,
                 which is 0.49 squared for a frame or two.
               - It cannot cover the menu.  panel_composite() runs
                 immediately after this and draws the 2D pass over the top,
                 so the fill lands behind the text exactly as the game's own
                 backdrop does.  That was the only reason it lived at pass
                 start, and panel_once removed it. */
            if (fill_canvas) d_rect(x0, y0, x1, y1);

            /* And the canvas ITSELF, but only while holding.  "Around and not
               over" above is right whenever the game is drawing its own
               backdrop -- covering it again would darken the menu by the
               square of the factor.  The held state is the one case where it
               is NOT drawing one: that is the trigger.  Painting only the
               bands there leaves the 16:9 centre bright inside a dark
               surround, which reads as a reverse cutout of the flat screen
               and is exactly what showed backing out of the details page.

               The alpha is the one the game itself last used on this menu, so
               the held picture matches the same screen entered the other
               way. */
            /* The canvas itself is NOT filled here.  This runs at swap,
               after the game has drawn its menu, so a fill would cover the
               menu text rather than sit behind it.  When the held state needs
               one it is painted at the start of the 2D pass instead, by
               dim_fill_canvas() -- which is where the game's own backdrop
               would have gone. */
        }

        o_glMatrixMode(GL_MODELVIEW);
        d_popm();
        o_glMatrixMode(GL_PROJECTION);
        d_popm();
        o_glMatrixMode(GL_MODELVIEW);
    }
    d_pop();
    g_internal = 0;
}

static int dim_procs_ready(void) {
    if (d_failed) return 0;
    if (d_push) return 1;
    d_push  = (void (APIENTRY *)(GLbitfield))proxy_real_proc("glPushAttrib");
    d_pop   = (void (APIENTRY *)(void))proxy_real_proc("glPopAttrib");
    d_pushm = (void (APIENTRY *)(void))proxy_real_proc("glPushMatrix");
    d_popm  = (void (APIENTRY *)(void))proxy_real_proc("glPopMatrix");
    d_col   = (void (APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat))
              proxy_real_proc("glColor4f");
    d_blend = (void (APIENTRY *)(GLenum, GLenum))proxy_real_proc("glBlendFunc");
    d_rect  = (void (APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat))
              proxy_real_proc("glRectf");
    d_mask  = (void (APIENTRY *)(GLboolean, GLboolean, GLboolean, GLboolean))
              proxy_real_proc("glColorMask");
    if (!d_push || !d_pop || !d_pushm || !d_popm || !d_col || !d_blend ||
        !d_rect || !d_mask) {
        kv_log("menu dim: base GL entry points missing, leaving the world "
               "bright outside the panel");
        d_failed = 1;
        return 0;
    }
    return 1;
}

/* The canvas fill for the HELD dim.

   Same space and same alpha as the surround, but painted at the START of the
   2D pass so the menu draws on top of it, the way the game's own backdrop
   does. Only for the held state: whenever the game IS drawing its backdrop,
   adding ours would darken the menu by the square of the factor. */
static void dim_fill_canvas(void) {
    float m[16], aspect, alpha;
    int eye, n;

    if (!g_vrcfg.menu_dim_hold) return;
    if (kv_photo_mode()) return;          /* photo mode: no dim either */
    if (!(g_dim_holding || g_vrcfg.menu_dim_hold_force)) return;
    /* With panel_once the fill is painted at the SWAP instead, under the
       composited 2D texture, where the frame's own detection is already
       known and nothing can double up.  This path is the fallback for
       panel_once = 0, where the menu is drawn straight into the eyes and a
       fill at the swap would cover it. */
    if (g_vrcfg.panel_once) return;
    /* Two gates, not one.  The regression to avoid here is not a missing dim,
       it is dimming the world during play. */
    if (!g_menu_up) return;
    /* Once a frame, and only counted once it would really have painted --
       counting above the gate above made this number overstate the pass
       count it claims to report. */
    if (g_dim_filled_this_frame) { g_dim_fill_suppressed++; return; }
    if (!vr_stereo_active() || !dim_procs_ready()) return;

    alpha = (g_dim_held > 0.0f ? g_dim_held : g_dim_alpha) * g_vrcfg.menu_dim;
    /* The diagnostic needs an alpha on screens the game never dims, where
       there is no measured one to reuse -- otherwise the forced fill computes
       zero and returns, and the control tests nothing at all. 0.49 is what
       this game uses on the menus that do dim. */
    if (alpha <= 0.0f && g_vrcfg.menu_dim_hold_force)
        alpha = 0.49f * g_vrcfg.menu_dim;
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    aspect = (g_hud_h > 0.0f) ? (g_hud_w / g_hud_h) : 1.7778f;
    n = vr_eye_count();
    g_internal = 1;
    d_push(GL_ALL_ATTRIB_BITS);
    for (eye = 0; eye < n; eye++) {
        int vx, vy, vw, vh;
        vr_eye_viewport(eye, &vx, &vy, &vw, &vh);
        o_glViewport(vx, vy, vw, vh);
        vr_hud_projection(eye, aspect, g_vrcfg.menu_size, 1, m);
        o_glMatrixMode(GL_PROJECTION);
        d_pushm();
        o_glLoadMatrixf(m);
        o_glMatrixMode(GL_MODELVIEW);
        d_pushm();
        o_glLoadIdentity();
        o_glDisable(GL_DEPTH_TEST);
        o_glDisable(GL_TEXTURE_2D);
        o_glDisable(GL_LIGHTING);
        o_glDisable(GL_CULL_FACE);
        o_glDisable(GL_SCISSOR_TEST_);
        o_glDisable(GL_ALPHA_TEST);
        d_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        o_glEnable(GL_BLEND);
        d_blend(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        /* Green where the surround is blue, so one capture says which of the
           two rectangles drew.  Black on a dark menu is indistinguishable
           from not drawing at all. */
        if (g_vrcfg.menu_dim_probe)
            d_col(0.0f, 1.0f, 0.25f, alpha);
        else
            d_col(g_dim_held_rgb[0], g_dim_held_rgb[1], g_dim_held_rgb[2],
                  alpha);
        d_rect(-1.0f, -1.0f, 1.0f, 1.0f);
        o_glMatrixMode(GL_MODELVIEW);
        d_popm();
        o_glMatrixMode(GL_PROJECTION);
        d_popm();
        o_glMatrixMode(GL_MODELVIEW);
    }
    d_pop();
    g_internal = 0;
    g_dim_filled_this_frame = 1;
    {
        static int said;
        if (!said) {
            said = 1;
            kv_log("DIM: the game stopped drawing its own backdrop while a "
                   "full-screen menu is up, so the canvas is being filled at "
                   "alpha %.2f behind the menu. Without this the surround "
                   "alone reads as a bright 16:9 hole in a dark ring.",
                   alpha);
        }
    }
}

/* Apply the engine's own colour blend factors, but accumulate COVERAGE in
   the alpha channel, so the panel texture ends up premultiplied and can be
   composited exactly. */
static int panel_blend_ready(void) {
    if (!d_blendsep_tried) {
        d_blendsep_tried = 1;
        if (o_wglGetProcAddress)
            d_blendsep = (void (APIENTRY *)(GLenum, GLenum, GLenum, GLenum))
                         o_wglGetProcAddress("glBlendFuncSeparate");
        if (!d_blendsep)
            kv_log("PANEL: glBlendFuncSeparate is missing, so the panel's "
                   "alpha channel cannot be made correct. Rendering the 2D "
                   "pass once is disabled -- a wrong composite is worse than "
                   "a slow correct one.");
    }
    return d_blendsep != NULL;
}

static void panel_apply_blend(void) {
    if (d_blendsep)
        d_blendsep(g_panel_src, g_panel_dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

/* Draw the captured 2D panel into each eye: one textured quad instead of
   the couple of thousand draws the pass actually contains. */
static void panel_composite(void) {
    float m[16], aspect;
    int eye, n, tex;

    if (!g_vrcfg.panel_once || !g_panel_have) return;
    if (kv_photo_mode()) return;          /* photo mode: the world alone */
    tex = vr_panel_tex();
    if (!tex) return;
    if (!vr_stereo_active() && !g_vrcfg.panel_flat_test) return;
    if (!dim_procs_ready()) return;
    if (!d_texcoord) {
        d_texcoord = (void (APIENTRY *)(GLfloat, GLfloat))
                     proxy_real_proc("glTexCoord2f");
        d_bindtex  = (void (APIENTRY *)(GLenum, GLuint))
                     proxy_real_proc("glBindTexture");
        d_texenvi  = (void (APIENTRY *)(GLenum, GLenum, GLint))
                     proxy_real_proc("glTexEnvi");
        d_vertex2f = (void (APIENTRY *)(GLfloat, GLfloat))
                     proxy_real_proc("glVertex2f");
        if (!d_texcoord || !d_bindtex || !d_texenvi || !d_vertex2f) {
            kv_log("PANEL: cannot resolve the texturing entry points; the 2D "
                   "pass stays per-eye");
            g_vrcfg.panel_once = 0;
            return;
        }
    }

    aspect = (g_hud_h > 0.0f) ? (g_hud_w / g_hud_h) : 1.7778f;
    /* Flat diagnostic: one pass, straight back over the window at 1:1, so a
       correct capture is pixel-identical to not capturing at all. */
    n = vr_stereo_active() ? vr_eye_count() : 1;
    g_internal = 1;
    d_push(GL_ALL_ATTRIB_BITS);
    for (eye = 0; eye < n; eye++) {
        int vx, vy, vw, vh;
        if (vr_stereo_active()) {
            vr_eye_viewport(eye, &vx, &vy, &vw, &vh);
            o_glViewport(vx, vy, vw, vh);
            vr_hud_projection(eye, aspect,
                              g_menu_up ? g_vrcfg.menu_size : g_vrcfg.hud_size,
                              1, m);
        } else {
            int k;
            o_glViewport(g_eng_vp[0], g_eng_vp[1], g_eng_vp[2], g_eng_vp[3]);
            for (k = 0; k < 16; k++) m[k] = (k % 5) ? 0.0f : 1.0f;
        }
        o_glMatrixMode(GL_PROJECTION);
        d_pushm();
        o_glLoadMatrixf(m);
        o_glMatrixMode(GL_MODELVIEW);
        d_pushm();
        o_glLoadIdentity();

        o_glDisable(GL_DEPTH_TEST);
        o_glDisable(GL_LIGHTING);
        o_glDisable(GL_CULL_FACE);
        o_glDisable(GL_SCISSOR_TEST_);
        o_glDisable(GL_ALPHA_TEST);
        d_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        o_glEnable(GL_TEXTURE_2D);
        d_bindtex(GL_TEXTURE_2D, (GLuint)tex);
        d_texenvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_REPLACE_);
        o_glEnable(GL_BLEND);
        /* ONE, not SRC_ALPHA: the engine blended into a target cleared to
           zero, so the colour stored there is already multiplied by its own
           coverage.  Multiplying by alpha a second time squares it and every
           translucent element comes out too dark. */
        d_blend(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        d_col(1.0f, 1.0f, 1.0f, 1.0f);

        o_glBegin(GL_QUADS_);
        d_texcoord(0.0f, 0.0f); d_vertex2f(-1.0f, -1.0f);
        d_texcoord(1.0f, 0.0f); d_vertex2f( 1.0f, -1.0f);
        d_texcoord(1.0f, 1.0f); d_vertex2f( 1.0f,  1.0f);
        d_texcoord(0.0f, 1.0f); d_vertex2f(-1.0f,  1.0f);
        o_glEnd();

        o_glMatrixMode(GL_MODELVIEW);
        d_popm();
        o_glMatrixMode(GL_PROJECTION);
        d_popm();
        o_glMatrixMode(GL_MODELVIEW);
    }
    d_pop();
    g_internal = 0;
}

/* Borderless full screen, once.  The game's own FullScreen=yes takes the
   display mode exclusively, which rearranges the player's other windows every
   launch; this leaves the desktop alone and simply removes the frame.

   Not HWND_TOPMOST: a window that stays above everything is a trap in a port
   the player has to alt-tab out of, and the mirror has no claim to be there. */
static int g_borderless_done;
static void make_borderless(HDC hdc) {
    HWND w;
    HMONITOR mon;
    MONITORINFO mi;
    LONG style, ex;
    RECT cl;
    if (g_borderless_done || !g_vrcfg.borderless) return;
    w = WindowFromDC(hdc);
    if (!w) return;
    g_borderless_done = 1;

    mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoA(mon, &mi)) {
        kv_log("borderless: could not find the monitor; window left alone");
        return;
    }

    style = GetWindowLongA(w, GWL_STYLE);
    ex = GetWindowLongA(w, GWL_EXSTYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP;
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
            WS_EX_STATICEDGE);
    SetWindowLongA(w, GWL_STYLE, style);
    SetWindowLongA(w, GWL_EXSTYLE, ex);
    /* SWP_FRAMECHANGED or the old frame is still applied to the layout. */
    SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);

    memset(&cl, 0, sizeof(cl));
    GetClientRect(w, &cl);
    kv_log("borderless: window is now %dx%d at %d,%d on a %dx%d monitor. The "
           "engine's canvas is %dx%d -- these should match, and the game reads "
           "its canvas from settings.ini at startup, so a mismatch here means "
           "XScreenRes/YScreenRes need to be the monitor's size.",
           (int)(cl.right - cl.left), (int)(cl.bottom - cl.top),
           (int)mi.rcMonitor.left, (int)mi.rcMonitor.top,
           (int)(mi.rcMonitor.right - mi.rcMonitor.left),
           (int)(mi.rcMonitor.bottom - mi.rcMonitor.top),
           g_eng_vp[2], g_eng_vp[3]);
}

static void cur2d_frame_end(HDC hdc);
static void curtex_learn(void);
static void hudbox_frame_end(void);
BOOL WINAPI hk_wglSwapBuffers(HDC hdc) {
    BOOL r;
    LARGE_INTEGER now;
    g_frames++;
    cur2d_frame_end(hdc);
    curtex_learn();
    hudbox_frame_end();
    if (g_vrcfg.group_census) gc_frame_end();
    if (g_vrcfg.mesh_capture) mc_frame_end();
    if (g_vrcfg.mv_shadow) mvc_frame_end();
    mg_frame_end();
    nop_frame_end();
    /* A pass still recording at the swap has no next projection change to
       end it: replay it now, before anything else touches the eyes.  And
       a deferred per-eye state goes back before anything else draws. */
    pl_end();
    dup_flush();
    /* Every frame starts with no assumptions: this covers all of the port's
       own GL at swap time, which does not pass through these hooks. */
    sf_drop();
    g_pl_done = 0;
    /* Belt and braces: a draw that never goes through the per-eye setup would
       otherwise inherit whatever the last pass left enabled, and clip planes
       left on are a black screen rather than a small mistake. */
    eye_clip_planes_off();
    vr_gpu_frame_end();
    make_borderless(hdc);
    if (g_saw_first_draw && g_swap_left.QuadPart) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_t_draw += t.QuadPart - g_swap_left.QuadPart;
    }
    g_saw_first_draw = 0;
    g_s.swap++;

    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&now);
    if (g_last_qpc.QuadPart && g_qpf.QuadPart) {
        double ms = 1000.0 * (double)(now.QuadPart - g_last_qpc.QuadPart) /
                    (double)g_qpf.QuadPart;
        if (ms > g_worst_ms) g_worst_ms = ms;
        if (ms > 25.0) g_stalls++;
        {
            /* This frame's own share of each phase, by difference: the
               counters themselves only reset once per report. */
            double k = 1000.0 / (double)g_qpf.QuadPart;
            double sub = (double)(g_t_finish - g_pf_finish) * k;
            double wai = (double)(g_t_wait - g_pf_wait) * k;
            double mir = (double)(g_t_mirror - g_pf_mirror) * k;
            unsigned dr = (g_s.begin + g_s.drawelements) - g_pf_draws;
            if (ms > (double)g_vrcfg.stall_ms) {
                unsigned slot = g_nstall;
                g_stall_total++;
                if (slot >= STALLS) {
                    /* keep the worst */
                    unsigned i, w = 0;
                    for (i = 1; i < STALLS; i++)
                        if (g_stall[i].ms < g_stall[w].ms) w = i;
                    if (g_stall[w].ms >= ms) slot = STALLS;
                    else slot = w;
                }
                if (slot < STALLS) {
                    g_stall[slot].frame = g_frames;
                    g_stall[slot].ms = ms;
                    g_stall[slot].submit = sub;
                    g_stall[slot].wait = wai;
                    g_stall[slot].mirror = mir;
                    g_stall[slot].draws = dr;
                    if (g_nstall < STALLS) g_nstall++;
                }
            }
            g_pf_finish = g_t_finish; g_pf_wait = g_t_wait;
            g_pf_mirror = g_t_mirror; g_pf_draws = g_s.begin + g_s.drawelements;
        }
    }
    if (g_last_qpc.QuadPart) g_t_frame += now.QuadPart - g_last_qpc.QuadPart;
    g_last_qpc = now;
    /* g_s is a CENSUS that accumulates until the 300-frame report resets it,
       so adding it every frame summed a running total and produced figures in
       the tens of thousands.  Add the per-frame delta. */
    {
        unsigned total = g_s.begin + g_s.drawelements;
        g_arm_draws[g_arm] += (double)(total - g_draws_prev);
        g_draws_prev = total;
        g_arm_frames[g_arm]++;
    }
    if (g_vrcfg.hud_ab && g_qpf.QuadPart && g_s.swap && g_t_frame) {
        double ms = (double)g_t_frame * 1000.0 / (double)g_qpf.QuadPart
                    / (double)g_s.swap;
        if (ms > 0.0) {
            g_arm_fps_sum[g_hud_arm] += 1000.0 / ms;
            g_arm_fps_n[g_hud_arm]++;
        }
    }
    if ((g_frames % 300) == 0) {
        report();
        if (g_vrcfg.engine_tanx_ab) {
            double a = g_arm_frames[1] ? g_arm_draws[1] / g_arm_frames[1] : 0.0;
            double b = g_arm_frames[0] ? g_arm_draws[0] / g_arm_frames[0] : 0.0;
            kv_log("  A/B horizontal culling, same scene, alternating blocks: "
                   "narrowed to tan %.2f -> %.1f draws/frame over %u frames | "
                   "engine's own width -> %.1f over %u",
                   g_vrcfg.engine_tanx, a, g_arm_frames[1], b, g_arm_frames[0]);
            kv_log("      -> narrowed/normal = %.3f. Near 1.00 means the "
                   "engine does not cull horizontally and this route is dead.",
                   b > 0.0 ? a / b : 0.0);
            g_arm_draws[0] = g_arm_draws[1] = 0.0;
            g_arm_frames[0] = g_arm_frames[1] = 0;
        }
        g_arm = !g_arm;
    }
    /* These walk the whole address space and stall the render thread for
       seconds -- which the tester saw as the main menu freezing.  They are
       diagnostics, so they run only when something is actually asking. */
    /* Early, so the keys the MENUS use are caught too. */
    /* The key spy is deliberately NOT installed any more.  It answered what it
       was for -- this game never polls the keyboard, it reads messages -- and
       it did that by subclassing the window procedure and redirecting
       DefWindowProc, which is exactly the shape of thing that silently eats
       keyboard input.  Keys worked before it existed and stopped afterwards.
       A spent diagnostic that can break the thing it watches does not stay in
       the build. */
    /* Repeated, because the answer grows as the player plays: a key is only
       seen once the game has reason to ask about it. */
    if (g_frames == 300 && g_vrcfg.dump_strings) dump_engine_strings();
    if (g_frames == 300) pointer_selfcheck(3840, 1431);
    /* Find the game's own cursor, using the Windows cursor as the known
       value to search for -- the one moment the two are guaranteed to agree
       is before anything has tried to move either of them. */
    /* The draw distance can only be searched for once the WORLD camera has
       been seen, and at frame 300 the game is still in the menu, whose camera
       is a different one (near 10, far 2000 rather than near 70, far 2200).
       Gating this on a frame number meant it never ran in a real session --
       the same mistake as every other one-shot in this port. */
    {
        static int far_done;
        if (!far_done && g_far_original > 1.0 && g_near > 10.0) {
            far_done = 1;
            /* The field-of-view scan runs at frame 300 too, in the menu, so it
               searches for the MENU camera and finds nothing useful.  Run it
               again now that the world camera exists -- this is the camera the
               game actually culls the world against. */
            if (g_eng_fovy > 0.0f && g_eng_aspect > 0.0f)
                fov_scan(g_eng_fovy, g_eng_aspect);
            far_scan((float)g_far_original, (float)g_near);
            /* Dump the world camera's surroundings too.  The existing dump
               only ever ran at frame 300, in the menu, so it described a
               camera that never draws the game.  What is wanted here is a
               CULL RADIUS: the objects on the ground are clipped by a circle
               that is not centred on the player, which no frustum does. */
            /* Diagnostics only: a 40 KB file per launch in LOCALAPPDATA that
               nothing reads. The two scans above are load-bearing and stay. */
            if (g_vrcfg.diagnostics)
                dump_camera_world(g_eng_fovy, g_eng_aspect, (float)g_near,
                                  (float)g_far_original);
            if (g_vrcfg.draw_distance > 1.0f)
                far_apply((float)(g_far_original * g_vrcfg.draw_distance),
                          (float)g_far_original);
        }
    }
    if (g_frames == 60) {
        RECT dpi_rc;
        HWND dpi_w = WindowFromDC(hdc);
        memset(&dpi_rc, 0, sizeof(dpi_rc));
        if (dpi_w) GetClientRect(dpi_w, &dpi_rc);
        /* This used to assert that the desktop was 3840x2160 at 150%% and
           call any other reading a fault.  It is 2560x1440 at 100%% on this
           machine, so the line has been contradicting a correct measurement
           since it was written.  Print what is there and let it be compared
           against the window the game actually made. */
        kv_log("DPI: awareness %s (err %lu). Screen metrics say %dx%d, and the "
               "game made a %dx%d window -- if the metrics are smaller than the "
               "real desktop the process is still being scaled.",
               g_dpi_result == 1 ? "set per-monitor v2"
                                 : (g_dpi_result == 2 ? "set (legacy system)"
                                                      : "REFUSED"),
               (unsigned long)g_dpi_err,
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
               dpi_rc.right - dpi_rc.left, dpi_rc.bottom - dpi_rc.top);
    }
    /* fov_apply_second is NOT called.  The bisection that motivated it ran
       with no headset attached, so the ordinary field-of-view patch was not
       active either -- which means scaling a 45.0 widened the camera for the
       first time, and what looked like a second field of view was the known
       one being rediscovered.  Patching the extra copies changes the draw
       count by nothing (84221 against 84586), so the write is unfounded and
       stays out until a search with the wide camera already applied says
       otherwise. */
    /* Widen the camera at the END of the frame as well as when the projection
       is built.  The log says the engine rewrites these copies back to 45
       every frame, so our value only lives for the part of the frame between
       our patch and its rewrite.  The terrain is drawn inside that window --
       which is why widening works for it -- but if the objects are culled
       outside it, they are being tested against 45 degrees the whole time.
       Writing it again here covers the other half of the frame. */
    /* F9 toggles this live.  Comparing two settings by restarting the game
       gives two different camera positions and two different scenes, so the
       only honest comparison is the same frame with one thing changed -- which
       needs the change to happen without a restart.  Every conclusion today
       that came from comparing separate runs has been wrong. */
    {
        static int was_down;
        int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was_down) {
            g_vrcfg.fov_both_ends = !g_vrcfg.fov_both_ends;
            kv_log("TOGGLE: fov_both_ends is now %d",
                   g_vrcfg.fov_both_ends);
        }
        was_down = down;
    }
    if (g_vrcfg.fov_both_ends && g_last_engine_fov > 0.0f)
        fov_apply(g_last_engine_fov, 45.0f);
    /* Development scaffolding: memory walks and file writes, on the render
       thread. The batch at frame 300 was a 784 ms stall on a test PC. */
    if (g_vrcfg.diagnostics) {
        if (g_frames == 300) dump_modules();
        if (g_frames == 300) dump_rawinput();
        if (g_frames == 600) dump_game_strings();
    }
    /* The engine's GL vocabulary, one name per line the first time it is
       seen used.  A CPU capture of immediate-mode blocks has to cover
       every vertex/colour/texcoord/normal variant listed here. */
    if (g_vrcfg.diagnostics && (g_frames % 300) == 0) {
        int i, n = g_real_n < 512 ? g_real_n : 512;
        kv_log("BLOCK CENSUS: %u blocks so far, %u vertices (%.1f per block, "
               "largest %u); modes: points %u lines %u line_loop %u line_strip "
               "%u triangles %u tri_strip %u tri_fan %u quads %u quad_strip %u "
               "polygon %u",
               g_bc_blocks, g_bc_verts,
               g_bc_blocks ? (double)g_bc_verts / g_bc_blocks : 0.0,
               g_bc_maxverts, g_bc_modes[0], g_bc_modes[1], g_bc_modes[2],
               g_bc_modes[3], g_bc_modes[4], g_bc_modes[5], g_bc_modes[6],
               g_bc_modes[7], g_bc_modes[8], g_bc_modes[9]);
        for (i = 0; i < n; i++)
            if (g_bc_inside[i])
                kv_log("BLOCK CENSUS:   inside blocks: %s x%u",
                       k_names_px_[i], g_bc_inside[i]);
    }
    if (g_vrcfg.diagnostics && (g_frames % 300) == 0) {
        static unsigned char reported[512];
        int i;
        for (i = 0; i < g_real_n && i < 512; i++) {
            if (g_real_count[i] && !reported[i]) {
                reported[i] = 1;
                kv_log("GL CENSUS: %s first seen by frame %u (%u calls so far)",
                       k_names_px_[i], g_frames, g_real_count[i]);
            }
        }
    }
    if (g_trace) {
        /* The last pass of a frame was never reported, because the report was
           only emitted when the NEXT projection change arrived -- and the 2D
           pass is the last one.  That is precisely the pass the menus are
           drawn in, so the instrument was blind to the thing being asked
           about. */
        if (g_pass_no)
            kv_log("   pass %d (the LAST of the frame) ended with %u draws",
                   g_pass_no, g_pass_draws);
        kv_log("   engine used glScissor: %s", g_have_scissor ? "YES" : "no");
        kv_log("======== end of traced frame %u ========", g_frames);
    }
    /* Remember what the frame's final (2D) pass drew, and arm a trace when it
       jumps well clear of the running baseline -- i.e. when a menu opens. */
    /* An ABSOLUTE threshold, not a learned baseline.  Learning it from the
       in-game HUD works in game and fails at the main menu, where there is no
       HUD to learn from -- so the baseline got set from the menu itself,
       nothing ever exceeded it, and every front-end screen stayed tiny. */
    g_hud_last = g_pass_draws_final;
    {
        unsigned a = g_hud_last, b = g_dim_prev_draws;
        unsigned d = (a > b) ? (a - b) : (b - a);
        g_dim_trace_armed = (d > 20) ? 3 : (g_dim_trace_armed > 0 ?
                             g_dim_trace_armed - 1 : 0);
        if (g_vrcfg.dim_trace && d > 20)
            kv_log("dim trace: the 2D pass went %u -> %u draws; "
                   "listing its translucent draws", b, a);
        g_dim_prev_draws = a;
    }
    {
        int was = g_menu_up;
        g_menu_up = (g_hud_last >= (unsigned)g_vrcfg.menu_draws) ? 1 : 0;
        if (g_menu_up && !was) {
            g_trace_armed = 1;
            kv_log("2D pass at %u draws (threshold %d) -- treating as a menu",
                   g_hud_last, g_vrcfg.menu_draws);
            /* Per opening, because that is the unit the complaint is in:
               "sometimes large, sometimes small" is invisible in a figure
               averaged over 300 frames. */
            if (g_dim_seen)
                kv_log("   dim: FOUND the game's backdrop: colour %.2f %.2f "
                       "%.2f at alpha %.2f, by %s -- the surround will extend "
                       "it past the panel in that colour",
                       g_dim_rgb[0], g_dim_rgb[1], g_dim_rgb[2], g_dim_alpha,
                       g_dim_by_cover
                           ? "its SIZE (a translucent quad spanning the whole "
                             "canvas, colour unreadable because it is a "
                             "texture)"
                           : "its COLOUR (a dark translucent quad)");
            else if (g_dim_bestf_ok)
                kv_log("   dim: no backdrop. Inspected %d of the pass's %u "
                       "draws. Darkest translucent was %.2f %.2f %.2f at "
                       "alpha %.2f, and the largest any translucent draw "
                       "covered was %.0f%% of the canvas (a backdrop needs "
                       "90%%). Coverage near 90 means the threshold is wrong; "
                       "coverage near 0 with a dark colour means the backdrop "
                       "arrives by a path with no measurable extent; a small "
                       "figure with a WHITE colour means this screen simply "
                       "does not dim.",
                       g_dim_checks, g_hud_last, g_dim_bestf[0],
                       g_dim_bestf[1], g_dim_bestf[2], g_dim_bestf[3],
                       g_dim_bestcover * 100.0f);
            else
                kv_log("   dim: nothing translucent at all in %d draws of %d "
                       "-- this menu does not dim.", g_dim_checks, g_dim_draws);
        } else if (!g_menu_up && was) {
            kv_log("2D pass at %u draws -- back to the in-game HUD", g_hud_last);
        }
    }
    /* A traced frame is about a hundred flushed lines. g_trace_armed is set
       whenever the 2D pass jumps to menu size, so this fired on the frame
       after EVERY menu opening -- a hitch each time the tech tree opened
       or the blueprints. */
    g_trace = g_vrcfg.diagnostics &&
              (g_frames == 300 || g_frames == 900 || g_trace_armed ||
               (g_vrcfg.diagnostics == 5 && g_frames % 600 == 0));
    g_trace_armed = 0;
    if (g_trace) kv_log("======== TRACE frame %u ========", g_frames + 1);
    if (g_have_view) {
        memcpy(g_last_view, g_view_mat, sizeof(g_last_view));
        g_last_view_ok = 1;
    }
    g_have_view = 0;
    g_pass_draws_final = g_pass_draws;
    g_pass_no = 0;
    g_pass_draws = 0;

    /* Everything the game meant to draw is now in the target, so the world
       outside the menu panel can be brought down to match it. */
    eye_projection_cache_flush();
    g_dim_filled_this_frame = 0;
    vr_panel_anchor_frame(g_dup_mode == DUP_HUD || g_hud_last > 0);
    dim_sample_after();
    /* The complaint is about the dim APPEARING AND DISAPPEARING between
       screens, which a per-opening line cannot show: moving from the
       flowchart into its details page never re-opens a menu. Log the
       state change itself. */
    /* The game stops drawing its backdrop on the way out of a details page
       while the menu is still up.  Hold what it last drew.  Only ever extends
       a dim already found, so a mistake leaves a menu dimmed rather than
       darkening the world during play. */
    if (g_vrcfg.menu_dim_hold) {
        /* Measured on this game: the busiest in-game HUD is 318 draws and
           these full-screen menus are 1166-2915, so 600 sits in the middle of
           a gap of nearly four to one. */
        int big = (g_pass_draws_final >= 600);
        g_dim_was_real = g_dim_seen;
        if (g_dim_seen && big) {
            g_dim_held = g_dim_alpha;
            memcpy(g_dim_held_rgb, g_dim_rgb, sizeof(g_dim_held_rgb));
            g_dim_holding = 0;
            g_dim_hold_wait = 0;
        /* On the FIRST frame the backdrop goes missing, not the third.  The
           debounce was there because the fill used to be painted a frame
           late and a two-frame flash appeared at every screen change; now
           that the fill is decided and painted inside the same frame, the
           only thing three frames of waiting buys is three frames of
           undimmed world backing out of a building's details -- which is
           exactly the flicker being reported. */
        } else if (!g_dim_seen && big && g_dim_held > 0.0f &&
                   ++g_dim_hold_wait >= 1) {
            g_dim_seen = 1;
            g_dim_alpha = g_dim_held;
            memcpy(g_dim_rgb, g_dim_held_rgb, sizeof(g_dim_rgb));
            if (!g_dim_holding) {
                g_dim_holding = 1;
                kv_log("DIM HELD -- the game stopped drawing its backdrop "
                       "while a full-screen menu (%u draws) is still up, so "
                       "the dim it last drew (alpha %.2f) is being kept. Set "
                       "menu_dim_hold = 0 to follow the game exactly.",
                       g_pass_draws_final, g_dim_alpha);
            }
        } else if (!big) {
            g_dim_held = 0.0f;
            g_dim_holding = 0;
            g_dim_hold_wait = 0;
        }
    }
    {
        static int was = -1;
        if (g_dim_seen != was) {
            was = g_dim_seen;
            kv_log("DIM %s -- 2D pass %u draws, inspected %d (cap 16384), "
                   "backdrop %.2f %.2f %.2f alpha %.2f, darkest translucent "
                   "%.2f %.2f %.2f a=%.2f",
                   g_dim_seen ? "ON (backdrop found)" : "OFF (none found)",
                   g_pass_draws_final, g_dim_checks,
                   g_dim_rgb[0], g_dim_rgb[1], g_dim_rgb[2], g_dim_alpha,
                   g_dim_bestf[0], g_dim_bestf[1], g_dim_bestf[2],
                   g_dim_bestf[3]);
        }
    }
    if (g_dim_both_frames) {
        kv_log("DIM: %u frame(s) painted our canvas fill while the game was "
               "ALSO drawing its own backdrop -- that is the double dim, and "
               "this line must never appear.", g_dim_both_frames);
        g_dim_both_frames = 0;
    }
    if (g_dim_fill_suppressed && g_s.swap) {
        static unsigned said;
        if (g_dim_fill_suppressed != said) {
            said = g_dim_fill_suppressed;
            kv_log("DIM: the held canvas fill was suppressed %u times -- "
                   "extra post-world 2D passes it would otherwise have "
                   "painted in, and painting in each one is what made the "
                   "dim double up.",
                   g_dim_fill_suppressed);
        }
    }
    /* F9 toggles the capture LIVE.  Comparing two launches is hopeless:
       the world state differs between them, so a whole-image diff came
       back 33%% different and every pixel of it was trees. The tech tree
       pauses the game, so flipping this with the scene frozen gives two
       frames differing ONLY by the feature. */
    if (g_vrcfg.panel_flat_test) {
        static int was_down;
        int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was_down) {
            g_vrcfg.panel_once = !g_vrcfg.panel_once;
            kv_log("PANEL: toggled live -- panel_once now %d",
                   g_vrcfg.panel_once);
        }
        was_down = down;
    }
    if (g_vrcfg.panel_flat_test) {
        static int was_down10;
        int down10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down10 && !was_down10) {
            g_vrcfg.stereo_pass_list = !g_vrcfg.stereo_pass_list;
            kv_log("PASS LIST: toggled live -- stereo_pass_list now %d",
                   g_vrcfg.stereo_pass_list);
        }
        was_down10 = down10;
    }
    {
        static int was_down5;
        int down5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (down5 && !was_down5) {
            g_vrcfg.state_filter = !g_vrcfg.state_filter;
            sf_drop();
            kv_log("STATE FILTER: toggled live -- state_filter now %d",
                   g_vrcfg.state_filter);
        }
        was_down5 = down5;
    }
    {
        static int was_down6;
        int down6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (down6 && !was_down6) {
            g_vrcfg.cull_follow_head = !g_vrcfg.cull_follow_head;
            kv_log("CULL FOLLOW: toggled live -- cull_follow_head now %d",
                   g_vrcfg.cull_follow_head);
        }
        was_down6 = down6;
    }
    {
        static int was_down7;
        int down7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (down7 && !was_down7) {
            g_vrcfg.skip_glfinish = !g_vrcfg.skip_glfinish;
            kv_log("glFinish: toggled live -- skip_glfinish now %d. The frame "
                   "report's glFinish line says how many are being skipped.",
                   g_vrcfg.skip_glfinish);
        }
        was_down7 = down7;
    }
    if (g_vrcfg.panel_flat_test) {
        static int was_down8;
        int down8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (down8 && !was_down8) {
            g_vrcfg.block_arrays = !g_vrcfg.block_arrays;
            kv_log("BLOCK ARRAYS: toggled live -- block_arrays now %d",
                   g_vrcfg.block_arrays);
        }
        was_down8 = down8;
    }
    if (g_vrcfg.panel_flat_test) {
        static int was_down11;
        int down11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (down11 && !was_down11) {
            g_vrcfg.dup_alternate = !g_vrcfg.dup_alternate;
            kv_log("DUP: toggled live -- dup_alternate now %d",
                   g_vrcfg.dup_alternate);
        }
        was_down11 = down11;
    }
    /* Stop capturing BEFORE anything else draws into the eyes: everything
       below this belongs in the scene target, not in the panel. */
    if (g_panel_capture) {
        vr_panel_end();
        g_panel_capture = 0;
    }
    /* Outside the capture block: a scene pass later in the frame ends the
       capture, so gating the probe on it meant it never fired. The probe
       binds the framebuffer itself and does not need the capture live --
       only the texture, which g_panel_have vouches for. */
    /* A readback stalls the pipeline, so it is gated on the diagnostic
       rather than left to fire three times in a shipping build. */
    if (g_vrcfg.panel_flat_test && g_panel_have && g_menu_up) {
        static unsigned probes;
        if (probes < 3) { probes++; vr_panel_probe(0.13f, 0.22f); }
    }
    dim_around_panel();
    panel_composite();
    g_panel_have = 0;
    if (g_dim_seen) g_dim_frames++;
    g_dim_seen = 0;
    g_dim_alpha = 0.0f;
    g_dim_checks = 0;
    g_dim_draws = 0;
    g_dim_bestf_ok = 0;
    g_dim_by_cover = 0;
    g_dim_cover = 0.0f;
    g_dim_bestcover = 0.0f;
    g_dim_traced = 0;

    /* The back buffer still holds the frame the game just drew. */
    {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        vr_finish_frame(hdc);
        QueryPerformanceCounter(&b);
        g_t_finish += b.QuadPart - a.QuadPart;
    }
    /* THE DESKTOP MIRROR IS NOT FREE.  A windowed present is absorbed at the
       desktop's refresh whatever swap interval was asked for, so presenting
       every VR frame keeps the swap queue permanently full and pins the game
       to the monitor -- 90 fps becomes 60, and the block lands on the NEXT
       call that touches the default framebuffer, so it looks like our own
       render cost rather than the present.  Pace it off a wall clock at a rate
       well under the desktop refresh.

       Schedule by advancing whole periods, never from `now`: scheduling from
       now adds each frame's lateness to every interval, and 30 Hz asked comes
       out at 22 measured.  Clamp the catch-up to one period so a level load
       cannot burst. */
    r = TRUE;
    if (g_vrcfg.mirror_fps > 0.0f && g_qpf.QuadPart) {
        LONGLONG period = (LONGLONG)(g_qpf.QuadPart / g_vrcfg.mirror_fps);
        if (period < 1) period = 1;
        if (now.QuadPart >= g_next_present) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            vr_mirror_present(hdc);
            r = o_wglSwapBuffers(hdc);
            QueryPerformanceCounter(&b);
            g_t_mirror += b.QuadPart - a.QuadPart;
            g_presents++;
            g_next_present += period;
            if (g_next_present < now.QuadPart)      /* fell behind: do not
                                                       accumulate the debt */
                g_next_present = now.QuadPart + period;
        } else {
            g_present_skips++;
        }
    } else if (g_vrcfg.mirror_fps > 0.0f) {
        vr_mirror_present(hdc);
        r = o_wglSwapBuffers(hdc);
        g_presents++;
    }

    /* Wait on the compositor and locate the views for the frame the game is
       ABOUT to draw.  The head pose has to be known before the draws happen,
       not after -- which is why this is here and not above the swap. */
    {
        /* xrWaitFrame lives in here, and it BLOCKS until the compositor is
           ready for the next frame.  Time spent here is not cost, it is
           slack -- if this is large the game is comfortably inside its budget
           and something else sets the rate.  If it is near zero the game is
           the thing that is late. */
        LARGE_INTEGER a, b, c;
        QueryPerformanceCounter(&a);
        vr_prepare_frame(hdc);
        QueryPerformanceCounter(&b);
        input_emu_frame(hdc);
        QueryPerformanceCounter(&c);
        g_t_wait += b.QuadPart - a.QuadPart;
        g_t_input += c.QuadPart - b.QuadPart;
    }
    g_dup_mode = DUP_NONE;

    /* Everything the engine draws from here goes into our own target, not
       the window.  It has no idea, and does not need one: the per-eye
       viewports already point into halves of it. */
    vr_bind_scene_target();
    QueryPerformanceCounter(&g_swap_left);
    vr_check_scene_target(g_frames);
    vr_gpu_frame_begin();

    /* The patch is applied where the engine has just finished writing its
       camera, not here -- see hk_glMultMatrixd. */

    /* The game may never call wglSwapIntervalEXT, in which case the driver
       default applies; ask for 0 ourselves, once. */
    if (!g_swapint_done && g_vrcfg.enabled && g_vrcfg.force_vsync) {
        g_swapint_done = 1;
        if (!o_wglSwapIntervalEXT)
            o_wglSwapIntervalEXT = (BOOL (WINAPI *)(int))
                o_wglGetProcAddress("wglSwapIntervalEXT");
        if (o_wglSwapIntervalEXT) {
            o_wglSwapIntervalEXT(0);
            kv_log("vsync interval set to 0 (compositor paces VR frames)");
        }
    }
    return r;
}

BOOL WINAPI hk_wglSwapLayerBuffers(HDC hdc, UINT planes) {
    g_s.swap++;
    return o_wglSwapLayerBuffers(hdc, planes);
}

HGLRC WINAPI hk_wglCreateContext(HDC hdc) {
    HGLRC rc = o_wglCreateContext(hdc);
    kv_log("wglCreateContext(hdc=%p) -> %p", hdc, rc);
    return rc;
}

/* The engine calls this about EIGHT times per frame -- 27906 calls in a 3486
   frame session.  Logging each one buried everything else in the file, so
   report the context identity once and then only count. */
BOOL WINAPI hk_wglMakeCurrent(HDC hdc, HGLRC rc) {
    BOOL r;
    if (g_pl_rec && pl_getcurrent && rc != pl_getcurrent()) pl_end();
    dup_flush();
    sf_drop();
    r = o_wglMakeCurrent(hdc, rc);
    g_makecurrent++;
    /* 24 Sep: this used to reset the shadows on EVERY wglMakeCurrent. In VR
       the runtime re-binds the SAME context ~8 times a frame, and a re-bind
       leaves GL's state exactly as it was -- so a reset made the shadows
       disagree with GL until the engine next loaded its matrices. Now: a
       re-bind changes nothing; a brand-new context starts at GL's defaults;
       a switch back to another context already seen sends every read-back
       shadow back to verifying against the driver. */
    if (r && rc && rc != g_last_rc) {
        static HGLRC seen[8];
        static int nseen;
        int i, known = 0;
        for (i = 0; i < nseen; i++) if (seen[i] == rc) known = 1;
        if (!known) {
            if (nseen < 8) seen[nseen++] = rc;
            mv_init(); nx_defaults();
        } else {
            g_rb_mv.trust = g_rb_mv.trust > 0 ? 0 : g_rb_mv.trust; g_rb_mv.checks = 0;
            g_rb_pj.trust = g_rb_pj.trust > 0 ? 0 : g_rb_pj.trust; g_rb_pj.checks = 0;
            g_rb_vp.trust = g_rb_vp.trust > 0 ? 0 : g_rb_vp.trust; g_rb_vp.checks = 0;
            g_rb_mat.trust = g_rb_mat.trust > 0 ? 0 : g_rb_mat.trust; g_rb_mat.checks = 0;
            g_rb_lt.trust = g_rb_lt.trust > 0 ? 0 : g_rb_lt.trust; g_rb_lt.checks = 0;
            g_rb_cc.trust = g_rb_cc.trust > 0 ? 0 : g_rb_cc.trust; g_rb_cc.checks = 0;
            g_rb_sc.trust = g_rb_sc.trust > 0 ? 0 : g_rb_sc.trust; g_rb_sc.checks = 0;
            kv_log("wglMakeCurrent: switched back to context %p -- read-back "
                   "shadows re-verify against the driver.", rc);
        }
        g_last_rc = rc;
    }
    if (r && rc && o_glGetString) {
        static HGLRC said;
        if (rc != said) {
            const GLubyte *v = o_glGetString(GL_VERSION);
            const GLubyte *rn = o_glGetString(GL_RENDERER);
            said = rc;
            kv_log("wglMakeCurrent: context %p now current on thread %lu",
                   rc, GetCurrentThreadId());
            kv_log("  GL_VERSION=%s", v ? (const char *)v : "?");
            kv_log("  GL_RENDERER=%s", rn ? (const char *)rn : "?");
        }
    }
    return r;
}

BOOL WINAPI hk_wglDeleteContext(HGLRC rc) {
    kv_log("wglDeleteContext(%p)", rc);
    if (g_list) { o_glDeleteLists(g_list, 1); g_list = 0; }
    if (g_pl_base) { o_glDeleteLists(g_pl_base, PL_MAXSEG); g_pl_base = 0; }
    g_pl_rec = 0; g_pl_open = 0; g_pl_nseg = 0;
    vr_on_delete_context(rc);
    return o_wglDeleteContext(rc);
}

/* In VR the compositor paces the frame, not the monitor.  The game's own
   vsync pins it to the desktop refresh -- 60 Hz here against a 90 Hz headset,
   which beats against the compositor and judders.  Swallow the game's request
   and keep the interval at 0.  This is not a quality reduction: nothing on the
   monitor needs tearing protection when the monitor is only a mirror. */
static BOOL WINAPI vr_swap_interval(int interval) {
    if (!g_vrcfg.enabled || !g_vrcfg.force_vsync) {
        /* Not our business: pass the game's own request straight through, so
           force_vsync = 0 is a genuine control rather than a half-measure. */
        if (o_wglSwapIntervalEXT) return o_wglSwapIntervalEXT(interval);
        return TRUE;
    }
    if (o_wglSwapIntervalEXT) return o_wglSwapIntervalEXT(0);
    return TRUE;
}

/* The engine binding 0 means "the window" to it and "our target" to us. */
static void APIENTRY hk_glBindFramebufferEng(GLenum target, GLuint fb) {
    if (!o_glBindFramebufferEng) return;
    if (fb == 0 && !g_internal) fb = vr_scene_fbo();
    o_glBindFramebufferEng(target, fb);
}

PROC WINAPI hk_wglGetProcAddress(LPCSTR name) {
    PROC p = o_wglGetProcAddress(name);
    /* Every extension entry point the engine fetches, once.  These bypass
       the pass-through census, and whether any of them is a vertex
       attribute decides whether the glBegin/glEnd capture is complete. */
    if (name) {
        static char seen[160][48];
        static int nseen;
        int i, have = 0;
        for (i = 0; i < nseen; i++)
            if (!lstrcmpA(seen[i], name)) { have = 1; break; }
        if (!have && nseen < 160) {
            lstrcpynA(seen[nseen++], name, 48);
            kv_log("PROCADDRESS: engine fetched %s -> %s", name,
                   p ? "found" : "NULL");
        }
    }
    if (name && p && (!lstrcmpA(name, "glBindFramebuffer") ||
                      !lstrcmpA(name, "glBindFramebufferEXT"))) {
        if (!o_glBindFramebufferEng) {
            o_glBindFramebufferEng = (void (APIENTRY *)(GLenum, GLuint))p;
            kv_log("the engine uses framebuffer objects; its binds of 0 will be "
                   "redirected to the scene target");
        }
        return (PROC)hk_glBindFramebufferEng;
    }
    if (name && p && (!lstrcmpA(name, "glActiveTextureARB") ||
                      !lstrcmpA(name, "glClientActiveTextureARB"))) {
        /* Which unit texenv and glBindTexture refer to is about to change,
           so nothing cached is trustworthy. Wrapped only to drop the
           cache; the engine's own call still runs. */
        sf_active_next = (void (APIENTRY *)(GLenum))p;
        return (PROC)w_glActiveTextureARB;
    }
    if (name && p && !lstrcmpA(name, "glMultiTexCoord2fARB")) {
        ba_mtc2f = (void (APIENTRY *)(GLenum, GLfloat, GLfloat))p;
        return (PROC)w_glMultiTexCoord2fARB;
    }
    if (name && p && !lstrcmpA(name, "glMultiTexCoord2fvARB")) {
        ba_mtc2fv = (void (APIENTRY *)(GLenum, const GLfloat *))p;
        return (PROC)w_glMultiTexCoord2fvARB;
    }
    if (name && p && !lstrcmpA(name, "glMultiTexCoord4fARB")) {
        ba_mtc4f = (void (APIENTRY *)(GLenum, GLfloat, GLfloat, GLfloat,
                                      GLfloat))p;
        return (PROC)w_glMultiTexCoord4fARB;
    }
    if (name && p && !lstrcmpA(name, "glMultiTexCoord4fvARB")) {
        ba_mtc4fv = (void (APIENTRY *)(GLenum, const GLfloat *))p;
        return (PROC)w_glMultiTexCoord4fvARB;
    }
    if (name && p && !lstrcmpA(name, "wglSwapIntervalEXT")) {
        if (!o_wglSwapIntervalEXT) {
            o_wglSwapIntervalEXT = (BOOL (WINAPI *)(int))p;
            kv_log("wglSwapIntervalEXT intercepted; vsync forced off for VR");
        }
        return (PROC)vr_swap_interval;
    }
    return p;
}

/* ---- matrix stack ----------------------------------------------------- */
void APIENTRY hk_glMatrixMode(GLenum mode) {
    STATE_TOUCH();
    if (!g_internal && mode == GL_PROJECTION) {
        if (g_pl_rec) pl_end();
        dup_flush();
    }
    if (!g_internal) {
        g_mode = mode;
        if (mode == GL_PROJECTION) g_s.matmode_proj++;
        else if (mode == GL_MODELVIEW) g_s.matmode_mv++;
    }
    o_glMatrixMode(mode);
}

void APIENTRY hk_glLoadIdentity(void) {
    STATE_TOUCH();
    PL_GUARD();
    if (!g_internal) g_s.loadidentity++;
    if (!g_internal && g_mode == GL_MODELVIEW) mv_loadi();
    if (!g_internal && g_mode == GL_PROJECTION) pj_loadi();
    o_glLoadIdentity();
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}

void APIENTRY hk_glLoadMatrixf(const GLfloat *m) {
    STATE_TOUCH();
    PL_GUARD();
    if (!g_internal && g_mode == GL_MODELVIEW && m) mv_load(m);
    if (!g_internal && g_mode == GL_PROJECTION && m) pj_load(m);
    o_glLoadMatrixf(m);
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}

void APIENTRY hk_glLoadMatrixd(const GLdouble *m) {
    STATE_TOUCH();
    PL_GUARD();
    if (!g_internal && g_mode == GL_MODELVIEW && m) mv_loadd(m);
    if (!g_internal && g_mode == GL_PROJECTION && m) {
        float f[16]; int i; for (i = 0; i < 16; i++) f[i] = (float)m[i]; pj_load(f);
    }
    o_glLoadMatrixd(m);
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}



void APIENTRY hk_glMultMatrixf(const GLfloat *m) {
    STATE_TOUCH();
    GLfloat pitched[16];
    PL_GUARD();
    if (!g_internal) g_s.multmatf++;
    /* Turn the engine's view by the head, so the ground it decides to draw is
       the ground being looked at.  The eye transform cancels this exactly, so
       the picture does not move.
       Without this half, only the cancellation runs -- which REMOVES the head
       rotation from the image and locks the world to the head.  That is what
       happened when the edit that was supposed to add this silently failed to
       match, and it is worth saying plainly because the symptom names the
       missing half. */
    if (!g_internal && g_mode == GL_MODELVIEW && g_dup_mode == DUP_SCENE &&
        g_world_pass && !g_world_view_taken && g_vrcfg.head_cull) {
        float rh[16];
        if (vr_cull_rotation(rh)) {
            int r, cc, k;
            static unsigned tick;
            for (cc = 0; cc < 4; cc++)
                for (r = 0; r < 4; r++) {
                    float sum = 0.0f;
                    for (k = 0; k < 4; k++) sum += rh[k * 4 + r] * m[cc * 4 + k];
                    pitched[cc * 4 + r] = sum;
                }
            m = pitched;
            /* The world view is installed several times a frame (once per eye,
               and more than one world camera), so counting calls said "1800 of
               300 frames".  Count the frames it happened on. */
            if (g_headcull_frame != g_frames) {
                g_headcull_frame = g_frames;
                g_headcull_hits++;
            }
            if ((tick++ % 180) == 0)
                kv_log("HEADCULL: engine view turned to follow the head");
        }
    }
    /* The first modelview multiply after a world projection is the camera's
       view -- the same condition head_cull pitches on, and it is still unset
       here. The object stack replaces it with identity. */
    if (!g_internal && g_mode == GL_MODELVIEW && m) {
        if (g_dup_mode == DUP_SCENE && g_world_pass && !g_world_view_taken)
            mv_view(m);
        else
            mv_mult(m);
    }
    if (!g_internal && g_mode == GL_PROJECTION && m) pj_mult(m);
    o_glMultMatrixf(m);
    /* The first modelview installed after the WORLD projection is that
       camera's view, and the pointer we were handed is its own storage. */
    if (!g_internal && g_mode == GL_MODELVIEW && g_dup_mode == DUP_SCENE &&
        g_world_pass && !g_world_view_taken) {
        float vm[16];
        g_world_view_taken = 1;
        g_internal++;
        if (g_rb_mv.trust == 1) memcpy(vm, g_mv[g_mv_sp], 64);
        else o_glGetFloatv(GL_MODELVIEW_MATRIX, vm);
        g_internal--;
        if (g_vrcfg.diagnostics) {
            dump_view_neighbourhood(m, vm, g_eng_fovy, (float)g_near,
                                    (float)g_far);
            dump_heap_cameras(vm);
        }
    }
    /* The FIRST modelview multiply of a scene pass is the view being installed;
       everything after it carries an object transform as well. */
    if (!g_internal && g_mode == GL_MODELVIEW && g_dup_mode == DUP_SCENE &&
        !g_have_view) {
        if (g_rb_mv.trust == 1) memcpy(g_view_mat, g_mv[g_mv_sp], 64);
        else o_glGetFloatv(GL_MODELVIEW_MATRIX, g_view_mat);
        g_have_view = 1;

        /* Hand the cache THIS frame's view, at the moment it is installed.
           It was being given the previous frame's view, captured at the swap
           -- so every remembered object was reconstructed against a camera
           that had already moved, and drifted across the ground. */

    }
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}

/* This is the engine's perspective camera path: gluPerspective-style, three
   times per frame. */
void APIENTRY hk_glMultMatrixd(const GLdouble *m) {
    STATE_TOUCH();
    PL_GUARD();
    if (!g_internal) {
        g_s.multmatd++;
        /* This pointer is into the ENGINE's own storage for its projection.
           Whatever object owns it very likely owns the field of view that
           built it, which is the value the culling uses. */
        if (g_mode == GL_PROJECTION) {
            fov_note_caller(_ReturnAddress());
            /* The engine rewrites its camera every frame, so a patch applied
               at the end of the previous frame is gone before the culling
               runs.  gluPerspective has just consumed the value, which means
               the engine has finished writing it -- so this is the moment it
               will survive until the world is walked. */
            {
                /* The A/B is over: widening the engine camera provably widens
                   its culling (259.5 draws/frame against 146.3).  Alternating
                   arms now would just show the tester the holes half the
                   time. */
                float want = g_vrcfg.engine_fov;
                if (want <= 0.0f) want = vr_needed_engine_fovy();
                g_last_engine_fov = want;
                if (want > 0.0f) fov_apply(want, 45.0f);
                /* And the horizontal, which rides on the engine's own 16:9
                   aspect. Same camera blocks, different offset. */
                if (g_vrcfg.engine_aspect > 0.0f &&
                    (!g_vrcfg.engine_tanx_ab || g_arm)) {
                    aspect_scan(g_eng_aspect0 > 0.0f ? g_eng_aspect0 : 1.7778f);
                    aspect_apply(g_vrcfg.engine_aspect,
                                 g_eng_aspect0 > 0.0f ? g_eng_aspect0 : 1.7778f);
                }
                if (g_vrcfg.draw_distance > 1.0f && g_far_original > 1.0)
                    far_apply((float)(g_far_original * g_vrcfg.draw_distance),
                              (float)g_far_original);
            }
        }
        if (g_mode == GL_PROJECTION && g_frames == 300 &&
            1) {
            if (g_vrcfg.diagnostics) dump_camera_neighbourhood(m);
            /* Search using the values the engine is demonstrably using right
               now, not constants from a previous session. */
            if (g_eng_fovy > 0.0f && g_eng_aspect > 0.0f)
                fov_scan(g_eng_fovy, g_eng_aspect);
            /* fov_scan above is NOT diagnostic -- it finds the camera
               copies that fov_apply writes to. These two only log: nothing
               reads an orientation candidate, and the layout dump is a
               file. */
            if (g_vrcfg.diagnostics && g_have_view) {
                orientation_scan(g_view_mat);
                camera_layout_dump(g_view_mat, 45.0f, g_eng_aspect,
                                   (float)g_near, (float)g_far);
            }
            /* The far plane is a second, independent limit: widening the
               camera stops the world being cut off at the edges of view, not
               in the distance. */
            /* (the far scan no longer runs from here -- see below) */
        }
    }
    /* Narrow the engine's horizontal culling.  It composes its frustum from
       the matrix it hands GL, so a narrower one makes it submit less -- and
       the picture is unaffected because every scene pass is drawn with OUR
       per-eye projection, not this one.  Only ever narrows, never widens. */
    if (!g_internal && g_mode == GL_PROJECTION && g_vrcfg.engine_tanx > 0.0f &&
        (!g_vrcfg.engine_tanx_ab || g_arm) &&
        m && m[11] == -1.0 && m[0] > 0.0) {
        double want = 1.0 / g_vrcfg.engine_tanx;
        if (want > m[0]) {
            double n[16];
            int k;
            for (k = 0; k < 16; k++) n[k] = m[k];
            n[0] = want;
            o_glMultMatrixd(n);
            if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
            return;
        }
    }
    if (!g_internal && g_mode == GL_MODELVIEW && m) mv_multd(m);
    if (!g_internal && g_mode == GL_PROJECTION && m) pj_multd(m);
    o_glMultMatrixd(m);
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}

void APIENTRY hk_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                           GLdouble n, GLdouble f) {
    STATE_TOUCH();
    PL_GUARD();
    if (!g_internal) g_s.frustum++;
    if (!g_internal && g_mode == GL_PROJECTION) pj_frustum(l, r, b, t, n, f);
    o_glFrustum(l, r, b, t, n, f);
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}

void APIENTRY hk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                         GLdouble n, GLdouble f) {
    STATE_TOUCH();
    PL_GUARD();
    if (!g_internal) {
        g_s.ortho++;
        /* The engine's own 2D canvas, straight from the call that defines it. */
        if (r - l != 0.0 && t - b != 0.0) {
            float w = (float)(r - l), h = (float)(t - b);
            /* The shape of every 2D element hangs off this one ratio, so a
               screen that sets a different canvas is drawn at a different
               aspect -- which is what "too vertical and squished" looks
               like. */
            if (w != g_hud_w || h != g_hud_h)
                kv_log("CANVAS: the engine set a 2D canvas of %.0f x %.0f "
                       "(aspect %.3f), was %.0f x %.0f (aspect %.3f)",
                       w, h, h != 0.0f ? w / h : 0.0f,
                       g_hud_w, g_hud_h,
                       g_hud_h != 0.0f ? g_hud_w / g_hud_h : 0.0f);
            g_hud_w = w;
            g_hud_h = h;
        }
        if (g_trace)
            kv_log("  glOrtho(%.1f,%.1f,%.1f,%.1f,%.2f,%.2f) while matrix mode "
                   "= %s  <-- if this says MODELVIEW, the 2D pass is invisible "
                   "to the projection tracker and is being drawn with the "
                   "SCENE projection", l, r, b, t, n, f,
                   g_mode == GL_PROJECTION ? "PROJECTION" :
                   (g_mode == GL_MODELVIEW ? "MODELVIEW" : "TEXTURE"));
    }
    if (!g_internal && g_mode == GL_PROJECTION) pj_ortho(l, r, b, t, n, f);
    o_glOrtho(l, r, b, t, n, f);
    if (!g_internal && g_mode == GL_PROJECTION) projection_changed();
}

/* ---- viewport --------------------------------------------------------- */
/* While our target is bound, GL_BACK does not exist.  The engine asks for it
   by name and the call would fail, silently, for the whole frame. */
void APIENTRY hk_glDrawBuffer(GLenum buf) {
    o_glDrawBuffer(g_internal ? buf : vr_map_buffer(buf));
}

void APIENTRY hk_glReadBuffer(GLenum buf) {
    o_glReadBuffer(g_internal ? buf : vr_map_buffer(buf));
}

void APIENTRY hk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    STATE_TOUCH();
    if (!g_internal) dup_flush();
    if (!g_internal) {
        g_s.viewport++;
        /* Each distinct one, once.  This was added to test whether the
           engine clips menu boxes by setting a small viewport -- it does not;
           it sets exactly two in a session, both before it draws anything.
           Kept because the canvas size is what the 2D maths is built on, but
           WITHOUT the warning: a flag that fires on the startup pair made a
           correct reading look like a fault every run. */
        if (x != g_eng_vp[0] || y != g_eng_vp[1] ||
            w != g_eng_vp[2] || h != g_eng_vp[3]) {
            kv_log("VIEWPORT: the engine asked for %d,%d %dx%d while drawing "
                   "%s", x, y, w, h,
                   g_dup_mode == DUP_HUD ? "2D" :
                   (g_dup_mode == DUP_SCENE ? "the world" : "nothing yet"));
        }
        g_eng_vp[0] = x; g_eng_vp[1] = y; g_eng_vp[2] = w; g_eng_vp[3] = h;
        g_vp_stale = 0;
    }
    o_glViewport(x, y, w, h);
}

/* The scissor is set in FULL-CANVAS coordinates.  Left alone while each eye
   draws into half the buffer, it clips one eye's content away entirely -- which
   is invisible UI that the game still lets you click, because selection is
   computed on the CPU and knows nothing about where we drew. */
/* A straight passthrough EXCEPT while the 2D pass is being captured, where
   the engine's colour factors are kept but the alpha channel is made to
   accumulate coverage instead. Without that the panel's alpha comes out
   squared and every translucent element composites at the wrong strength. */
void APIENTRY hk_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    STATE_TOUCH();
    if (!g_internal) {
        g_panel_src = sfactor; g_panel_dst = dfactor;
        gk_set(GKS_BSRC, (unsigned)sfactor);
        gk_set(GKS_BDST, (unsigned)dfactor);
    }
    /* Which blend modes does the 2D pass actually use?  The captured
       panel comes back with alpha 0 everywhere except the parchment, so
       the game's dim is darkening the framebuffer by some means that
       writes no coverage -- a multiply (DST_COLOR, ZERO) would do exactly
       that, and a multiply cannot be captured into a transparent layer
       and composited back. Log each distinct pair once. */
    if (!g_internal && g_dup_mode == DUP_HUD) {
        static GLenum seen[12][2];
        static int n;
        int i, have = 0;
        for (i = 0; i < n; i++)
            if (seen[i][0] == sfactor && seen[i][1] == dfactor) { have = 1; break; }
        if (!have && n < 12) {
            seen[n][0] = sfactor; seen[n][1] = dfactor; n++;
            kv_log("BLEND in the 2D pass: src=0x%04X dst=0x%04X  "
                   "(SRC_ALPHA=0x0302 ONE_MINUS_SRC_ALPHA=0x0303 "
                   "DST_COLOR=0x0306 ZERO=0 ONE=1)", sfactor, dfactor);
        }
    }
    if (!g_internal && g_panel_capture && d_blendsep) {
        d_blendsep(sfactor, dfactor, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return;
    }
    o_glBlendFunc(sfactor, dfactor);
}

void APIENTRY hk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    STATE_TOUCH();
    if (!g_internal) dup_flush();
    if (!g_internal) {
        g_eng_sc[0] = x; g_eng_sc[1] = y; g_eng_sc[2] = w; g_eng_sc[3] = h;
        g_have_scissor = 1;
        if (g_menu_up && g_n_scissor < 6) {
            kv_log("scissor while a menu is up: %d,%d %dx%d  (canvas viewport "
                   "%d,%d %dx%d)", x, y, w, h,
                   g_eng_vp[0], g_eng_vp[1], g_eng_vp[2], g_eng_vp[3]);
            g_n_scissor++;
        }
    }
    o_glScissor(x, y, w, h);
}

/* Map the engine's scissor through the SAME transform the content goes
   through.  It is set in framebuffer pixels for the whole canvas; the 2D is
   now placed as a panel with its own scale and centre, so a scissor mapped any
   other way clips the content away -- which is invisible UI the game still
   lets you click, because hit-testing is on the CPU and knows nothing about
   where we drew.  `m` is the panel projection, or NULL for a scene pass. */
/* Four planes in canvas pixels -- the very numbers the engine hands to
   glScissor.  Specified while the engine's own modelview is current, so GL
   transforms them the same way it transforms the vertices, and the clip is
   right at any head angle.  A scissor cannot do this: it is axis aligned in
   screen pixels and the region is a skewed quad. */
#define GL_CLIP_PLANE0_ 0x3000
static void (APIENTRY *o_glClipPlane)(GLenum, const GLdouble *);
static int g_clip_on;
static int g_clip_pushed;
static int g_scissor_owned;
/* the screen as the game will see it, read before we change its DPI view */
static int g_screen_w0, g_screen_h0;

static void eye_clip_planes(void) {
    GLdouble p[4];
    int i;
    if (!o_glClipPlane) {
        o_glClipPlane = (void (APIENTRY *)(GLenum, const GLdouble *))
                        proxy_real_proc("glClipPlane");
        if (!o_glClipPlane) {
            static int said;
            if (!said) { said = 1; kv_log("clip planes: glClipPlane missing"); }
            return;
        }
    }
    /* Only for the 2D, only while the engine is itself clipping, and never
       for the world.  The planes are in CANVAS pixels; against the world's
       modelview those coordinates clip everything, which is what turned the
       load screen black. */
    /* g_scissor_was, not a fresh query: the caller disables the engine's
       scissor test immediately before calling this, so asking GL returns the
       answer we just wrote and the planes would never be enabled. */
    if (g_dup_mode != DUP_HUD || !g_have_scissor || !g_scissor_was ||
        g_eng_sc[2] <= 0 || g_eng_sc[3] <= 0) {
        if (g_clip_on) {
            for (i = 0; i < 4; i++) o_glDisable(GL_CLIP_PLANE0_ + i);
            g_clip_on = 0;
        }
        {
            static int said;
            if (!said && g_dup_mode == DUP_HUD) {
                said = 1;
                kv_log("CLIP: never armed during a 2D pass -- have_scissor=%d "
                       "scissor_was=%d rect %dx%d. The planes cannot clip "
                       "something they were never enabled for.",
                       g_have_scissor, g_scissor_was, g_eng_sc[2], g_eng_sc[3]);
            }
        }
        return;
    }
    /* A plane is transformed by the modelview IN FORCE WHEN IT IS GIVEN, and
       the engine's is not canvas-pixels-to-NDC: measured, its Y scale is
       NEGATIVE and its translation is -0.3, 0.3 rather than -1, -1, so it is
       some element's own transform -- most likely the list's scroll offset.
       Canvas pixel numbers carried through that land nowhere useful.

       So do not depend on whatever happens to be bound: load the canvas
       mapping for the four calls and give back what was there.  Bottom-left
       origin, matching the coordinates glScissor uses. */
    if (g_hud_w > 0.0f && g_hud_h > 0.0f) {
        GLfloat c2n[16];
        int k;
        for (k = 0; k < 16; k++) c2n[k] = (k % 5) ? 0.0f : 1.0f;
        c2n[0]  =  2.0f / g_hud_w;
        c2n[5]  =  2.0f / g_hud_h;
        c2n[12] = -1.0f;
        c2n[13] = -1.0f;
        o_glMatrixMode(GL_MODELVIEW);
        d_pushm();
        o_glLoadMatrixf(c2n);
        g_clip_pushed = 1;
    } else {
        g_clip_pushed = 0;
    }

    /* ax + by + cz + d >= 0 is kept. */
    p[1] = 0.0; p[2] = 0.0;
    p[0] =  1.0; p[3] = -(double)g_eng_sc[0];
    o_glClipPlane(GL_CLIP_PLANE0_ + 0, p);
    p[0] = -1.0; p[3] =  (double)(g_eng_sc[0] + g_eng_sc[2]);
    o_glClipPlane(GL_CLIP_PLANE0_ + 1, p);
    p[0] = 0.0;
    p[1] =  1.0; p[3] = -(double)g_eng_sc[1];
    o_glClipPlane(GL_CLIP_PLANE0_ + 2, p);
    p[1] = -1.0; p[3] =  (double)(g_eng_sc[1] + g_eng_sc[3]);
    o_glClipPlane(GL_CLIP_PLANE0_ + 3, p);
    for (i = 0; i < 4; i++) o_glEnable(GL_CLIP_PLANE0_ + i);
    g_clip_on = 1;
    if (g_clip_pushed) {            /* planes captured; give the matrix back */
        o_glMatrixMode(GL_MODELVIEW);
        d_popm();
        g_clip_pushed = 0;
    }
    {
        /* Three things can make enabled planes clip nothing, and they need
           different answers, so say which. GL_CURRENT_PROGRAM is the one I
           flagged and never checked: clip planes are fixed-function, and a
           shader that does not write gl_ClipVertex ignores them entirely. */
        static int said;
        if (!said) {
            GLint prog = 0, mm = 0;
            GLfloat mv[16];
            said = 1;
            o_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
            o_glGetIntegerv(GL_MATRIX_MODE, &mm);
            o_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
            kv_log("CLIP: planes on for canvas rect %d,%d %dx%d. Shader "
                   "program bound = %d (NON-ZERO means clip planes are "
                   "ignored unless it writes gl_ClipVertex, and this whole "
                   "approach is dead). Modelview diag %.4f %.4f, translate "
                   "%.1f %.1f -- for a canvas ortho of %.0f x %.0f that "
                   "should be about %.5f %.5f and -1 -1.",
                   g_eng_sc[0], g_eng_sc[1], g_eng_sc[2], g_eng_sc[3],
                   (int)prog, mv[0], mv[5], mv[12], mv[13],
                   g_hud_w, g_hud_h,
                   g_hud_w > 0.0f ? 2.0f / g_hud_w : 0.0f,
                   g_hud_h > 0.0f ? 2.0f / g_hud_h : 0.0f);
        }
    }
}

static void eye_clip_planes_off(void) {
    int i;
    if (!g_clip_on || !o_glClipPlane) return;
    for (i = 0; i < 4; i++) o_glDisable(GL_CLIP_PLANE0_ + i);
    g_clip_on = 0;
}

static void eye_scissor(int eye, const float *m) {
    int vx, vy, vw, vh;
    double xn0, xn1, yn0, yn1, ex0, ex1, ey0, ey1;
    int px0, px1, py0, py1;

    /* A clip rectangle can only ever HIDE things.  The engine sets it in
       full-canvas framebuffer coordinates and our 2D goes through a panel
       transform, so a mapping that is even slightly wrong makes UI vanish
       while the game still lets you click it -- which is exactly the missing
       save-game list.  Unless asked otherwise, simply stop clipping: content
       drawn a little outside its box is visible and wrong, which is strictly
       better than correct and invisible. */
    if (g_vrcfg.scissor_mode >= 2) {
        /* Clip planes instead: correct at any head angle, where a screen-space
           rectangle cannot be.  The engine's scissor test is turned off so the
           two do not both clip.

           Capture the engine's state ONCE PER PASS, not once per eye.  This
           runs again for the second eye, and re-reading the flag there reads
           back the value WE wrote for the first -- so it came out 0, the gate
           below refused, and the planes armed for the left eye only.  That is
           the oldest symptom in the book: content in one eye, because a
           once-per-frame flag was consumed on the first. */
        if (o_glIsEnabled && o_glDisable) {
            if (!g_scissor_owned) {
                g_scissor_was = sc_enabled();
                g_scissor_owned = 1;
            }
            if (g_scissor_was) o_glDisable(GL_SCISSOR_TEST_);
        }
        eye_clip_planes();
        return;
    }
    if (!g_vrcfg.scissor_mode) {
        if (o_glIsEnabled && o_glDisable) {
            g_scissor_was = o_glIsEnabled(GL_SCISSOR_TEST_);
            if (g_scissor_was) o_glDisable(GL_SCISSOR_TEST_);
        }
        return;
    }
    /* Whether the engine has the test ON is the whole question for content
       that draws past its box: a rectangle it never applies clips nothing, and
       that needs a different answer from a rectangle mapped wrongly. */
    if (eye == 0 && g_dup_mode == DUP_HUD && o_glIsEnabled) {
        static int said = -1;
        int on = o_glIsEnabled(GL_SCISSOR_TEST_) ? 1 : 0;
        int state = on * 2 + (g_have_scissor ? 1 : 0);
        if (state != said) {
            said = state;
            kv_log("SCISSOR: while drawing 2D the engine has the clip test %s "
                   "and %s set a rectangle. Content drawn with the test off is "
                   "clipped by nothing, whatever we map.",
                   on ? "ON" : "OFF", g_have_scissor ? "has" : "has NOT");
        }
    }
    if (!g_have_scissor || g_eng_vp[2] <= 0 || g_eng_vp[3] <= 0) return;
    vr_eye_viewport(eye, &vx, &vy, &vw, &vh);

    /* framebuffer pixels -> the normalised space the engine draws 2D in */
    xn0 = 2.0 * (g_eng_sc[0] - g_eng_vp[0]) / g_eng_vp[2] - 1.0;
    xn1 = 2.0 * (g_eng_sc[0] + g_eng_sc[2] - g_eng_vp[0]) / g_eng_vp[2] - 1.0;
    yn0 = 2.0 * (g_eng_sc[1] - g_eng_vp[1]) / g_eng_vp[3] - 1.0;
    yn1 = 2.0 * (g_eng_sc[1] + g_eng_sc[3] - g_eng_vp[1]) / g_eng_vp[3] - 1.0;

    if (m) {
        /* Project all four corners.  The old form -- xn*m[0] + m[12] -- is
           only right while m is a scale and an offset; once the panel stands
           in the world the perspective terms matter, they differ between the
           eyes, and they change as the head turns.  That is the save list
           disappearing from one eye on head movement. */
        double cx[4], cy[4];
        int ci;
        cx[0] = xn0; cy[0] = yn0;
        cx[1] = xn1; cy[1] = yn0;
        cx[2] = xn0; cy[2] = yn1;
        cx[3] = xn1; cy[3] = yn1;
        ex0 = ey0 = 1e30; ex1 = ey1 = -1e30;
        for (ci = 0; ci < 4; ci++) {
            double X = m[0] * cx[ci] + m[4] * cy[ci] + m[12];
            double Y = m[1] * cx[ci] + m[5] * cy[ci] + m[13];
            double W = m[3] * cx[ci] + m[7] * cy[ci] + m[15];
            if (W > 1e-6) { X /= W; Y /= W; }
            if (X < ex0) ex0 = X;
            if (X > ex1) ex1 = X;
            if (Y < ey0) ey0 = Y;
            if (Y > ey1) ey1 = Y;
        }
    } else {
        ex0 = xn0; ex1 = xn1; ey0 = yn0; ey1 = yn1;
    }

    px0 = vx + (int)((ex0 + 1.0) * 0.5 * vw);
    px1 = vx + (int)((ex1 + 1.0) * 0.5 * vw);
    py0 = vy + (int)((ey0 + 1.0) * 0.5 * vh);
    py1 = vy + (int)((ey1 + 1.0) * 0.5 * vh);
    if (px1 < px0) { int t = px0; px0 = px1; px1 = t; }
    if (py1 < py0) { int t = py0; py0 = py1; py1 = t; }
    /* Report each distinct rectangle once, with the viewport it has to land
       inside.  A clip rectangle can only hide things, so "it looks fine" from
       a tester is not evidence -- the rectangle either contains the box or it
       does not, and that is checkable here. */
    if (eye == 0) {
        static int last[4];
        if (last[0] != px0 || last[1] != py0 ||
            last[2] != px1 - px0 || last[3] != py1 - py0) {
            last[0] = px0; last[1] = py0;
            last[2] = px1 - px0; last[3] = py1 - py0;
            kv_log("SCISSOR: engine %d,%d %dx%d in its %dx%d canvas -> eye "
                   "%d,%d %dx%d inside a %d,%d %dx%d viewport%s",
                   g_eng_sc[0], g_eng_sc[1], g_eng_sc[2], g_eng_sc[3],
                   g_eng_vp[2], g_eng_vp[3],
                   px0, py0, px1 - px0, py1 - py0, vx, vy, vw, vh,
                   (px1 <= px0 || py1 <= py0)
                       ? "  <<< EMPTY: everything inside it is being clipped "
                         "away, which is invisible-but-clickable UI"
                       : (px0 >= vx + vw || py0 >= vy + vh ||
                          px1 <= vx || py1 <= vy)
                           ? "  <<< OUTSIDE the eye: same result"
                           : "");
        }
    }
    o_glScissor(px0, py0, px1 - px0, py1 - py0);
}

static void restore_scissor(void) {
    eye_clip_planes_off();
    /* Whatever we switched off, switch back on -- in EVERY mode.  This restore
       used to live inside the mode-0 branch, so mode 2 disabled the engine's
       scissor test on every per-eye pass and never gave it back.  The clipping
       that still worked was the engine re-enabling it for itself between our
       passes, which is why enabling the clip planes appeared to REMOVE
       clipping: it did not, it just stopped that accident from mattering. */
    if (g_scissor_was && o_glEnable) o_glEnable(GL_SCISSOR_TEST_);
    g_scissor_was = 0;
    g_scissor_owned = 0;         /* the pass is over; read it fresh next time */
    /* Mode 1 maps the rectangle per eye, so the engine's own rectangle has to
       be put back for whatever it draws outside our per-eye passes. */
    if (g_vrcfg.scissor_mode == 1 && g_have_scissor)
        o_glScissor(g_eng_sc[0], g_eng_sc[1], g_eng_sc[2], g_eng_sc[3]);
}

/* ---- draws ------------------------------------------------------------ */

void APIENTRY hk_glDrawElements(GLenum m, GLsizei c, GLenum t, const GLvoid *i) {
    note_first_draw();
    note_draw_mode();
    int eye, n;
    LARGE_INTEGER t0, t1;
    if (g_mc_cur && !g_internal) mc_taint();
    if (!g_internal) { g_s.drawelements++; g_elems_total++; g_pass_draws++; }
    if (!g_internal && g_dup_mode == DUP_SCENE && g_vrcfg.group_census)
        gc_note_draw(0xFFFFFFFEu);        /* not a list; its own bucket */
    /* A vertex-array draw can be a menu's backdrop just as easily as an
       immediate-mode quad, and this path has never been looked at. Coverage
       is unknown here, so only the colour test can fire -- which can fail to
       dim but cannot dim the wrong thing. */
    if (!g_internal && g_dup_mode == DUP_HUD &&
        (g_vrcfg.menu_dim_wide_search || g_vrcfg.dim_trace)) {
        g_dim_cover = 0.0f;
        g_dim_path = "glDrawElements";
        note_backdrop_colour();
    }
    if (!dup_active()) { o_glDrawElements(m, c, t, i); return; }
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&t0);
    g_n_elems++;
    n = pass_eye_count();
    (void)eye;
    DUP_LOOP(n, o_glDrawElements(m, c, t, i));
    if (g_vrcfg.dup_profile) {
        QueryPerformanceCounter(&t1);
        g_t_elem += t1.QuadPart - t0.QuadPart;
    }
    if (g_dup_mode == DUP_SCENE) g_s.dup_scene++; else g_s.dup_hud++;
}

void APIENTRY hk_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    if (g_mc_cur && !g_internal) mc_taint();
    int eye, n;
    if (!g_internal) { g_s.drawarrays++; g_pass_draws++; }
    if (!g_internal && g_dup_mode == DUP_HUD &&
        (g_vrcfg.menu_dim_wide_search || g_vrcfg.dim_trace)) {
        g_dim_cover = 0.0f;
        g_dim_path = "glDrawArrays";
        note_backdrop_colour();
    }
    if (!dup_active()) { o_glDrawArrays(m, f, c); return; }
    n = pass_eye_count();
    (void)eye;
    DUP_LOOP(n, o_glDrawArrays(m, f, c));
    if (g_dup_mode == DUP_SCENE) g_s.dup_scene++; else g_s.dup_hud++;
}

/* Immediate mode: record the block, then replay it once per eye.  In
   GL_COMPILE mode nothing is drawn until glCallList, so the engine's own
   glBegin/glEnd pair produces no output of its own. */
/* Vertex hooks, present for one reason: the bounding box of a 2D primitive.
   Everything else about them is a straight pass-through.  The box is only
   accumulated during the engine's own 2D pass, so the world costs one
   predictable branch per vertex and nothing else. */
#define VB_NOTE(X, Y)                                                        \
    do {                                                                     \
        if (g_bc_in && !g_internal) g_bc_cur++;                              \
        if (!g_internal &&                                                   \
            (g_dup_mode == DUP_HUD || g_listcov_building)) {                  \
            float _x = (float)(X), _y = (float)(Y);                          \
            if (!g_vb_have) {                                                \
                g_vb_minx = g_vb_maxx = _x;                                  \
                g_vb_miny = g_vb_maxy = _y;                                  \
                g_vb_have = 1;                                               \
            } else {                                                         \
                if (_x < g_vb_minx) g_vb_minx = _x;                          \
                if (_x > g_vb_maxx) g_vb_maxx = _x;                          \
                if (_y < g_vb_miny) g_vb_miny = _y;                          \
                if (_y > g_vb_maxy) g_vb_maxy = _y;                          \
            }                                                                \
        }                                                                    \
    } while (0)

/* Coverage of each display list the game compiles, so a backdrop drawn as a
   list is measured like one drawn immediately.  Open addressed, fixed size,
   no allocation; a miss means "not known", which beats a wrong answer. */
#define LISTCOV_N 2048
/* The list's geometry in its OWN space. What it covers on screen depends on
   the modelview in force when it is CALLED -- that is what a display list is
   for -- so a coverage figure computed at compile time answers a question
   nobody has asked yet. Keep the box; transform it at the call. */
static struct { GLuint id; float minx, maxx, miny, maxy; } g_listcov[LISTCOV_N];
static GLuint g_listcov_building;      /* list id currently being compiled */

static int listcov_get(GLuint id) {
    unsigned h = (unsigned)id * 2654435761u;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned s = (h + (unsigned)i) & (LISTCOV_N - 1);
        if (g_listcov[s].id == id) {
            g_vb_minx = g_listcov[s].minx; g_vb_maxx = g_listcov[s].maxx;
            g_vb_miny = g_listcov[s].miny; g_vb_maxy = g_listcov[s].maxy;
            g_vb_have = 1;
            return 1;
        }
        if (g_listcov[s].id == 0) return 0;
    }
    return 0;
}

static void listcov_put(GLuint id) {
    unsigned h = (unsigned)id * 2654435761u;
    int i;
    if (!id || !g_vb_have) return;
    for (i = 0; i < 8; i++) {
        unsigned s = (h + (unsigned)i) & (LISTCOV_N - 1);
        if (g_listcov[s].id == id || g_listcov[s].id == 0) {
            g_listcov[s].id = id;
            g_listcov[s].minx = g_vb_minx; g_listcov[s].maxx = g_vb_maxx;
            g_listcov[s].miny = g_vb_miny; g_listcov[s].maxy = g_vb_maxy;
            return;
        }
    }
}

/* The box the vertices just described, as a fraction of the visible screen.

   Transformed through the modelview first.  The engine puts its ortho in the
   MODELVIEW and leaves the projection at identity, and every 2D element adds
   its own transform on top, so raw glVertex coordinates are in that element's
   local units and mean nothing on their own -- measured against the canvas
   they made a full-screen quad read as 0%.  After the modelview, with an
   identity projection, the coordinates ARE normalised device coordinates: the
   screen is -1..1, so a full-screen quad spans 2. */
static float box_coverage(void) {
    GLfloat m[16];
    float xs[4], ys[4];
    float minx, maxx, miny, maxy, cw, ch;
    int i;
    if (!g_vb_have) return 0.0f;

    g_internal++;
    if (g_rb_mv.trust == 1) memcpy(m, g_mv[g_mv_sp], 64);
    else o_glGetFloatv(GL_MODELVIEW_MATRIX, m);
    g_internal--;

    /* Four corners, because a rotated or skewed element is not axis-aligned
       once transformed and taking two would understate it. */
    xs[0] = g_vb_minx; ys[0] = g_vb_miny;
    xs[1] = g_vb_maxx; ys[1] = g_vb_miny;
    xs[2] = g_vb_maxx; ys[2] = g_vb_maxy;
    xs[3] = g_vb_minx; ys[3] = g_vb_maxy;
    minx = miny = 1e30f;
    maxx = maxy = -1e30f;
    for (i = 0; i < 4; i++) {
        float x = m[0] * xs[i] + m[4] * ys[i] + m[12];
        float y = m[1] * xs[i] + m[5] * ys[i] + m[13];
        float w = m[3] * xs[i] + m[7] * ys[i] + m[15];
        if (w != 0.0f && w != 1.0f) { x /= w; y /= w; }
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    cw = (maxx - minx) * 0.5f;
    ch = (maxy - miny) * 0.5f;
    if (cw > 1.0f) cw = 1.0f;
    if (ch > 1.0f) ch = 1.0f;
    if (cw < 0.0f || ch < 0.0f) return 0.0f;
    return (cw < ch) ? cw : ch;
}

/* DIAGNOSTIC (diagnostics = 3), 24 Sep 2026: which 2D element is the GAME's
   cursor? The game keeps a cursor position of its own, fed by movement, so an
   aimed pointer drifts from it by an offset nothing reports. If the sprite
   the game draws its cursor with can be recognised, the offset can be
   MEASURED every frame instead of squeezed out blind. Record the last few 2D
   elements of each frame with their screen box in normalised coordinates, and
   log them beside where the Windows cursor is. */
typedef struct { float cx, cy, w, h; GLint tex; const char *path; } Cur2D;
#define CUR2D_N 8
static Cur2D    g_c2d[CUR2D_N], g_c2d_last[CUR2D_N];
static unsigned g_c2d_n, g_c2d_last_n;
static void cur2d_note(const char *path) {
    GLfloat m[16];
    float xs[4], ys[4], minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    int i;
    Cur2D *c;
    if (g_vrcfg.diagnostics != 3 || !g_vb_have) return;
    g_internal++;
    if (g_rb_mv.trust == 1) memcpy(m, g_mv[g_mv_sp], 64);
    else o_glGetFloatv(GL_MODELVIEW_MATRIX, m);
    c = &g_c2d[g_c2d_n % CUR2D_N];
    c->tex = 0;
    o_glGetIntegerv(0x8069, &c->tex);          /* GL_TEXTURE_BINDING_2D */
    g_internal--;
    xs[0] = g_vb_minx; ys[0] = g_vb_miny; xs[1] = g_vb_maxx; ys[1] = g_vb_miny;
    xs[2] = g_vb_maxx; ys[2] = g_vb_maxy; xs[3] = g_vb_minx; ys[3] = g_vb_maxy;
    for (i = 0; i < 4; i++) {
        float x = m[0] * xs[i] + m[4] * ys[i] + m[12];
        float y = m[1] * xs[i] + m[5] * ys[i] + m[13];
        float w = m[3] * xs[i] + m[7] * ys[i] + m[15];
        if (w != 0.0f && w != 1.0f) { x /= w; y /= w; }
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (y < miny) miny = y; if (y > maxy) maxy = y;
    }
    c->cx = (minx + maxx) * 0.5f; c->cy = (miny + maxy) * 0.5f;
    c->w = maxx - minx; c->h = maxy - miny;
    c->path = path;
    g_c2d_n++;
}
/* At the end of the frame: keep this frame's list, start the next. */
static void cur2d_frame_end(HDC hdc) {
    unsigned i, k;
    if (g_vrcfg.diagnostics != 3) return;
    g_c2d_last_n = g_c2d_n;
    memcpy(g_c2d_last, g_c2d, sizeof(g_c2d));
    g_c2d_n = 0;
    if ((g_frames % 30) == 0 && g_c2d_last_n) {
        HWND w = WindowFromDC(hdc);
        POINT p; RECT r;
        float nx = -9.0f, ny = -9.0f;
        char line[900]; int len;
        if (w && GetCursorPos(&p) && ScreenToClient(w, &p) && GetClientRect(w, &r) &&
            r.right > 0 && r.bottom > 0) {
            nx = (float)p.x / (float)r.right * 2.0f - 1.0f;
            ny = 1.0f - (float)p.y / (float)r.bottom * 2.0f;
        }
        len = sprintf(line, "CURSOR2D f%u: OS cursor at (%+.3f,%+.3f) | last 2D:",
                      g_frames, nx, ny);
        k = g_c2d_last_n < CUR2D_N ? g_c2d_last_n : CUR2D_N;
        for (i = 0; i < k && len < 820; i++) {
            const Cur2D *c = &g_c2d_last[(g_c2d_last_n - k + i) % CUR2D_N];
            len += sprintf(line + len, " [%s tex%d c(%+.3f,%+.3f) %.3fx%.3f]",
                           c->path, (int)c->tex, c->cx, c->cy, c->w, c->h);
        }
        kv_log("%s", line);
    }
}

/* ---- the game's cursor sprite, for the grip mouse (pointer_mode 3) -------
   Found at the desk on 24 Sep (diagnostics = 3): the game draws its cursor as
   the LAST 2D element of the frame, a 64x64 textured quad at the Windows
   cursor, hotspot at its top-left. Its texture names are LEARNED here rather
   than written in: each frame, if the last 2D element is cursor-sized and
   sits on the point where the pointer put the cursor, its texture is a
   cursor texture (the arrow, and the hand shown over things). At most four.

   Then, at the moment a cursor texture starts drawing in the 2D pass, the
   pointer's verdict is applied: hidden (colour writes off) in controller
   play, or moved to where the controller ray crosses the panel. Undone
   straight after the draw. The game's draws in this pass go straight to the
   driver, so state set before glBegin is the state the sprite draws with. */
static GLuint   g_curtex[4];
static int      g_ncurtex;
static struct { float cx, cy, w, h, tx, ty; GLuint tex; int have; } g_lastel, g_lastel_frame;
/* Per cursor texture: the sprite's origin (its modelview translation, in
   canvas pixels) minus where OUR cursor was, measured while the cursor
   stands still. The arrow and the hand may differ. */
static float    g_cur_offx[4], g_cur_offy[4];
static int      g_cur_off_ok[4];
static int      g_cur_active;              /* 1 hide, 2 shift, on this draw */
static GLboolean g_cur_mask[4];
static void (APIENTRY *p_colormask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void (APIENTRY *p_getbool)(GLenum, GLboolean *);
int kv_canvas_size(float *w, float *h) {
    if (g_hud_w <= 0.0f || g_hud_h <= 0.0f) return 0;
    *w = g_hud_w; *h = g_hud_h;
    return 1;
}
static int is_curtex(GLuint t) {
    int i;
    for (i = 0; i < g_ncurtex; i++) if (g_curtex[i] == t) return 1;
    return 0;
}
/* THE HUD'S BOXES, for the grip mouse. In the world the grip mouse aims
   through the engine's camera, which is ~3.8x the panel's angle, so pointing
   anywhere on the panel keeps the cursor in the middle quarter of the canvas
   and the HUD's icons -- the fist that punches and demolishes -- are out of
   reach. So: every 2D element outside the middle of the canvas is kept as a
   box, and while the ray crosses the panel over one, the pointer uses the
   panel's own mapping, as in a menu.
     - The middle is left out: the game's hover label sits at the cursor, and
       counting it would hold the cursor wherever it went.
     - The list is not refreshed while the pointer is on a box, for the same
       reason: a hover label drawn beside the fist must not become a box.
     - Cursor sprites, and anything over half the canvas (fades, dims), are
       never boxes. */
#define HUDBOX_N 256
static struct { float x0, y0, x1, y1; } g_hb[2][HUDBOX_N];
static int g_nhb[2], g_hb_w, g_hb_frozen;
static volatile int g_hb_pub = -1;
static void hudbox_note(float x0, float y0, float x1, float y1) {
    float cw = g_hud_w, ch = g_hud_h, nx0, nx1, ny0, ny1;
    int n = g_nhb[g_hb_w];
    if (g_dup_mode != DUP_HUD || is_curtex(g_tex0) || n >= HUDBOX_N ||
        cw <= 0.0f || ch <= 0.0f) return;
    if (x1 - x0 > cw * 0.5f || y1 - y0 > ch * 0.5f || x1 - x0 < 4.0f || y1 - y0 < 4.0f) return;
    nx0 = x0 / cw * 2.0f - 1.0f; nx1 = x1 / cw * 2.0f - 1.0f;
    ny0 = y0 / ch * 2.0f - 1.0f; ny1 = y1 / ch * 2.0f - 1.0f;
    if (nx1 > -0.3f && nx0 < 0.3f && ny1 > -0.3f && ny0 < 0.3f) return;  /* the middle */
    g_hb[g_hb_w][n].x0 = x0; g_hb[g_hb_w][n].y0 = y0;
    g_hb[g_hb_w][n].x1 = x1; g_hb[g_hb_w][n].y1 = y1;
    g_nhb[g_hb_w] = n + 1;
}
/* THE FRONT END (title and its submenus) has no HUD. In a level the game
   always draws its resource bar at the top-left and its icons at the
   top-right; the title screen draws nothing in either corner. Neither the
   2D draw count (the main menu draws as many as the HUD) nor the camera (both
   have a near-10 and a near-70 pass) can tell them apart, and the grip mouse
   needs to: at the front end everything is a menu, pointed at on the panel.
   Debounced, so one frame without the HUD (a load, a fade) changes nothing. */
static int g_no_hud_frames = 1000;
int kv_front_end(void) { return g_no_hud_frames > 30; }
static void front_end_frame(void) {
    float cw = g_hud_w, ch = g_hud_h;
    int i, n = g_nhb[g_hb_w], corner = 0, was = kv_front_end();
    if (g_vrcfg.pointer_mode != 3 || cw <= 0.0f || ch <= 0.0f) return;
    for (i = 0; i < n && !corner; i++)
        if (g_hb[g_hb_w][i].y0 > ch * 0.9f &&
            (g_hb[g_hb_w][i].x1 < cw * 0.25f || g_hb[g_hb_w][i].x0 > cw * 0.75f)) corner = 1;
    g_no_hud_frames = corner ? 0 : (g_no_hud_frames < 1000 ? g_no_hud_frames + 1 : 1000);
    if (kv_front_end() != was)
        kv_log("POINTER: %s", kv_front_end() ?
               "the front end (no HUD in the top corners) -- all menu" :
               "in a level (the HUD is up)");
}
static void hudbox_frame_end(void) {
    int frozen = g_hb_frozen;
    front_end_frame();                    /* before the lists swap */
    g_hb_frozen = 0;                      /* the pointer re-freezes it each frame */
    if (!frozen) { g_hb_pub = g_hb_w; g_hb_w ^= 1; }
    g_nhb[g_hb_w] = 0;
    if (g_vrcfg.diagnostics == 5) {
        /* the desk check: a menu opened in a level, once it has settled */
        static int menu_frames, menu_dumped;
        if (g_menu_up && !kv_front_end()) {
            if (++menu_frames == 90 && menu_dumped < 3) {
                char nm[40];
                sprintf(nm, "keflings_menu%d.bmp", ++menu_dumped);
                mgg_dump_screen(nm);
                kv_log("MENU: desk check -- dumped %s (2D pass %u draws)", nm, g_hud_last);
            }
        } else menu_frames = 0;
    }
    if (g_vrcfg.diagnostics && g_hb_pub >= 0) {
        static unsigned tick;
        static int dumped;
        if (g_vrcfg.diagnostics == 5 && !dumped && g_nhb[g_hb_pub] > 40 && ++dumped) {
            /* the desk check: the frame, and every box on it, to overlay */
            int i, p = g_hb_pub;
            mgg_dump_screen("keflings_hud.bmp");
            kv_log("HUDBOX: dumped keflings_hud.bmp with %d boxes", g_nhb[p]);
            for (i = 0; i < g_nhb[p]; i++)
                kv_log("HUDBOX: all %.0f %.0f %.0f %.0f", g_hb[p][i].x0, g_hb[p][i].y0,
                       g_hb[p][i].x1, g_hb[p][i].y1);
        }
        if ((++tick % 900) == 0) {
            int i, p = g_hb_pub;
            kv_log("HUDBOX: %d boxes outside the middle of the canvas%s", g_nhb[p],
                   frozen ? " (frozen: the pointer is on one)" : "");
            for (i = 0; i < g_nhb[p] && i < 24; i++)
                kv_log("HUDBOX:   %4.0f,%4.0f - %4.0f,%4.0f", g_hb[p][i].x0, g_hb[p][i].y0,
                       g_hb[p][i].x1, g_hb[p][i].y1);
        }
    }
}
/* Canvas pixels, y up. 1 if a HUD element is under (x, y); while it is, the
   list stays as it was, so the element cannot be replaced by its own hover
   label. The freeze lasts one frame; the pointer renews it. */
int kv_hud_hit(float x, float y, int freeze, float pad) {
    int i, p = g_hb_pub, hit = 0;
    if (p >= 0)
        for (i = 0; i < g_nhb[p]; i++)
            if (x >= g_hb[p][i].x0 - pad && x <= g_hb[p][i].x1 + pad &&
                y >= g_hb[p][i].y0 - pad && y <= g_hb[p][i].y1 + pad) { hit = 1; break; }
    g_hb_frozen = hit && freeze;
    return hit;
}
/* Every 2D element's box in canvas pixels, keeping only the last. */
static void lastel_note(void) {
    GLfloat m[16];
    float xs[4], ys[4], minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    int i;
    if (g_vrcfg.pointer_mode != 3 || g_internal || !g_vb_have || g_rb_mv.trust != 1 ||
        g_embed3d) return;
    memcpy(m, g_mv[g_mv_sp], 64);
    xs[0] = g_vb_minx; ys[0] = g_vb_miny; xs[1] = g_vb_maxx; ys[1] = g_vb_miny;
    xs[2] = g_vb_maxx; ys[2] = g_vb_maxy; xs[3] = g_vb_minx; ys[3] = g_vb_maxy;
    for (i = 0; i < 4; i++) {
        float x = m[0] * xs[i] + m[4] * ys[i] + m[12];
        float y = m[1] * xs[i] + m[5] * ys[i] + m[13];
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (y < miny) miny = y; if (y > maxy) maxy = y;
    }
    g_lastel.cx = (minx + maxx) * 0.5f; g_lastel.cy = (miny + maxy) * 0.5f;
    g_lastel.w = maxx - minx; g_lastel.h = maxy - miny;
    g_lastel.tx = m[12]; g_lastel.ty = m[13];
    g_lastel.tex = g_tex0; g_lastel.have = 1;
    hudbox_note(minx, miny, maxx, maxy);
}
/* At the end of the frame: was the last element the cursor? */
int kv_cursor_desk_phase(void);
static void curtex_learn(void) {
    float x, y;
    const float slack = 48.0f;
    if (g_vrcfg.pointer_mode != 3) return;
    {   /* diagnostics = 5: three CONSECUTIVE frames -- as drawn, shifted,
           hidden -- so a diff of them shows the cursor and nothing else.
           This runs at the swap, before the present: the back buffer holds
           the frame just drawn, with the mode set at the end of the one
           before. */
        extern int g_cur_desk_mode;
        static int step, wait;
        if (kv_cursor_desk_phase() >= 0 && g_ncurtex > 0 && kv_in_level() && step < 4) {
            static const char *nm[3] = { "keflings_cur_normal.bmp",
                "keflings_cur_shifted.bmp", "keflings_cur_hidden.bmp" };
            static const int mode_for[3] = { 0, 2, 1 };
            if (step == 0) {
                if (++wait > 300) { g_cur_desk_mode = mode_for[0]; step = 1; }
            } else {
                mgg_dump_screen(nm[step - 1]);
                kv_log("CURSOR: desk check -- dumped %s", nm[step - 1]);
                if (step < 3) g_cur_desk_mode = mode_for[step];
                else g_cur_desk_mode = 0;
                step++;
            }
        }
    }
    g_lastel_frame = g_lastel;
    g_lastel.have = 0;
    {   /* the hotspot, per cursor texture, from a cursor that stood still */
        static float px = -1e9f, py = -1e9f;
        float x, y;
        int i;
        if (g_lastel_frame.have && kv_cursor_desk_phase() <= 0 && kv_cursor_canvas(&x, &y)) {
            for (i = 0; i < g_ncurtex; i++) {
                float ox, oy;
                if (g_curtex[i] != g_lastel_frame.tex || x != px || y != py) continue;
                ox = g_lastel_frame.tx - x; oy = g_lastel_frame.ty - y;
                if (ox < -100.0f || ox > 100.0f || oy < -100.0f || oy > 100.0f) continue;
                if (!g_cur_off_ok[i])
                    kv_log("CURSOR: texture %u is drawn from %+.0f,%+.0f of the "
                           "cursor; the arrow is placed from where the game draws "
                           "it from now on", g_curtex[i], ox, oy);
                g_cur_offx[i] = ox; g_cur_offy[i] = oy; g_cur_off_ok[i] = 1;
            }
            px = x; py = y;
        }
    }
    if (!g_lastel_frame.have || g_ncurtex >= 4 || g_lastel_frame.tex == 0xFFFFFFFFu) return;
    if (is_curtex(g_lastel_frame.tex)) return;
    if (g_lastel_frame.w < 40.0f || g_lastel_frame.w > 100.0f ||
        g_lastel_frame.h < 40.0f || g_lastel_frame.h > 100.0f) return;
    if (!kv_cursor_canvas(&x, &y)) return;
    if (g_lastel_frame.cx < x - slack || g_lastel_frame.cx > x + slack ||
        g_lastel_frame.cy < y - slack || g_lastel_frame.cy > y + slack) return;
    g_curtex[g_ncurtex++] = g_lastel_frame.tex;
    kv_log("CURSOR: the game's cursor sprite is texture %u (%.0fx%.0f at %.0f,%.0f; "
           "our cursor at %.0f,%.0f) -- %d known", g_lastel_frame.tex, g_lastel_frame.w,
           g_lastel_frame.h, g_lastel_frame.cx, g_lastel_frame.cy, x, y, g_ncurtex);
}
/* Before the game draws a 2D element: if it is the cursor, apply the verdict. */
static void cursor_draw_begin(void) {
    float dx, dy;
    int mode;
    g_cur_active = 0;
    if (g_vrcfg.pointer_mode != 3 || g_internal || g_dup_mode != DUP_HUD || g_embed3d) return;
    if (kv_photo_mode() || !is_curtex(g_tex0)) return;
    mode = kv_cursor_draw(&dx, &dy);
    if (mode != 1 && g_rb_mv.trust == 1) {
        /* Place the tip at the target from where the game is drawing the
           sprite THIS frame, not from where we last moved the cursor: the
           game can be a frame behind, and that frame is a jump (25 Sep). */
        float tx, ty;
        int i;
        for (i = 0; i < g_ncurtex; i++)
            if (g_curtex[i] == g_tex0 && g_cur_off_ok[i] && kv_cursor_target(&tx, &ty)) {
                const GLfloat *mv = g_mv[g_mv_sp];
                dx = tx - (mv[12] - g_cur_offx[i]);
                dy = ty - (mv[13] - g_cur_offy[i]);
                mode = 2;
                break;
            }
    }
    if (mode == 1) {
        if (!p_colormask) p_colormask = (void *)proxy_real_proc("glColorMask");
        if (!p_getbool) p_getbool = (void *)proxy_real_proc("glGetBooleanv");
        if (!p_colormask || !p_getbool) return;
        g_internal++;
        p_getbool(GL_COLOR_WRITEMASK, g_cur_mask);
        p_colormask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        g_internal--;
        g_cur_active = 1;
    } else if (mode == 2) {
        /* The game keeps its 2D ortho in the MODELVIEW (projection identity),
           so the shift goes in clip space: projection = T * P, T a translate
           in NDC.  Works whatever either stack holds. */
        GLfloat pj[16];
        float cw, ch;
        if (!kv_canvas_size(&cw, &ch) || cw < 1.0f || ch < 1.0f) return;
        g_internal++;
        o_glGetFloatv(GL_PROJECTION_MATRIX, pj);
        if (g_mode != GL_PROJECTION) o_glMatrixMode(GL_PROJECTION);
        o_glPushMatrix();
        o_glLoadIdentity();
        o_glTranslatef(dx * 2.0f / cw, dy * 2.0f / ch, 0.0f);
        o_glMultMatrixf(pj);
        if (g_mode != GL_PROJECTION) o_glMatrixMode(g_mode);
        g_internal--;
        g_cur_active = 2;
    }
}
static void cursor_draw_end(void) {
    if (!g_cur_active) return;
    g_internal++;
    if (g_cur_active == 1 && p_colormask) {
        p_colormask(g_cur_mask[0], g_cur_mask[1], g_cur_mask[2], g_cur_mask[3]);
    } else if (g_cur_active == 2) {
        if (g_mode != GL_PROJECTION) o_glMatrixMode(GL_PROJECTION);
        o_glPopMatrix();
        if (g_mode != GL_PROJECTION) o_glMatrixMode(g_mode);
    }
    g_internal--;
    g_cur_active = 0;
}

/* glRect draws a filled quad in a single call.  Its corners are the
   arguments, so coverage is exact here without any vertex tracking -- and the
   colour is read after the real call, where the query is legal. */
static void rect_seen(double x1, double y1, double x2, double y2) {
    if (g_internal) return;
    if (g_dup_mode != DUP_HUD) return;
    /* Same local space as the vertices, so measure it the same way. */
    g_vb_minx = (float)(x1 < x2 ? x1 : x2);
    g_vb_maxx = (float)(x1 < x2 ? x2 : x1);
    g_vb_miny = (float)(y1 < y2 ? y1 : y2);
    g_vb_maxy = (float)(y1 < y2 ? y2 : y1);
    g_vb_have = 1;
    g_dim_cover = box_coverage();
    g_dim_path = "glRect";
    note_backdrop_colour();
    cur2d_note("rect");
    lastel_note();
}

void APIENTRY hk_glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) {
    if (g_mc_cur && !g_internal) mc_taint();
    if (!g_internal) { g_s.begin++; g_pass_draws++; g_draws_total++; }
    o_glRectf(x1, y1, x2, y2);
    rect_seen(x1, y1, x2, y2);
}
void APIENTRY hk_glRecti(GLint x1, GLint y1, GLint x2, GLint y2) {
    if (g_mc_cur && !g_internal) mc_taint();
    if (!g_internal) { g_s.begin++; g_pass_draws++; g_draws_total++; }
    o_glRecti(x1, y1, x2, y2);
    rect_seen(x1, y1, x2, y2);
}
void APIENTRY hk_glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) {
    if (g_mc_cur && !g_internal) mc_taint();
    if (!g_internal) { g_s.begin++; g_pass_draws++; g_draws_total++; }
    o_glRectd(x1, y1, x2, y2);
    rect_seen(x1, y1, x2, y2);
}

void APIENTRY hk_glVertex2f(GLfloat x, GLfloat y) {
    if (g_mc_cur && !g_internal) mc_taint();
    VB_NOTE(x, y);
    if (g_ba_on) { ba_push(x, y, 0.0f); return; }
    o_glVertex2f(x, y);
}
void APIENTRY hk_glVertex2i(GLint x, GLint y) {
    if (g_mc_cur && !g_internal) mc_taint();
    VB_NOTE(x, y);
    if (g_ba_on) { ba_push((float)x, (float)y, 0.0f); return; }
    o_glVertex2i(x, y);
}
void APIENTRY hk_glVertex2d(GLdouble x, GLdouble y) {
    if (g_mc_cur && !g_internal) mc_taint();
    VB_NOTE(x, y);
    if (g_ba_on) { ba_push((float)x, (float)y, 0.0f); return; }
    o_glVertex2d(x, y);
}
void APIENTRY hk_glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (g_mc_cur && !g_internal) mc_taint();
    VB_NOTE(x, y);
    if (g_ba_on) { ba_push(x, y, z); return; }
    o_glVertex3f(x, y, z);
}
void APIENTRY hk_glVertex3i(GLint x, GLint y, GLint z) {
    if (g_mc_cur && !g_internal) mc_taint();
    VB_NOTE(x, y);
    if (g_ba_on) { ba_push((float)x, (float)y, (float)z); return; }
    o_glVertex3i(x, y, z);
}
void APIENTRY hk_glVertex3d(GLdouble x, GLdouble y, GLdouble z) {
    if (g_mc_cur && !g_internal) mc_taint();
    VB_NOTE(x, y);
    if (g_ba_on) { ba_push((float)x, (float)y, (float)z); return; }
    o_glVertex3d(x, y, z);
}
void APIENTRY hk_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
    STATE_TOUCH();
    if (g_engine_list && g_mc_cur) g_mc_cur->sets_c = 1;
    if (!g_internal && !g_engine_list) {
        g_cc[0] = r / 255.0f; g_cc[1] = g / 255.0f;
        g_cc[2] = b / 255.0f; g_cc[3] = a / 255.0f; g_cc_valid = 1;
    }
    if (g_ba_on) {
        g_ba_cur.col[0] = r; g_ba_cur.col[1] = g;
        g_ba_cur.col[2] = b; g_ba_cur.col[3] = a;
        g_ba_set_col = 1; g_ba_col_is_ub = 1;
        return;
    }
    o_glColor4ub(r, g, b, a);
}
void APIENTRY hk_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    STATE_TOUCH();
    if (g_engine_list && g_mc_cur) g_mc_cur->sets_c = 1;
    if (!g_internal && !g_engine_list) {
        g_cc[0] = r; g_cc[1] = g; g_cc[2] = b; g_cc[3] = a; g_cc_valid = 1;
    }
    if (g_ba_on) {
        g_ba_cur.col[0] = ba_f2ub(r); g_ba_cur.col[1] = ba_f2ub(g);
        g_ba_cur.col[2] = ba_f2ub(b); g_ba_cur.col[3] = ba_f2ub(a);
        g_ba_col4f[0] = r; g_ba_col4f[1] = g; g_ba_col4f[2] = b; g_ba_col4f[3] = a;
        g_ba_set_col = 1; g_ba_col_is_ub = 0;
        return;
    }
    o_glColor4f(r, g, b, a);
}
void APIENTRY hk_glColor4fv(const GLfloat *v) {
    STATE_TOUCH();
    if (g_engine_list && g_mc_cur) g_mc_cur->sets_c = 1;
    if (!g_internal && !g_engine_list && v && !g_ba_on) {
        g_cc[0] = v[0]; g_cc[1] = v[1]; g_cc[2] = v[2]; g_cc[3] = v[3]; g_cc_valid = 1;
    }
    if (g_ba_on) { hk_glColor4f(v[0], v[1], v[2], v[3]); return; }
    o_glColor4fv(v);
}
void APIENTRY hk_glNormal3f(GLfloat x, GLfloat y, GLfloat z) {
    STATE_TOUCH();
    if (g_ba_on) {
        g_ba_cur.nrm[0] = x; g_ba_cur.nrm[1] = y; g_ba_cur.nrm[2] = z;
        g_ba_set_nrm = 1;
        return;
    }
    o_glNormal3f(x, y, z);
}
void APIENTRY hk_glTexCoord2f(GLfloat s, GLfloat t) {
    STATE_TOUCH();
    if (g_ba_on) {
        g_ba_cur.tc[0][0] = s; g_ba_cur.tc[0][1] = t;
        g_ba_set_tc[0] = 1;
        return;
    }
    o_glTexCoord2f(s, t);
}

static void cursor_draw_begin(void);
static void cursor_draw_end(void);
void APIENTRY hk_glBegin(GLenum mode) {
    /* the game's cursor sprite: hidden or moved for the grip mouse. Before
       anything else, so it applies to the capture list's replay too. */
    cursor_draw_begin();
    if (g_mc_cur && !g_internal) mc_block_begin(mode);
    if (g_vrcfg.block_census && !g_internal) {
        int i, n = g_real_n < 512 ? g_real_n : 512;
        for (i = 0; i < n; i++) g_bc_snap[i] = g_real_count[i];
        if (mode < 16) g_bc_modes[mode]++;
        g_bc_in = 1;
        g_bc_cur = 0;
        g_bc_blocks++;
    }
    /* While a display list is being compiled the box spans the WHOLE list,
       not its last primitive -- otherwise a backdrop built from several
       pieces measures as only the final one and reads as too small. */
    if (!g_listcov_building) g_vb_have = 0;
    note_first_draw();
    note_draw_mode();
    if (!g_internal) { g_s.begin++; g_pass_draws++; g_draws_total++; }
    if (g_vrcfg.block_arrays && !g_internal && dup_active() &&
        ba_procs_ready()) {
        ba_begin(mode);
        return;                 /* nothing reaches the driver until glEnd */
    }
    if (dup_active() && o_glNewList) {
        if (!g_list) { g_list = o_glGenLists(1); g_s.lists++; }
        if (g_list) {
            o_glNewList(g_list, GL_COMPILE);
            g_capturing = 1;
        }
    }
    o_glBegin(mode);
}

void APIENTRY hk_glEnd(void) {
    LARGE_INTEGER t0, t1;
    if (g_ba_on) ba_end();      /* the block never reached the driver */
    else o_glEnd();
    if (g_mc_cur && !g_internal) mc_block_end();
    if (g_bc_in && !g_internal) {
        int i, n = g_real_n < 512 ? g_real_n : 512;
        for (i = 0; i < n; i++)
            g_bc_inside[i] += g_real_count[i] - g_bc_snap[i];
        g_bc_in = 0;
        g_bc_verts += g_bc_cur;
        if (g_bc_cur > g_bc_maxverts) g_bc_maxverts = g_bc_cur;
    }
    /* AFTER the real glEnd, not before it.  glGetFloatv is illegal between
       glBegin and glEnd -- GL raises GL_INVALID_OPERATION and writes nothing
       -- so asking for GL_CURRENT_COLOR here used to read an uninitialised
       stack buffer and judge the draw on whatever happened to be in it.
       That is why this detector has been erratic since it was written, and
       why it only ever worked on the glCallList path, which is outside any
       begin/end block. The colour survives glEnd, so reading it now is both
       legal and correct.

       At glEnd rather than glBegin for the original reason: a quad drawn as
       begin / colour / vertices / end still carries the PREVIOUS primitive's
       colour when the begin runs. */
    if (g_capturing) {
        int eye, n;
        if (g_vrcfg.dup_profile) QueryPerformanceCounter(&t0);
        g_capturing = 0;
        g_n_lists++;
        if (g_dup_mode == DUP_SCENE && g_vrcfg.group_census)
            gc_note_draw(0xFFFFFFFDu);    /* immediate-mode block, not a list */
        o_glEndList();
        n = pass_eye_count();
        (void)eye;
        DUP_LOOP(n, o_glCallList(g_list));
        if (g_vrcfg.dup_profile) {
            QueryPerformanceCounter(&t1);
            g_t_list += t1.QuadPart - t0.QuadPart;
        }
        if (g_dup_mode == DUP_SCENE) g_s.dup_scene++; else g_s.dup_hud++;
    }
    /* Last of all.  Not merely outside the begin/end block -- where the query
       would be rejected outright -- but outside the capture list too: while a
       list is being COMPILED, glColor is recorded rather than executed, so
       the current colour would still be the previous primitive's. After the
       list has been ended and replayed it is this primitive's. */
    if (!g_internal && g_dup_mode == DUP_HUD) {
        /* How much of the engine's own canvas did this primitive span?  A
           backdrop covers it; an icon does not. */
        g_dim_cover = box_coverage();
        g_dim_path = "glEnd";
        note_backdrop_colour();
        cur2d_note("end");
        lastel_note();
    }
    cursor_draw_end();
}

/* The engine may compile display lists of its own, typically at load time.
   Track that so the duplication stays out of the way, and duplicate at the
   point the list is CALLED, which is where drawing actually happens. */
void APIENTRY hk_glNewList(GLuint list, GLenum mode) {
    STATE_TOUCH();
    if (!g_internal && g_vrcfg.diagnostics) list_begin_note();
    if (!g_internal) {
        g_listcov_building = list;
        g_vb_have = 0;
        mc_list_begin(list);
    }
    /* Lists do not nest.  Close our segment, let the engine compile its
       own, and reopen after its glEndList. */
    if (!g_internal && g_pl_rec && g_pl_open) {
        pl_close_segment();
        g_pl_reopen = 1;
    }
    if (!g_internal) g_engine_list = 1;
    o_glNewList(list, mode);
}

void APIENTRY hk_glEndList(void) {
    STATE_TOUCH();
    if (!g_internal && g_vrcfg.diagnostics) list_end_note();
    if (!g_internal && g_listcov_building) {
        listcov_put(g_listcov_building);
        g_listcov_building = 0;
    }
    if (!g_internal) mc_list_end();
    o_glEndList();
    if (!g_internal) g_engine_list = 0;
    if (!g_internal && g_pl_rec && g_pl_reopen) {
        g_pl_reopen = 0;
        pl_open_segment();
    }
}


/* What executing a captured list leaves behind: the current colour, normal
   and texcoord of its last vertex. Replayed wherever the port stands in for
   the list (the no-op skip, a merged member) so the engine's next draw
   inherits exactly what it would have. */
static void list_leave_last(GLuint list) {
    Mesh *me = mc_find(list, 0);
    if (me && me->nvert) {
        const BaVert *lv = &me->v[me->nvert - 1];
        if (me->has_n) o_glNormal3f(lv->nrm[0], lv->nrm[1], lv->nrm[2]);
        if (me->has_t) o_glTexCoord2f(lv->tc[0][0], lv->tc[0][1]);
        if (me->has_c) {
            o_glColor4ub(lv->col[0], lv->col[1], lv->col[2], lv->col[3]);
            g_cc[0] = lv->col[0] / 255.0f; g_cc[1] = lv->col[1] / 255.0f;
            g_cc[2] = lv->col[2] / 255.0f; g_cc[3] = lv->col[3] / 255.0f;
            g_cc_valid = 1;
        }
    }
}
void APIENTRY hk_glCallList(GLuint list) {
    LARGE_INTEGER t0, t1;
    if (g_mc_cur && !g_internal) mc_taint();     /* a nested list call */
    note_first_draw();
    note_draw_mode();
    int eye, n;
    if (!g_internal && g_dup_mode == DUP_SCENE) {
        /* Is this list call contiguous with the last one?  g_real_calls has
           not moved if nothing at all happened in between. */
        if (g_run_len && g_real_calls != g_run_mark) run_close();
        g_run_len++; g_run_lists++;
        g_run_mark = g_real_calls;
        if (g_vrcfg.diagnostics) gap_note();
        if (g_vrcfg.group_census) gc_note_draw((unsigned)list * 2246822519u + 1u);
        if (g_vrcfg.mesh_capture) mc_note_call(list);
        if (g_vrcfg.mv_shadow) { mvc_note(list); if (g_frames > 600) mv_verify(); }
    }
    if (!g_internal && g_dup_mode == DUP_HUD) {
        /* The list's own box, transformed by the modelview in force NOW --
           which is what decides how much of the screen it covers. Unknown
           reads as zero, which can only fail to dim, never dim wrongly. */
        g_vb_have = 0;
        g_dim_cover = listcov_get(list) ? box_coverage() : 0.0f;
        g_dim_path = "glCallList";
        note_backdrop_colour();
        cur2d_note("list");
        lastel_note();
    }
    if (!g_internal) g_lists_total++;
    if (g_vrcfg.skip_noop && !g_internal && !g_engine_list &&
        g_dup_mode == DUP_SCENE) g_nop_frame_world++;
    if (g_vrcfg.skip_noop && !g_internal && !g_engine_list &&
        g_dup_mode == DUP_SCENE && nop_member(list)) {
        /* Nothing reaches the screen -- but executing the list would have
           left the current colour, normal and texcoord at its last vertex,
           and a later draw may inherit them. Leave exactly the same. */
        list_leave_last(list);
        return;                        /* provably changes nothing */
    }
    /* After this call the current colour is unknown -- unless the list is
       one we captured whole, with no colour array and no glColor recorded
       into it: then executing it cannot change the colour. (Throwing it away
       after EVERY list left the fade check blind: 24 Sep 2026.) */
    if (!g_internal && !g_engine_list) {
        Mesh *mz = mc_find(list, 0);
        if (!mz || !mz->ok || mz->has_c || mz->sets_c) g_cc_valid = 0;
    }
    if (g_vrcfg.merge_group && !g_internal && !g_engine_list &&
        g_dup_mode == DUP_SCENE && mg_member(list)) {
        /* Drawn as part of the merged buffer -- so the list itself never
           ran, and neither did its side effects; the merged draw's colour
           array even leaves the current colour undefined. The next draw
           the engine makes inherits them: in test save 6 (24 Sep 2026) the
           far hills came out brighter and yellower whenever merging was on,
           4.9% of the screen against a 0.08% control. Leave what the list
           would have left. */
        list_leave_last(list);
        return;
    }
    if (!dup_active()) { o_glCallList(list); return; }
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&t0);
    g_n_cl++;
    n = pass_eye_count();
    (void)eye;
    DUP_LOOP(n, o_glCallList(list));
    if (g_vrcfg.dup_profile) {
        QueryPerformanceCounter(&t1);
        g_t_cl += t1.QuadPart - t0.QuadPart;
    }
    if (g_dup_mode == DUP_SCENE) g_s.dup_scene++; else g_s.dup_hud++;
}

void APIENTRY hk_glCallLists(GLsizei cnt, GLenum type, const GLvoid *lists) {
    if (g_mc_cur && !g_internal) mc_taint();
    LARGE_INTEGER t0, t1;
    int eye, n;
    if (!g_internal) g_lists_total++;
    if (!g_internal && !g_engine_list) g_cc_valid = 0;   /* any of them may set it */
    if (!dup_active()) { o_glCallLists(cnt, type, lists); return; }
    if (g_vrcfg.dup_profile) QueryPerformanceCounter(&t0);
    g_n_cls++;
    n = pass_eye_count();
    (void)eye;
    DUP_LOOP(n, o_glCallLists(cnt, type, lists));
    if (g_vrcfg.dup_profile) {
        QueryPerformanceCounter(&t1);
        g_t_cls += t1.QuadPart - t0.QuadPart;
    }
    if (g_dup_mode == DUP_SCENE) g_s.dup_scene++; else g_s.dup_hud++;
}

/* The engine reads the projection matrix back about 170 times a frame, which
   is what an engine does when it decides object visibility by projecting each
   one into screen space itself.  The camera structures have been ruled out by
   bisection, and this is the other way culling can be driven -- so if it is
   reading the projection to cull with, handing it a WIDER one makes it keep
   objects it would otherwise drop.

   Only the readback is changed.  What is actually drawn still uses the real
   projection, so this cannot alter the picture; it can only alter what the
   engine chooses to submit. */
/* Engine read-backs that reach the driver, timed. See tools/get_timing.py. */
static void get_note(GLenum pname, LONGLONG t) {
    int i;
    g_get_ticks += t; g_get_n++;
    if (t > g_get_max) g_get_max = t;
    for (i = 0; i < 8; i++) {
        if (g_get_pc[i] && g_get_pn[i] == pname) { g_get_pc[i]++; g_get_pt[i] += t; return; }
        if (!g_get_pc[i]) { g_get_pn[i] = pname; g_get_pc[i] = 1; g_get_pt[i] = t; return; }
    }
}
#define GET_TIMED(pname, CALL) do { LARGE_INTEGER _g0, _g1;                       QueryPerformanceCounter(&_g0); CALL; QueryPerformanceCounter(&_g1);          get_note((pname), _g1.QuadPart - _g0.QuadPart); } while (0)

void APIENTRY hk_glGetFloatv(GLenum pname, GLfloat *params) {
    if (!g_internal) {
        if (pname == GL_PROJECTION_MATRIX) g_n_getproj++;
        else if (pname == GL_MODELVIEW_MATRIX) g_n_getmv++;
        /* While a pass is being recorded eye 0's projection is loaded for
           the whole pass.  The engine must keep seeing its own, exactly as
           the per-draw path restores it between draws. */
        if (pname == GL_PROJECTION_MATRIX && params &&
            (g_pl_rec || g_dup_loaded >= 0)) {
            memcpy(params, g_eng_proj, sizeof(float) * 16);
            return;
        }
        if (params && pname == GL_MODELVIEW_MATRIX && g_rb_mv.trust == 1) {
            memcpy(params, g_mv[g_mv_sp], 64); return;
        }
        if (params && pname == GL_PROJECTION_MATRIX && g_rb_pj.trust == 1) {
            memcpy(params, g_pj[g_pj_sp], 64); return;
        }
        GET_TIMED(pname, o_glGetFloatv(pname, params));
        if (params && pname == GL_MODELVIEW_MATRIX && g_rb_mv.trust == 0)
            rb_check(&g_rb_mv, g_mv[g_mv_sp], params, 16);
        if (params && pname == GL_PROJECTION_MATRIX && g_rb_pj.trust == 0)
            rb_check(&g_rb_pj, g_pj[g_pj_sp], params, 16);
        return;
    }
    o_glGetFloatv(pname, params);
}

/* Only counted, not altered.  Reporting a viewport larger than the real one
 * was tried -- the theory being that the engine skips ground tiles outside its
 * screen rectangle -- and it changed nothing except the frame rate, which
 * collapsed.  The engine culls against its modelview; see head_cull. */
void APIENTRY hk_glGetIntegerv(GLenum pname, GLint *params) {
    if (!g_internal && params && pname == GL_VIEWPORT &&
        (g_pl_rec || g_dup_loaded >= 0)) {
        params[0] = g_eng_vp[0]; params[1] = g_eng_vp[1];
        params[2] = g_eng_vp[2]; params[3] = g_eng_vp[3];
        g_n_getviewport++;
        return;
    }
    if (!g_internal && params && pname == 0x0C10 /* GL_SCISSOR_BOX */ &&
        g_have_scissor && (g_pl_rec || g_dup_loaded >= 0)) {
        params[0] = g_eng_sc[0]; params[1] = g_eng_sc[1];
        params[2] = g_eng_sc[2]; params[3] = g_eng_sc[3];
        return;
    }
    if (!g_internal && params && rb_is_constant(pname)) {
        unsigned i;
        for (i = 0; i < g_rb_nconst; i++)
            if (g_rb_const[i].p == pname) {
                params[0] = g_rb_const[i].v; g_rb_const_hits++; return;
            }
        GET_TIMED(pname, o_glGetIntegerv(pname, params));
        if (g_rb_nconst < 16) {
            g_rb_const[g_rb_nconst].p = pname; g_rb_const[g_rb_nconst].v = params[0];
            g_rb_nconst++;
        }
        return;
    }
    if (!g_internal && params && pname == GL_VIEWPORT && g_rb_vp.trust == 1 &&
        g_vp_stale && g_dup_loaded < 0 && !g_pl_rec) {
        /* the first read after a pop: ask the driver, and believe it */
        GET_TIMED(pname, o_glGetIntegerv(pname, params));
        g_eng_vp[0] = params[0]; g_eng_vp[1] = params[1];
        g_eng_vp[2] = params[2]; g_eng_vp[3] = params[3];
        g_vp_stale = 0;
        g_n_getviewport++;
        return;
    }
    if (!g_internal && params && pname == GL_VIEWPORT && g_rb_vp.trust == 1) {
        params[0] = g_eng_vp[0]; params[1] = g_eng_vp[1];
        params[2] = g_eng_vp[2]; params[3] = g_eng_vp[3];
        g_n_getviewport++;
        return;
    }
    if (!g_internal) GET_TIMED(pname, o_glGetIntegerv(pname, params));
    else o_glGetIntegerv(pname, params);
    if (!g_internal && params && pname == GL_VIEWPORT && g_rb_vp.trust == 0 &&
        !g_pl_rec && g_dup_loaded < 0) {
        float sh[4], re[4];
        int i;
        for (i = 0; i < 4; i++) { sh[i] = (float)g_eng_vp[i]; re[i] = (float)params[i]; }
        rb_check(&g_rb_vp, sh, re, 4);
    }
    if (g_internal || !params) return;
    if (pname != GL_VIEWPORT) return;
    g_n_getviewport++;
}

/* ---- the last three read-backs (tools/no_readbacks2.py) ---------------- */
static DWORD    g_err_lastcheck;
/* Who asks, and what they get: up to 8 callers by return address, with the
   error codes each one has seen and the frame of the first non-zero one. */
static void ge_note(void *ra, GLenum e) {
    int i;
    for (i = 0; i < 8; i++) {
        if (g_ge[i].ra == ra || !g_ge[i].ra) {
            g_ge[i].ra = ra; g_ge[i].n++;
            if (e != GL_NO_ERROR) {
                if (!g_ge[i].errs) { g_ge[i].first = e; g_ge[i].first_frame = g_frames; }
                g_ge[i].errs++;
            }
            return;
        }
    }
}
GLenum APIENTRY hk_glGetError(void) {
    GLenum e;
    if (g_internal) return o_glGetError();
    if (g_rb_err.trust == 1) {
        DWORD now = GetTickCount();
        if (now - g_err_lastcheck < 5000u) return GL_NO_ERROR;
        g_err_lastcheck = now;
        e = o_glGetError();                 /* the periodic real check */
        ge_note(_ReturnAddress(), e);
        if (e != GL_NO_ERROR) {
            g_rb_err.trust = 0; g_rb_err.checks = 0;
            kv_log("READ-BACK SHADOW: glGetError returned 0x%04X on a periodic "
                   "check at frame %u -- back to the driver, verification "
                   "restarts.", e, g_frames);
        }
        return e;
    }
    e = o_glGetError();
    ge_note(_ReturnAddress(), e);
    if (g_rb_err.trust == 0) {
        g_rb_err.checks++;
        if (e != GL_NO_ERROR) {
            /* not condemned: the count restarts, and it takes 2,000 clean
               answers IN A ROW after the last error to be trusted */
            kv_log("READ-BACK SHADOW: glGetError returned 0x%04X at frame %u "
                   "(call %u) -- verification restarts.", e, g_frames,
                   g_rb_err.checks);
            g_rb_err.checks = 0;
        } else if (g_rb_err.checks >= 2000) {
            g_rb_err.trust = 1;
            g_err_lastcheck = GetTickCount();
            kv_log("READ-BACK SHADOW: glGetError was GL_NO_ERROR on 2000 "
                   "calls -- answered here from now on, with a real check "
                   "every 5 s.");
        }
    }
    return e;
}
static int mat_count(GLenum pname) {
    switch (pname) {
        case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_EMISSION:
            return 4;
        case GL_SHININESS: return 1;
        default: return 0;
    }
}
/* The material the state filter last saw for this face and pname, or 0. */
static int mat_cached(GLenum face, GLenum pname, GLfloat *out) {
    int k = SF_KEY(pname), n = mat_count(pname), i;
    if (!n || g_colmat_on || !SF_OK(g_sf_mat[k]) || g_sf_mat[k].pname != pname)
        return 0;
    if (g_sf_mat[k].face != face && g_sf_mat[k].face != GL_FRONT_AND_BACK)
        return 0;
    if (g_sf_mat[k].iv != 0x7FFFFFFF) {           /* set by glMateriali */
        if (n != 1) return 0;
        out[0] = (GLfloat)g_sf_mat[k].iv;
        return 1;
    }
    for (i = 0; i < n; i++) out[i] = g_sf_mat[k].v[i];
    return n;
}
void APIENTRY hk_glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params) {
    GLfloat c[4];
    int n;
    if (g_internal || !params) { o_glGetMaterialfv(face, pname, params); return; }
    n = mat_cached(face, pname, c);
    if (n && g_rb_mat.trust == 1) { memcpy(params, c, n * sizeof(GLfloat)); return; }
    GET_TIMED(pname, o_glGetMaterialfv(face, pname, params));
    if (n && g_rb_mat.trust == 0) rb_check(&g_rb_mat, c, params, n);
}

/* Lights: GL_POSITION and GL_SPOT_DIRECTION are held in EYE space, as GL
   holds them -- transformed by the modelview at the moment they are set. */
typedef struct { GLenum pname; GLfloat v[4]; int n; } LtEnt;
static LtEnt g_lt[8][12];
static int   g_lt_n[8];
static void lt_drop(void) { memset(g_lt_n, 0, sizeof(g_lt_n)); }
static void lt_store(GLenum light, GLenum pname, const GLfloat *p, int n) {
    int li = (int)light - GL_LIGHT0, i;
    GLfloat v[4];
    if (li < 0 || li > 7 || n < 1 || n > 4) return;
    for (i = 0; i < n; i++) v[i] = p[i];
    if (pname == GL_POSITION) {
        const float *m = g_mv[g_mv_sp];
        v[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12] * p[3];
        v[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13] * p[3];
        v[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] * p[3];
        v[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15] * p[3];
    } else if (pname == GL_SPOT_DIRECTION) {
        const float *m = g_mv[g_mv_sp];
        v[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2];
        v[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2];
        v[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2];
    }
    for (i = 0; i < g_lt_n[li]; i++)
        if (g_lt[li][i].pname == pname) break;
    if (i == g_lt_n[li]) { if (i >= 12) return; g_lt_n[li]++; }
    g_lt[li][i].pname = pname; g_lt[li][i].n = n;
    memcpy(g_lt[li][i].v, v, sizeof(v));
}
static int lt_count(GLenum pname) {
    switch (pname) {
        case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION:
            return 4;
        case GL_SPOT_DIRECTION: return 3;
        case GL_SPOT_EXPONENT: case GL_SPOT_CUTOFF: case GL_CONSTANT_ATTENUATION:
        case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION: return 1;
        default: return 0;
    }
}
void APIENTRY hk_glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list && params && g_mode == GL_MODELVIEW)
        lt_store(light, pname, params, lt_count(pname));
    else if (!g_internal) {
        /* set under another matrix mode, or into a list: not modelled */
        int li = (int)light - GL_LIGHT0;
        if (li >= 0 && li < 8) g_lt_n[li] = 0;
    }
    o_glLightfv(light, pname, params);
}
void APIENTRY hk_glLightf(GLenum light, GLenum pname, GLfloat param) {
    STATE_TOUCH();
    if (!g_internal && !g_engine_list && lt_count(pname) == 1)
        lt_store(light, pname, &param, 1);
    o_glLightf(light, pname, param);
}
void APIENTRY hk_glGetLightfv(GLenum light, GLenum pname, GLfloat *params) {
    int li = (int)light - GL_LIGHT0, i, n = lt_count(pname);
    const LtEnt *e = NULL;
    if (g_internal || !params) { o_glGetLightfv(light, pname, params); return; }
    if (li >= 0 && li < 8 && n)
        for (i = 0; i < g_lt_n[li]; i++)
            if (g_lt[li][i].pname == pname && g_lt[li][i].n == n) { e = &g_lt[li][i]; break; }
    if (e && g_rb_lt.trust == 1) { memcpy(params, e->v, n * sizeof(GLfloat)); return; }
    GET_TIMED(pname, o_glGetLightfv(light, pname, params));
    if (e && g_rb_lt.trust == 0) rb_check(&g_rb_lt, e->v, params, n);
}

void APIENTRY hk_glGetDoublev(GLenum pname, GLdouble *params) {
    if (!g_internal) {
        if (pname == GL_PROJECTION_MATRIX) g_n_getproj++;
        else if (pname == GL_MODELVIEW_MATRIX) g_n_getmv++;
        if (pname == GL_PROJECTION_MATRIX && params &&
            (g_pl_rec || g_dup_loaded >= 0)) {
            int k;
            for (k = 0; k < 16; k++) params[k] = (GLdouble)g_eng_proj[k];
            return;
        }
        if (params && pname == GL_MODELVIEW_MATRIX && g_rb_mv.trust == 1) {
            int k; for (k = 0; k < 16; k++) params[k] = (GLdouble)g_mv[g_mv_sp][k];
            return;
        }
        if (params && pname == GL_PROJECTION_MATRIX && g_rb_pj.trust == 1) {
            int k; for (k = 0; k < 16; k++) params[k] = (GLdouble)g_pj[g_pj_sp][k];
            return;
        }
        GET_TIMED(pname, o_glGetDoublev(pname, params));
        return;
    }
    o_glGetDoublev(pname, params);
}

void APIENTRY hk_glDrawPixels(GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                              const GLvoid *px) {
    if (!g_internal) g_n_drawpixels++;
    o_glDrawPixels(w, h, fmt, type, px);
}
void APIENTRY hk_glBitmap(GLsizei w, GLsizei h, GLfloat x0, GLfloat y0,
                          GLfloat xm, GLfloat ym, const GLubyte *bm) {
    if (!g_internal) g_n_bitmap++;
    o_glBitmap(w, h, x0, y0, xm, ym, bm);
}
void APIENTRY hk_glCopyPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum t) {
    if (!g_internal) g_n_copypixels++;
    o_glCopyPixels(x, y, w, h, t);
}
void APIENTRY hk_glCopyTexImage2D(GLenum t, GLint l, GLenum ifmt, GLint x,
                                  GLint y, GLsizei w, GLsizei h, GLint b) {
    if (!g_internal) g_n_copytex++;
    o_glCopyTexImage2D(t, l, ifmt, x, y, w, h, b);
}
void APIENTRY hk_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo,
                                     GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!g_internal) g_n_copytex++;
    o_glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}

void APIENTRY hk_glClear(GLbitfield mask) {
    STATE_TOUCH();
    if (!g_internal) { g_s.clear++; dup_flush(); }
    /* A clear ignores the viewport, so one clear already covers both eyes
       -- but REPLAYED for eye 1 it would wipe eye 0's finished picture.
       Keep it out of the recording. */
    if (!g_internal && g_pl_rec && g_pl_open) {
        pl_close_segment();
        o_glClear(mask);
        pl_open_segment();
        return;
    }
    o_glClear(mask);
}

/* The engine calls this once a frame.  It is a hard stop: the CPU waits
   until the GPU has completed every command issued so far, so nothing
   overlaps and the frame costs the two added together.  See
   skip_glfinish. */
void APIENTRY hk_glFinish(void) {
    if (!g_internal) {
        g_finish_calls++;
        if (g_vrcfg.skip_glfinish) {
            g_finish_skipped++;
            /* WHERE in the frame, for the first few.  Immediately before the
               buffer swap is the 2010 frame-limiter idiom and is safe to
               drop.  Mid-frame is more likely a sync for a readback or a
               pbuffer bind, and that is the case worth a second look. */
            if (g_finish_logged < 3) {
                g_finish_logged++;
                kv_log("glFinish: the engine called it during pass %d (%s), "
                       "%u draws in. SKIPPED -- it stops the CPU until the "
                       "GPU has drained, so the two never overlap. Set "
                       "skip_glfinish = 0 to put it back; F7 toggles it "
                       "live.", g_pass_no,
                       g_dup_mode == DUP_HUD ? "2D" :
                           (g_dup_mode == DUP_SCENE ? "world" : "between "
                            "passes, which is where a frame limiter sits"),
                       g_pass_draws);
            }
            return;
        }
    }
    o_glFinish();
}

void APIENTRY hk_glEnable(GLenum cap) {
    STATE_TOUCH();
    /* The per-eye state switched the engine's scissor test off; if the
       engine turns it on again mid-pass it must find its own state. */
    if (!g_internal && cap == GL_SCISSOR_TEST_) dup_flush();
    if (!g_internal) { int s = gk_cap_slot(cap); if (s >= 0) gk_set(GKS_CAP(s), 1u); }
    if (!g_internal && cap == 0x0B57) g_colmat_on = 1;
    if (!g_internal && !g_engine_list) {
        if (cap == 0x0B50) g_en_light = 1;
        else if (cap == 0x0BC0) g_en_atest = 1;
        else if (cap == 0x0B57) g_en_cmat = 1;
        if (cap == 0x0B50) g_nx.light = 1;
        else if (cap == 0x0BC0) g_nx.atest = 1;
        else if (cap == 0x0B57) g_nx.cmat = 1;
        else if (cap == 0x0DE1) {
            if (g_nx.unit >= 0) g_nx.tex[g_nx.unit] = 1;
            else { int u; for (u = 0; u < NX_UNITS; u++) g_nx.tex[u] = -1; }
        }
    }
    if (!g_internal && cap == GL_SCISSOR_TEST_) g_sc_on = 1;
    o_glEnable(cap);
}
void APIENTRY hk_glDisable(GLenum cap) {
    STATE_TOUCH();
    if (!g_internal && cap == GL_SCISSOR_TEST_) dup_flush();
    if (!g_internal) { int s = gk_cap_slot(cap); if (s >= 0) gk_set(GKS_CAP(s), 0u); }
    if (!g_internal && cap == 0x0B57) g_colmat_on = 0;
    if (!g_internal && !g_engine_list) {
        if (cap == 0x0B50) g_en_light = 0;
        else if (cap == 0x0BC0) g_en_atest = 0;
        else if (cap == 0x0B57) g_en_cmat = 0;
        if (cap == 0x0B50) g_nx.light = 0;
        else if (cap == 0x0BC0) g_nx.atest = 0;
        else if (cap == 0x0B57) g_nx.cmat = 0;
        else if (cap == 0x0DE1) {
            if (g_nx.unit >= 0) g_nx.tex[g_nx.unit] = 0;
            else { int u; for (u = 0; u < NX_UNITS; u++) g_nx.tex[u] = -1; }
        }
    }
    if (!g_internal && cap == GL_SCISSOR_TEST_) g_sc_on = 0;
    o_glDisable(cap);
}
/* A push or pop of the projection with OUR matrix loaded would push ours
   and pop it back later.  A push or pop of attributes carries the viewport
   and the scissor.  Put the engine's own state back first. */
void APIENTRY hk_glPushMatrix(void) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_PROJECTION) { dup_flush(); pj_push(); }
    if (!g_internal && g_mode == GL_MODELVIEW) mv_push();
    o_glPushMatrix();
}
void APIENTRY hk_glPopMatrix(void) {
    STATE_TOUCH();
    if (!g_internal && g_mode == GL_PROJECTION) { dup_flush(); pj_pop(); }
    if (!g_internal && g_mode == GL_MODELVIEW) mv_pop();
    o_glPopMatrix();
}
void APIENTRY hk_glPushAttrib(GLbitfield mask) {
    STATE_TOUCH();
    sf_drop();
    lt_drop();
    if (!g_internal) {
        if (g_nx_sp < 16) {
            g_nx_stack[g_nx_sp].mask = mask; g_nx_stack[g_nx_sp].st = g_nx;
        }
        g_nx_sp++;
    }
    if (!g_internal) g_en_light = g_en_atest = g_en_cmat = -1;
    if (!g_internal) dup_flush();
    o_glPushAttrib(mask);
}
void APIENTRY hk_glPopAttrib(void) {
    STATE_TOUCH();
    sf_drop();
    lt_drop();
    if (!g_internal && g_nx_sp > 0) {
        g_nx_sp--;
        if (g_nx_sp < 16) {
            /* restore exactly what the pushed mask saved, as GL does */
            GLbitfield m = g_nx_stack[g_nx_sp].mask;
            const NxState *o = &g_nx_stack[g_nx_sp].st;
            int u;
            if (m & (GL_ENABLE_BIT | GL_LIGHTING_BIT)) { g_nx.light = o->light; g_nx.cmat = o->cmat; }
            if (m & (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT)) g_nx.atest = o->atest;
            if (m & (GL_ENABLE_BIT | GL_TEXTURE_BIT))
                for (u = 0; u < NX_UNITS; u++) g_nx.tex[u] = o->tex[u];
            if (m & GL_TEXTURE_BIT) {
                for (u = 0; u < NX_UNITS; u++) g_nx.env[u] = o->env[u];
                g_nx.unit = o->unit;
                g_tex0 = 0xFFFFFFFFu;   /* the binding came back too: unknown */
            }
            if (m & GL_LIGHTING_BIT) { g_nx.dif_ok = o->dif_ok; g_nx.dif_a = o->dif_a; }
            if (m & GL_COLOR_BUFFER_BIT) {
                g_nx.af_ok = o->af_ok; g_nx.af_func = o->af_func; g_nx.af_ref = o->af_ref;
            }
        } else {
            memset(&g_nx, 0xFF, sizeof(g_nx));   /* overflowed: all unknown */
            g_nx.dif_ok = 0; g_nx.af_ok = 0;
        }
    }
    if (!g_internal) { g_vp_stale = 1; g_sc_on = -1; g_cc_valid = 0;
                       g_en_light = g_en_atest = g_en_cmat = -1; }
    if (!g_internal) dup_flush();
    o_glPopAttrib();
}

/* ---- init ------------------------------------------------------------- */
#define BIND(n) *(FARPROC *)&o_##n = GetProcAddress(g_realdll, #n)

/* Armadillo's Debug-Blocker can run the game as a child process that the
 * unpacking parent debugs, and unlocking the full game restarts it -- so one
 * launch can produce two pids, both loading this DLL.  A single fixed log path
 * means whichever starts second truncates the first.  One file per pid.
 */
static void open_log(void) {
    char dir[MAX_PATH], path[MAX_PATH], exe[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 64) return;
    lstrcatA(dir, "\\KeflingsVR");
    CreateDirectoryA(dir, NULL);

    exe[0] = 0;
    GetModuleFileNameA(NULL, exe, MAX_PATH);

    /* One per launch, so they pile up: 80 in one day of testing, some 12 MB.
       Keep the newest few -- enough to cover a launch that ran as two
       processes and the launch before it -- and delete the rest. */
    {
        char pat[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        struct { FILETIME t; char name[MAX_PATH]; } keep[8];
        int nkeep = 0, i;
        _snprintf(pat, MAX_PATH, "%s\\probe-*.log", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                /* find the oldest kept; replace it if this one is newer */
                int oldest = -1;
                char victim[MAX_PATH];
                victim[0] = 0;
                if (nkeep < 8) {
                    keep[nkeep].t = fd.ftLastWriteTime;
                    lstrcpynA(keep[nkeep].name, fd.cFileName, MAX_PATH);
                    nkeep++;
                    continue;
                }
                for (i = 0; i < nkeep; i++)
                    if (oldest < 0 || CompareFileTime(&keep[i].t, &keep[oldest].t) < 0)
                        oldest = i;
                if (CompareFileTime(&fd.ftLastWriteTime, &keep[oldest].t) > 0) {
                    lstrcpynA(victim, keep[oldest].name, MAX_PATH);
                    keep[oldest].t = fd.ftLastWriteTime;
                    lstrcpynA(keep[oldest].name, fd.cFileName, MAX_PATH);
                } else {
                    lstrcpynA(victim, fd.cFileName, MAX_PATH);
                }
                if (victim[0]) {
                    _snprintf(pat, MAX_PATH, "%s\\%s", dir, victim);
                    DeleteFileA(pat);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

    _snprintf(path, MAX_PATH, "%s\\probe-%lu.log", dir, GetCurrentProcessId());
    g_log = fopen(path, "w");

    /* And one beside the game itself.  A fixed name, because a tester asked
       for "the log" needs one file rather than one per launch -- and the
       previous run is kept, because a crash followed by a restart would
       otherwise erase the very session being reported. */
    if (exe[0]) {
        char here[MAX_PATH], prev[MAX_PATH], *slash;
        lstrcpynA(here, exe, MAX_PATH);
        slash = strrchr(here, '\\');
        if (slash) {
            *slash = 0;
            _snprintf(prev, MAX_PATH, "%s\\keflings_vr_log.prev.txt", here);
            _snprintf(path, MAX_PATH, "%s\\keflings_vr_log.txt", here);
            DeleteFileA(prev);
            MoveFileA(path, prev);
            g_log2 = fopen(path, "w");
        }
    }
    if (!g_log && !g_log2) return;

    kv_log("A Kingdom for Keflings VR %s  (built %s %s)", KV_VERSION, __DATE__, __TIME__);
    kv_log("host exe   : %s", exe);
    kv_log("command    : %s", GetCommandLineA());
    kv_log("pid        : %lu", GetCurrentProcessId());
}

static BOOL init(void) {
    char sys[MAX_PATH];
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    if (!n) return FALSE;
    lstrcatA(sys, "\\opengl32.dll");
    g_realdll = LoadLibraryA(sys);
    if (!g_realdll) return FALSE;

    proxy_bind_passthrough(g_realdll);
    BIND(glDrawBuffer);        BIND(glReadBuffer);
    BIND(wglSwapBuffers);      BIND(wglSwapLayerBuffers);
    BIND(wglCreateContext);    BIND(wglMakeCurrent);
    BIND(wglDeleteContext);    BIND(wglGetProcAddress);
    BIND(glViewport);          BIND(glScissor);
    BIND(glPushMatrix);        BIND(glPopMatrix);
    BIND(glPushAttrib);        BIND(glPopAttrib);
    BIND(glColor4ub);          BIND(glColor4f);
    BIND(glColor4fv);          BIND(glNormal3f);
    BIND(glTexCoord2f);
    BIND(glDrawPixels);        BIND(glBitmap);
    BIND(glCopyPixels);        BIND(glCopyTexImage2D);
    BIND(glCopyTexSubImage2D);
    BIND(glMatrixMode);
    BIND(glLoadIdentity);      BIND(glLoadMatrixf);
    BIND(glLoadMatrixd);       BIND(glMultMatrixf);
    BIND(glMultMatrixd);       BIND(glFrustum);
    BIND(glOrtho);             BIND(glBegin);
    BIND(glRectf);             BIND(glRecti);
    BIND(glRectd);
    BIND(glVertex2f);          BIND(glVertex2i);
    BIND(glVertex2d);          BIND(glVertex3f);
    BIND(glVertex3i);          BIND(glVertex3d);
    BIND(glEnd);               BIND(glDrawArrays);
    BIND(glDrawElements);      BIND(glClear);
    BIND(glEnable);            BIND(glDisable);
    BIND(glFinish);
    BIND(glTexEnvi);           BIND(glTexEnvf);
    BIND(glGetError);          BIND(glGetMaterialfv);
    BIND(glGetLightfv);        BIND(glLightfv);
    BIND(glLightf);
    BIND(glTranslatef);        BIND(glTranslated);
    BIND(glRotatef);           BIND(glRotated);
    BIND(glScalef);            BIND(glScaled);
    BIND(glArrayElement);      BIND(glVertexPointer);
    BIND(glNormalPointer);     BIND(glTexCoordPointer);
    BIND(glColorPointer);      BIND(glEnableClientState);
    BIND(glDisableClientState);
    BIND(glAlphaFunc);         BIND(glDepthMask);
    BIND(glShadeModel);        BIND(glDepthFunc);
    BIND(glMateriali);         BIND(glMaterialfv);
    *(FARPROC *)&o_glBindTextureR = GetProcAddress(g_realdll, "glBindTexture");
    BIND(glBlendFunc);
    BIND(glIsEnabled);         BIND(glGetDoublev);
    BIND(glGetIntegerv);       BIND(glGetFloatv);
    BIND(glGetString);         BIND(glGenLists);
    BIND(glDeleteLists);       BIND(glNewList);
    BIND(glEndList);           BIND(glCallList);
    BIND(glCallLists);

    open_log();
    kv_log("A Kingdom for Keflings VR loaded. real opengl32 = %s", sys);
    g_report_tick = GetTickCount();
    vr_config_load();
    return TRUE;
}

/* Tell Windows this process speaks in real pixels.
 *
 * The game is DPI-unaware, and this display runs at 150%.  So Windows hands it
 * a 2560x1440 desktop, the game renders into a 2560-wide buffer, and Windows
 * then bitmap-stretches that to the 3840-wide screen.  Everything is softened
 * by that stretch before VR is involved at all -- and the eye textures are
 * copied from that same buffer, so the headset inherits it.
 *
 * Declaring awareness here makes the window and the framebuffer native 4K.  It
 * has to happen before the process creates a window, which is why it is in
 * DllMain: opengl32 is loaded at startup, well before that.
 *
 * Called through GetProcAddress rather than linked, so that running on a
 * Windows without the newer entry points simply falls back to the older one
 * rather than failing to load the DLL at all.
 */
/* Correct the game's own resolution to the monitor, before it reads it.
   Runs at process attach, so WinMain has not started and the game has not
   opened its config yet -- the values take effect on this launch.

   Text in, text out: the file is the player's, and rewriting it through an ini
   API would reorder it, drop the comments and lose the warning NinjaBee put at
   the top. Only the two numbers change. */
/* `len` is the live length of the text and grows when a value needs more
   room; `cap` is what was allocated for it. */
static void ini_set_int(char *text, size_t *len, size_t cap,
                        const char *key, int value, int *changed) {
    char *p = text;
    size_t klen = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        char *line = p, *eq, *digits, *end;
        int cur;
        /* must be at the start of a line, allowing leading spaces */
        while (line > text && line[-1] != '\n') {
            if (line[-1] != ' ' && line[-1] != '\t') break;
            line--;
        }
        if (line != text && line[-1] != '\n') { p += klen; continue; }
        eq = p + klen;
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq != '=') { p += klen; continue; }
        digits = eq + 1;
        while (*digits == ' ' || *digits == '\t') digits++;
        end = digits;
        while (*end >= '0' && *end <= '9') end++;
        if (end == digits) { p += klen; continue; }
        cur = atoi(digits);
        if (cur != value) {
            char buf[16];
            size_t have = (size_t)(end - digits), want;
            _snprintf(buf, sizeof(buf), "%d", value);
            want = strlen(buf);
            /* Same width or shorter, padded: rewriting in place keeps every
               byte of the rest of the file, comments and all. */
            if (want <= have) {
                memcpy(digits, buf, want);
                memset(digits + want, ' ', have - want);
                *changed = 1;
            } else if (*len + (want - have) < cap) {
                /* Longer: shift the rest of the file along.  A three-digit
                   height cannot hold 2160, and 1024x768 is an ordinary thing
                   to find in this file, so this is not a rare path. */
                size_t grow = want - have;
                size_t tail = *len - (size_t)(end - text);
                memmove(end + grow, end, tail + 1);   /* +1 for the NUL */
                memcpy(digits, buf, want);
                *len += grow;
                *changed = 1;
            } else {
                *changed = -1;   /* would not fit; say so rather than corrupt */
            }
        }
        return;
    }
}

/* The same in-place rewrite for a word-valued key.  Only ever shortens or
   pads, so every other byte of the player's file survives. */
static void ini_set_word(char *text, const char *key, const char *value,
                         int *changed) {
    char *p = text;
    size_t klen = strlen(key), vlen = strlen(value);
    while ((p = strstr(p, key)) != NULL) {
        char *line = p, *eq, *val, *end;
        while (line > text && line[-1] != '\n') {
            if (line[-1] != ' ' && line[-1] != '\t') break;
            line--;
        }
        if (line != text && line[-1] != '\n') { p += klen; continue; }
        eq = p + klen;
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq != '=') { p += klen; continue; }
        val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        end = val;
        while (*end && *end != '\r' && *end != '\n' &&
               *end != ' ' && *end != '\t' && *end != ';') end++;
        if (end == val) return;
        if ((size_t)(end - val) >= vlen &&
            _strnicmp(val, value, vlen) != 0) {
            memcpy(val, value, vlen);
            memset(val + vlen, ' ', (size_t)(end - val) - vlen);
            *changed = 1;
        }
        return;
    }
}

static char *g_ini_original;

/* Is there a live (uncommented) "key =" line?  Same line-start rule as the
   setters above. */
static int ini_has_key(const char *text, const char *key) {
    const char *p = text;
    size_t klen = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        const char *line = p, *eq;
        while (line > text && line[-1] != '\n') {
            if (line[-1] != ' ' && line[-1] != '\t') break;
            line--;
        }
        eq = p + klen;
        while (*eq == ' ' || *eq == '\t') eq++;
        if ((line == text || line[-1] == '\n') && *eq == '=') return 1;
        p += klen;
    }
    return 0;
}

/* A FRESH install's settings.ini has the resolution only as commented
   examples (";XScreenRes = 960"), so the setters above found nothing and the
   first launch ran at 960x720 -- on a second test PC, 25 Sep. The game writes its own
   values as live lines straight under [GRAPHICS]; add ours the same way. */
static void ini_add_graphics(char *text, size_t *len, size_t cap,
                             const char *lines, int *changed) {
    char *sec = strstr(text, "[GRAPHICS]");
    size_t add = strlen(lines);
    if (sec) {
        char *at = strchr(sec, '\n');
        size_t tail;
        at = at ? at + 1 : text + *len;
        tail = *len - (size_t)(at - text);
        if (*len + add >= cap) { *changed = -1; return; }
        memmove(at + add, at, tail + 1);
        memcpy(at, lines, add);
        *len += add;
    } else {
        const char *hdr = strstr(text, "\r\n") ? "\r\n[GRAPHICS]\r\n" : "\n[GRAPHICS]\n";
        size_t h = strlen(hdr);
        if (*len + h + add >= cap) { *changed = -1; return; }
        memcpy(text + *len, hdr, h);
        memcpy(text + *len + h, lines, add + 1);
        *len += h + add;
    }
    *changed = 1;
}

static void match_game_canvas(void) {
    char exe[MAX_PATH], path[MAX_PATH], *text, *slash;
    FILE *f;
    long len;
    size_t cap, live;
    int w, h, changed = 0;

    if (!g_vrcfg.match_canvas_to_monitor) return;
    /* From the SAME source the borderless resize uses, so the two cannot
       disagree.  This machine gives three different answers to "how big is
       the screen" -- 3840x2160 from GetSystemMetrics once DPI awareness is
       declared, 2560x1440 from the same call before it, and whatever a given
       thread happens to inherit -- and predicting which one the window will
       end up at was wrong the first two times I tried.  Do not predict: ask
       the monitor, exactly as make_borderless does. */
    {
        POINT origin;
        HMONITOR mon;
        MONITORINFO mi;
        origin.x = 0; origin.y = 0;
        mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoA(mon, &mi)) return;
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    if (w < 640 || h < 480) return;
    kv_log("CANVAS: the monitor is %dx%d; GetSystemMetrics said %dx%d before "
           "DPI awareness was declared. Where those disagree, the monitor is "
           "the one the window will be given.",
           w, h, g_screen_w0, g_screen_h0);

    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return;
    slash = strrchr(exe, '\\');
    if (!slash) return;
    *slash = 0;
    _snprintf(path, MAX_PATH, "%s\\fsall\\settings.ini", exe);

    f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 1 << 20) { fclose(f); return; }
    /* Slack, so a value that needs more digits than the file currently gives
       it can be written rather than refused.  64 bytes is far more than the
       few digits any of these keys can gain. */
    cap = (size_t)len + 256;
    text = (char *)malloc(cap);
    if (!text) { fclose(f); return; }
    if (fread(text, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(text); return;
    }
    fclose(f);
    text[len] = 0;

    /* Keep the file as it arrived.  The backup below is what a player uses
       to undo all of this, and writing it from the buffer we are about to
       edit -- which is what this did -- hands them a copy of our own change
       and calls it their original. */
    {
        char *orig = (char *)malloc((size_t)len);
        if (orig) memcpy(orig, text, (size_t)len);
        g_ini_original = orig;
    }

    live = (size_t)len;
    {
        const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
        char add[160];
        size_t n = 0;
        add[0] = 0;
        if (!ini_has_key(text, "FullScreen"))
            n += _snprintf(add + n, sizeof(add) - n, "FullScreen = no%s", eol);
        if (!ini_has_key(text, "XScreenRes"))
            n += _snprintf(add + n, sizeof(add) - n, "XScreenRes = %d%s", w, eol);
        if (!ini_has_key(text, "YScreenRes"))
            n += _snprintf(add + n, sizeof(add) - n, "YScreenRes = %d%s", h, eol);
        if (n > 0) {
            ini_add_graphics(text, &live, cap, add, &changed);
            kv_log("CANVAS: settings.ini had no live resolution lines (a fresh "
                   "install ships them commented out) -- added under [GRAPHICS]: "
                   "%s", changed > 0 ? "done" : "NO ROOM, left alone");
        }
    }
    ini_set_int(text, &live, cap, "XScreenRes", w, &changed);
    ini_set_int(text, &live, cap, "YScreenRes", h, &changed);
    /* Exclusive full screen takes the display mode and rearranges every other
       window the player had open, and this port does borderless itself. */
    ini_set_word(text, "FullScreen", "no", &changed);

    if (changed > 0) {
        char bak[MAX_PATH];
        FILE *b;
        _snprintf(bak, MAX_PATH, "%s.before-vr", path);
        /* Once only: the first backup is the player's real original. */
        b = fopen(bak, "rb");
        if (b) fclose(b);
        else {
            b = fopen(bak, "wb");
            if (b) {
                fwrite(g_ini_original ? g_ini_original : text,
                       1, (size_t)len, b);
                fclose(b);
            }
        }
        f = fopen(path, "wb");
        if (!f)
            kv_log("CANVAS: could not write %s (error %d) -- the game will "
                   "start at the resolution in that file. Set XScreenRes and "
                   "YScreenRes to %d and %d in its [GRAPHICS] section.",
                   path, errno, w, h);
        if (f) {
            fwrite(text, 1, live, f);
            fclose(f);
            kv_log("CANVAS: corrected the game's own settings.ini -- %dx%d, "
                   "and windowed rather than exclusive full screen. "
                   "The game lays its whole 2D out for those numbers whether "
                   "or not its window is that size, and a mismatch misshapes "
                   "every menu. Your original is saved beside it as "
                   "settings.ini.before-vr.", w, h);
        }
    } else if (changed < 0) {
        kv_log("CANVAS: this monitor is %dx%d but settings.ini has less room "
               "than that needs on one of its lines, so it was left alone. "
               "Set XScreenRes and YScreenRes to %d and %d by hand.",
               w, h, w, h);
    }
    free(text);
    free(g_ini_original);
    g_ini_original = NULL;
}

static void make_dpi_aware(void) {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    typedef BOOL (WINAPI *PFN_CTX)(HANDLE);
    typedef BOOL (WINAPI *PFN_OLD)(void);
    PFN_CTX ctx;
    PFN_OLD old;
    int ok = 0;
    DWORD err = 0;
    if (!u32) return;
    /* PER_MONITOR_AWARE_V2 is (HANDLE)-4 */
    ctx = (PFN_CTX)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (ctx) {
        ok = ctx((HANDLE)(INT_PTR)-4) ? 1 : 0;
        if (!ok) err = GetLastError();
    }
    if (!ok) {
        old = (PFN_OLD)GetProcAddress(u32, "SetProcessDPIAware");
        if (old) ok = old() ? 2 : 0;
        if (!ok && !err) err = GetLastError();
    }
    g_dpi_result = ok;
    g_dpi_err = err;
}

unsigned kv_draw_count(void) { return g_draws_total; }
unsigned kv_elem_count(void) { return g_elems_total; }
unsigned kv_list_count(void) { return g_lists_total; }
/* The world camera has been seen AND the 2D pass is the HUD. */
int kv_in_level(void) {
    /* Judged by the CAMERA, not by counting draws.  The world camera has a
       near plane of 70 and the menu backdrop has 10, which separates them
       exactly.  The draw-count heuristic cannot: this player's in-game HUD
       draws 101 elements and the main menu draws 99, so no threshold exists
       that tells them apart -- and with menu_draws at 70 the port believed a
       menu was permanently open during play. */
    return g_near > 10.5;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res) {
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        InitializeCriticalSection(&g_lock);
        /* BEFORE declaring DPI awareness: this is the screen size the game's
           own window will be given, and the canvas has to match the window,
           not the hardware. */
        g_screen_w0 = GetSystemMetrics(SM_CXSCREEN);
        g_screen_h0 = GetSystemMetrics(SM_CYSCREEN);
        make_dpi_aware();
        if (!init()) return FALSE;
        /* After init (which loads our own config and opens the log) and
           still long before WinMain, so the game has not read its own config
           yet and will pick up the correction on this launch. */
        match_game_canvas();
    } else if (reason == DLL_PROCESS_DETACH) {
        input_emu_shutdown();
        vr_shutdown();
        kv_log("=== shutdown after %u frames ===", g_frames);
        if (g_log) fclose(g_log);
        if (g_log2) fclose(g_log2);
    }
    return TRUE;
}



