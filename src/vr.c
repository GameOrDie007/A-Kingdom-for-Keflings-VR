/* A Kingdom for Keflings VR -- the OpenXR side.
 *
 * Two modes, selected by `stereo` in keflings_vr.ini:
 *
 *   stereo = 0   The frame the game drew is presented on a world-locked quad:
 *                the flat game on a big virtual screen.  This is stage 1, kept
 *                because it is the fallback whenever the stereo path is in
 *                doubt and because it is pleasant to play in its own right.
 *
 *   stereo = 1   The proxy issues every scene draw twice, into the left and
 *                right halves of the game's own back buffer, with a per-eye
 *                asymmetric frustum.  Each half becomes one view of an OpenXR
 *                projection layer.  The kingdom then sits in front of you as a
 *                model you can lean into.
 *
 * Why the projection matrix carries everything: GL applies projection after
 * modelview, and P * (E * MV) == (P * E) * MV.  So the eye offset and the head
 * pose fold into the projection alone, and the engine's modelview -- which is
 * where all of its object placement lives -- is never touched.  A consequence
 * worth keeping: with the head at the tracking origin, E is the identity and
 * the picture is exactly the flat game.  That is a built-in control.
 *
 * All OpenXR calls happen on the thread that owns the GL context, because the
 * session is bound to that context.
 */
#include "vr.h"
#include "proxy.h"

#include <tlhelp32.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The SDK's structs must be laid out naturally.  Nothing here includes an
   engine header, but the guard costs nothing and documents the requirement. */
#pragma pack(push, 8)
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
/* openxr_platform.h's Win32 section declares COM interop entry points that
   take IUnknown*, so this must come first. */
#include <unknwn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#pragma pack(pop)

VrConfig g_vrcfg = { 1, 2.5f, 3.0f, 1, 1, 500.0f, 0.0f, 1.0f, 0, 0, 1, 0, 1, 0, 1, 0, 9, 0, 16.0f, 0, 0, 0, 0.0f, 0, 0.0f, 0, 0, 25.0f, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f, 1, 0.25f, 14.0f, 1, 2, 1, 0, 1, 1, 0, 0, 0.5f, 0.5f, 1, 0, 3, 1, 400.0f, 0.55f, 150.0f, 3.0f, 30.0f, 1, 0, 2.2f, 3.2f, 800.0f, 75.0f, 150, 75.0f, 0.5f, -40.0f, 1.5f, 1.0f, 1, 1, 1, 1.0f, 0 };

#define MAXEYE 2

/* ---- GL bits GL/gl.h (1.1) does not declare ---------------------------- */
#define GL_READ_FRAMEBUFFER   0x8CA8
#define GL_DRAW_FRAMEBUFFER   0x8CA9
#define GL_COLOR_ATTACHMENT0  0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SRGB8_ALPHA8       0x8C43
#define GL_RGBA8              0x8058
#define GL_FRAMEBUFFER_SRGB   0x8DB9
#define GL_FRAMEBUFFER            0x8D40
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_RENDERBUFFER           0x8D41
#define GL_DEPTH24_STENCIL8       0x88F0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_SAMPLE_BUFFERS         0x80A8
#define GL_SAMPLES                0x80A9
#define GL_TIME_ELAPSED           0x88BF
#define GL_QUERY_RESULT           0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867

typedef void (APIENTRY *PFNGENFRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNDELETEFRAMEBUFFERS)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFNBLITFRAMEBUFFER)(GLint, GLint, GLint, GLint,
                                            GLint, GLint, GLint, GLint,
                                            GLbitfield, GLenum);
typedef GLenum (APIENTRY *PFNCHECKFRAMEBUFFERSTATUS)(GLenum);

static PFNGENFRAMEBUFFERS        p_glGenFramebuffers;
static PFNDELETEFRAMEBUFFERS     p_glDeleteFramebuffers;
static PFNBINDFRAMEBUFFER        p_glBindFramebuffer;
static PFNFRAMEBUFFERTEXTURE2D   p_glFramebufferTexture2D;
static PFNBLITFRAMEBUFFER        p_glBlitFramebuffer;
static PFNCHECKFRAMEBUFFERSTATUS p_glCheckFramebufferStatus;
static void      (APIENTRY *p_glReadBuffer)(GLenum);
static HGLRC     (WINAPI *p_wglGetCurrentContext)(void);
static void      (APIENTRY *p_glDisable)(GLenum);
static void      (APIENTRY *p_glEnable)(GLenum);
static GLboolean (APIENTRY *p_glIsEnabled)(GLenum);
/* the desktop mirror's own copy of the left eye */
static void      (APIENTRY *p_glDrawBuffer)(GLenum);
static void      (APIENTRY *p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void      (APIENTRY *p_glGenTextures)(GLsizei, GLuint *);
static void      (APIENTRY *p_glDeleteTextures)(GLsizei, const GLuint *);
static void      (APIENTRY *p_glBindTexture)(GLenum, GLuint);
static void      (APIENTRY *p_glTexImage2D)(GLenum, GLint, GLint, GLsizei,
                                            GLsizei, GLint, GLenum, GLenum,
                                            const void *);
static void      (APIENTRY *p_glTexParameteri)(GLenum, GLenum, GLint);
static void      (APIENTRY *p_glColorMask)(GLboolean, GLboolean, GLboolean,
                                           GLboolean);
static void      (APIENTRY *p_glClearColor)(GLclampf, GLclampf, GLclampf,
                                            GLclampf);
static void      (APIENTRY *p_glClear)(GLbitfield);
static void      (APIENTRY *p_glGetIntegerv)(GLenum, GLint *);
static void      (APIENTRY *p_glReadPixels)(GLint, GLint, GLsizei, GLsizei,
                                            GLenum, GLenum, void *);
static GLenum    (APIENTRY *p_glGetError)(void);
static void      (APIENTRY *p_glGenRenderbuffers)(GLsizei, GLuint *);
static void      (APIENTRY *p_glBindRenderbuffer)(GLenum, GLuint);
static void      (APIENTRY *p_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void      (APIENTRY *p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
/* The scene target: two eyes side by side, sized from the runtime rather than
   from the desktop window. */
static GLuint    g_scene_fbo, g_scene_tex, g_scene_depth;
static int       g_scene_w, g_scene_h;
/* GPU timing.  Two queries, used alternately: one is being filled while the
   other is read, so nothing ever waits on the GPU to answer. */
static void (APIENTRY *p_glGenQueries)(GLsizei, GLuint *);
static void (APIENTRY *p_glBeginQuery)(GLenum, GLuint);
static void (APIENTRY *p_glEndQuery)(GLenum);
static void (APIENTRY *p_glGetQueryObjectuiv)(GLuint, GLenum, GLuint *);
static void (APIENTRY *p_glGetQueryObjectui64v)(GLuint, GLenum, unsigned __int64 *);
static GLuint   g_gpu_q[2];
static int      g_gpu_slot, g_gpu_open, g_gpu_armed;
static double   g_gpu_sum;
static unsigned g_gpu_n;
static GLuint    g_mirror_tex;
static int       g_mirror_w, g_mirror_h;

/* ---- OpenXR state ------------------------------------------------------ */
static HMODULE      g_loader;
static XrInstance   g_instance = XR_NULL_HANDLE;
static XrSystemId   g_system = XR_NULL_SYSTEM_ID;
static XrSession    g_session = XR_NULL_HANDLE;
static XrSpace      g_space = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static int          g_running, g_frame_open, g_failed;
static XrTime       g_predicted;
static HGLRC        g_rc;
static GLuint       g_fbo;

/* stage 1: one swapchain, the whole frame on a quad */
static XrSwapchain  g_quad_sc = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLKHR *g_quad_img;
static uint32_t     g_quad_n;
static int          g_quad_w, g_quad_h;

/* stage 2: one swapchain per eye, each fed from half the back buffer */
static XrSwapchain  g_eye_sc[MAXEYE];
static XrSwapchainImageOpenGLKHR *g_eye_img[MAXEYE];
static uint32_t     g_eye_n[MAXEYE];
static int          g_eyew, g_eyeh;     /* the SWAPCHAIN: always what the
                                           runtime recommended, because it
                                           takes a very slow path for
                                           anything else */
static int          g_scenew, g_sceneh; /* where the engine actually draws.
                                           render_scale scales THIS, and our
                                           blit upscales it into the eye. */
static XrView       g_view[MAXEYE];
static uint32_t     g_nview;
static int          g_views_valid;      /* xrLocateViews succeeded this frame */
static int          g_stereo;           /* stereo path is up and running */
static float        g_eng_tanx = 0.0f;  /* the engine's own half-angle tangents */
static float        g_eng_tany = 0.0f;
static float        g_fov_sx = 1.0f, g_fov_sy = 1.0f;  /* used this frame */
static float        g_mag = 1.0f;       /* uniform magnification applied */
static int          g_cull_checked;     /* the once-per-session self-check */
float               g_head_dev_deg;    /* head rotation off the camera axis */
int                 g_fov_clamped;     /* the engine could not be widened enough */

static XrActionSet  g_actions = XR_NULL_HANDLE;
static XrAction     a_move, a_look, a_select, a_back, a_menu;
static XrAction     a_gripL, a_gripR;
static XrAction     a_btna, a_btnb, a_btnx, a_btny;
static XrAction     a_trigL;
static XrAction     a_stickL, a_stickR;
static XrAction     a_aimR, a_aimL;
static XrSpace      g_aim_space = XR_NULL_HANDLE;
static XrSpace      g_aim_space_l = XR_NULL_HANDLE;   /* the Leftorium points with it */
/* The panel as it was last actually drawn, so the pointer and the thing
   it points at can never disagree. */
static float        g_panel_hw, g_panel_hh, g_panel_dist;
static int          g_panel_ok;
static int create_actions(void);   /* defined below; called above it */
static int          g_actions_ready;
VrInput             g_vrin;

/* Published for the winmm proxy, which presents these as a joystick.  A named
   mapping rather than a shared symbol: the two DLLs are in one process, but a
   mapping cannot go wrong if one of them is not loaded. */
typedef struct {
    volatile LONG magic;
    float move_x, move_y;
    float look_x, look_y;
    int   select, back, menu;
    float grip_l, grip_r;
    int   enabled;
} SharedInput;
#define KVIN_MAGIC 0x4B56494E
static HANDLE       g_inmap;
static SharedInput *g_inshare;

static void publish_input(void) {
    if (!g_inshare) return;
    g_inshare->move_x = g_vrin.move_x;
    g_inshare->move_y = g_vrin.move_y;
    g_inshare->look_x = g_vrin.look_x;
    g_inshare->look_y = g_vrin.look_y;
    g_inshare->select = g_vrin.select;
    g_inshare->back   = g_vrin.back;
    g_inshare->menu   = g_vrin.menu;
    g_inshare->grip_l = g_vrin.grip_l;
    g_inshare->grip_r = g_vrin.grip_r;
    g_inshare->enabled = g_vrcfg.controllers ? 1 : 0;
    g_inshare->magic = KVIN_MAGIC;
}

static void open_shared_input(void) {
    g_inmap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 sizeof(SharedInput), "KeflingsVRInput");
    if (!g_inmap) { kv_log("INPUT: could not create the shared block"); return; }
    g_inshare = (SharedInput *)MapViewOfFile(g_inmap, FILE_MAP_ALL_ACCESS, 0, 0,
                                             sizeof(SharedInput));
    if (g_inshare) memset((void *)g_inshare, 0, sizeof(SharedInput));
}

static int64_t      g_format;

static unsigned g_fail_wait, g_fail_begin, g_fail_acquire,
                g_fail_submit, g_fail_locate;
static unsigned g_vr_frames;

/* ---- loader entry points ---------------------------------------------- */
static PFN_xrGetInstanceProcAddr xrGIPA;
#define XRFN(name) static PFN_##name f_##name;
XRFN(xrCreateInstance) XRFN(xrDestroyInstance) XRFN(xrGetSystem)
XRFN(xrGetSystemProperties) XRFN(xrCreateSession) XRFN(xrDestroySession)
XRFN(xrCreateReferenceSpace) XRFN(xrDestroySpace)
XRFN(xrEnumerateSwapchainFormats) XRFN(xrCreateSwapchain)
XRFN(xrDestroySwapchain) XRFN(xrEnumerateSwapchainImages)
XRFN(xrAcquireSwapchainImage) XRFN(xrWaitSwapchainImage)
XRFN(xrReleaseSwapchainImage) XRFN(xrBeginSession) XRFN(xrEndSession)
XRFN(xrWaitFrame) XRFN(xrBeginFrame) XRFN(xrEndFrame) XRFN(xrPollEvent)
XRFN(xrResultToString) XRFN(xrGetInstanceProperties) XRFN(xrLocateViews)
XRFN(xrEnumerateViewConfigurationViews)
XRFN(xrCreateActionSet) XRFN(xrDestroyActionSet) XRFN(xrCreateAction)
XRFN(xrStringToPath) XRFN(xrSuggestInteractionProfileBindings)
XRFN(xrAttachSessionActionSets) XRFN(xrSyncActions)
XRFN(xrGetActionStateBoolean) XRFN(xrGetActionStateFloat)
XRFN(xrGetActionStateVector2f)
XRFN(xrCreateActionSpace) XRFN(xrLocateSpace)
#undef XRFN

static const char *xr_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (f_xrResultToString && g_instance &&
        f_xrResultToString(g_instance, r, buf) == XR_SUCCESS)
        return buf;
    /* xrResultToString needs an instance, and the failures that matter most
       happen before there is one.  Name the ones that can occur that early. */
    switch (r) {
    case XR_ERROR_API_VERSION_UNSUPPORTED:
        return "XR_ERROR_API_VERSION_UNSUPPORTED (runtime is older than these "
               "headers; request a lower apiVersion)";
    case XR_ERROR_RUNTIME_UNAVAILABLE:
        return "XR_ERROR_RUNTIME_UNAVAILABLE (no runtime, or it is not running)";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
        return "XR_ERROR_FORM_FACTOR_UNAVAILABLE (headset not connected)";
    case XR_ERROR_EXTENSION_NOT_PRESENT:
        return "XR_ERROR_EXTENSION_NOT_PRESENT (runtime lacks XR_KHR_opengl_enable)";
    case XR_ERROR_FILE_ACCESS_ERROR:
        return "XR_ERROR_FILE_ACCESS_ERROR (the loader could not READ a file "
               "it needed -- the runtime's own dll, or an API layer. Seen when "
               "the game is run from a network drive: put it on a local disk)";
    case XR_ERROR_FILE_CONTENTS_INVALID:
        return "XR_ERROR_FILE_CONTENTS_INVALID (a runtime or API layer json is "
               "malformed)";
    case XR_ERROR_API_LAYER_NOT_PRESENT:
        return "XR_ERROR_API_LAYER_NOT_PRESENT (an installed API layer -- "
               "ReShade and overlays register these -- is declared but "
               "missing)";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_VALIDATION_FAILURE:    return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_LIMIT_REACHED:         return "XR_ERROR_LIMIT_REACHED";
    default: break;
    }
    _snprintf(buf, sizeof(buf), "XrResult %d", (int)r);
    return buf;
}

/* Our own check, under our own name.  A borrowed VR layer's macro is usually
   compiled out in release, and then a green log means nothing was looking. */
#define XRCHECK(expr, what)                                                  \
    do {                                                                     \
        XrResult _r = (expr);                                                \
        if (XR_FAILED(_r)) {                                                 \
            kv_log("VR: %s failed: %s", what, xr_str(_r));                   \
            return 0;                                                        \
        }                                                                    \
    } while (0)

/* ---- config ------------------------------------------------------------ */
static void ini_path(char *out, size_t cb) {
    char exe[MAX_PATH], *p;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    p = strrchr(exe, '\\');
    if (p) p[1] = 0;
    _snprintf(out, cb, "%skeflings_vr.ini", exe);
}

/* Bump whenever a default changes meaning or a key is added.  A config file
   written by an older build is silently authoritative over every default in
   the source, so a new default reaches nobody who has already run the game --
   which is how a culling fix got tested with the culling fix switched off. */
#define KV_CONFIG_VERSION 92

static void write_default_config(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f,
                "; A Kingdom for Keflings VR - settings\n"
                "; Delete opengl32.dll from this folder to remove the VR port.\n"
                "\n"
                "[vr]\n"
                "enabled = 1\n"
                "\n"
                "; --- how the world is presented ---\n"
                "; 1 = stereo diorama: the kingdom stands in front of you in 3D.\n"
                "; 0 = the flat game on a big virtual screen.\n"
                "stereo = 1\n"
                "\n"
                "; Game units per real metre. THE size knob: larger makes the\n"
                "; kingdom read as a smaller model nearer your face.\n"
                "; Try 250 / 500 / 1000 and pick.\n"
                "world_scale = 500\n"
                "\n"
                "; Distance, in game units, at which the two eyes agree.\n"
                "; Lower brings the kingdom further out towards you.\n"
                "converge = 800\n"
                "\n"
                "; How much of your headset's field of view to fill.\n"
                "; 1 = all of it. Lower fits the view inside the narrower\n"
                "; frustum the engine renders, which costs immersion and is\n"
                "; no longer needed: head_cull makes the engine draw what you\n"
                "; are looking at, whichever way you turn.\n"
                "fov_scale = 1.0\n"
                "\n"
                "; Extra uniform magnification, 0..1, on top of fov_scale.\n"
                "; 0 = geometrically exact. 1 = as magnified as it can go\n"
                "; without distorting, which looks like binoculars. The stereo\n"
                "; separation grows with it so depth still agrees with size.\n"
                "fill_amount = 0\n"
                "\n"
                "; 1 = declare the compositor texture sRGB. Correct for this\n"
                "; game; 0 is a test setting and looks washed out.\n"
                "srgb_swapchain = 1\n"
                "\n"
                "; --- resolution ---\n"
                "; 1 = correct the GAME's own XScreenRes/YScreenRes, in its\n"
                "; fsall/settings.ini, to this monitor's resolution before the\n"
                "; game reads them.\n"
                "; The game lays its entire 2D out for those two numbers whether\n"
                "; or not the window it gets is that size. When they disagree,\n"
                "; menus are measured against a rectangle that is not on screen:\n"
                "; panels come out the wrong shape and some of the UI is drawn\n"
                "; outside the window entirely. A flat player never sees that,\n"
                "; because it is cropped; in VR the whole canvas is visible.\n"
                "; Your original file is backed up once, beside it, as\n"
                "; settings.ini.before-vr. Set this to 0 to manage it yourself.\n"
                "match_canvas_to_monitor = 1\n"
                "\n"
                "; 1 = make the game's window borderless and fill the monitor.\n"
                "; This is NOT exclusive full screen: the desktop display mode\n"
                "; is left alone, so your other windows are not rearranged every\n"
                "; time the game starts. It also stops the taskbar covering the\n"
                "; window and stops the title bar eating rows off the canvas.\n"
                "; The window is not kept on top, so alt-tab behaves normally.\n"
                "borderless = 1\n"
                "\n"
                "; 1 = render into our own target, sized from what your headset\n"
                "; runtime asks for. 0 = render into halves of the game's own\n"
                "; window, which lets your MONITOR decide the VR resolution --\n"
                "; on a 1440p desktop that was 1287x1431 an eye against the\n"
                "; 3072x3264 the runtime wanted. Leave this on.\n"
                "offscreen = 1\n"
                "\n"
                "; Fraction of the runtime's recommended eye size. 1.0 is what\n"
                "; it asks for.\n"
                "; LEAVE THIS AT 1.0 unless you are testing. Measured 20 Sep\n"
                "; 2026 on VirtualDesktopXR: 0.8 took the eye copy and submit\n"
                "; from 0.45 ms to 143.9 ms and the game to 5.6 fps. Our own\n"
                "; copy stays 1:1 either way, so this is the runtime taking a\n"
                "; slow path for a swapchain that is not the size it asked for.\n"
                "; It may behave on other runtimes; it has only been measured\n"
                "; on this one, and the port says so in its log when the value\n"
                "; is not 1.0.\n"
                "render_scale = 1.0\n"
                "\n"
                "; --- how far the engine draws, and where it looks ---\n"
                "; 1 = turn the engine's own view by your head, so the ground\n"
                "; and props it decides to draw are the ones you are looking\n"
                "; at. The eye transform cancels the rotation exactly, so the\n"
                "; picture does not move. Without this the world ends at a hard\n"
                "; edge below and to the right, where the flat game's camera\n"
                "; never pointed.\n"
                "head_cull = 1\n"
                "\n"
                "; How much further than default the engine should draw. It\n"
                "; stops at 2200 game units, which in a headset reads as a\n"
                "; bubble of world travelling with you. 1 = untouched,\n"
                "; 3 = three times as far, which is 6600 units.\n"
                "; This is the far plane only. It is NOT what makes ground tiles\n"
                "; vanish at the bottom edge of your view when you tilt down --\n"
                "; that is engine_fov, and it should be left at 0 so the port\n"
                "; derives it from your headset. Raise this if distant parts of\n"
                "; the map pop in, lower it if you need frames; on the test PC the\n"
                "; whole range from 1 to 4 moved the frame by less than a\n"
                "; millisecond, so it is not where the time goes.\n"
                "draw_distance = 3\n"
                "\n"
                "; The engine's camera is hardcoded to 45 degrees vertically and\n"
                "; culls to it. This writes a wider value over that camera in\n"
                "; memory so it DRAWS what you can see.\n"
                "; 0 = derive it from your headset: exactly wide enough and not\n"
                "; a degree more. A number forces that value instead.\n"
                "engine_fov = 0\n"
                "\n"
                "; Ceiling on the above. Wider covers more head turn and costs\n"
                "; frames. With head_cull on, the derived value sits well under\n"
                "; this and the ceiling never comes into play.\n"
                "engine_fov_max = 150\n"
                "\n"
                "; 1 = write the widened camera again at the end of the frame.\n"
                "; The engine resets the value every frame, and one of the two\n"
                "; moments is not always enough.\n"
                "fov_both_ends = 1\n"
                "\n"
                "; --- frame pacing and the desktop window ---\n"
                "; 1 = hold vsync off and let the headset compositor pace the\n"
                "; frames. 0 leaves the game vsynced to your desktop refresh,\n"
                "; which caps VR at that rate.\n"
                "force_vsync = 1\n"
                "\n"
                "; How many times a second to update the desktop window.\n"
                "; Presenting it every VR frame keeps the swap queue full and\n"
                "; pins the whole game to the monitor's refresh. Keep this well\n"
                "; under your desktop rate; 0 turns the window off entirely.\n"
                "mirror_fps = 30\n"
                "\n"
                "; What that window shows. The two eyes are drawn side by side\n"
                "; into one buffer, so showing it untouched is a squashed pair.\n"
                "; 0 = the left eye, cropped to the window's shape and filling\n"
                "; it -- what other headset mirrors do.\n"
                "; 1 = the whole left eye, with black bars.\n"
                "; 2 = untouched.\n"
                "mirror_mode = 0\n"
                "\n"
                "; --- menus and the in-game screen ---\n"
                "; Angular width of the 2D panel, in degrees. The game's own\n"
                "; shape is kept inside it, so raising this makes menus bigger,\n"
                "; not wider.\n"
                "; It is ALSO how far away the panel's CORNERS are, and some of\n"
                "; this game's UI is anchored to them -- the What's Next list\n"
                "; sits in the canvas's bottom-left. At 100 that corner is 54\n"
                "; degrees off centre and out at the rim of a Quest 3's view; at\n"
                "; 85 it is 46; at 75, 41; at 60, 34. Bigger text and reachable\n"
                "; corners are one dial pulling two ways, so set it for whichever\n"
                "; UI you read most.\n"
                "hud_size = 75\n"
                "\n"
                "; The same, for frames the port judges to be a menu. Keep it\n"
                "; EQUAL to hud_size unless you want the panel to change size\n"
                "; when a menu opens: the port switches between the two on a\n"
                "; draw-count threshold that flips as you click around, so any\n"
                "; difference here shows up as the whole panel jumping.\n"
                "menu_size = 75\n"
                "\n"
                "; 2D draws per frame at or above which a frame counts as a\n"
                "; menu. On this game the in-game HUD draws about 101 and the\n"
                "; main menu about 99, so NO threshold separates them -- which\n"
                "; is why this is set above both, and hud_size is what you\n"
                "; actually want to adjust. The log prints the live figure.\n"
                "menu_draws = 150\n"
                "\n"
                "; How far away the panel sits, in metres. Flat images identical\n"
                "; in both eyes read as infinitely far without this, which\n"
                "; fights a world an arm's length in front of you.\n"
                "hud_distance = 1.5\n"
                "\n"
                "; Metres the menu screen is raised above your eyes (negative:\n"
                "; lowered). Moves it straight up, so menu_tilt stays as it is.\n"
                "; 0.5 was chosen in the headset (25 Sep). -2 to 2.\n"
                "menu_height = 0.5\n"
                "\n"
                "; Degrees the menu screen (menus, blueprints, tech tree, HUD) leans\n"
                "; back at the top; NEGATIVE leans the top toward you. The game looks\n"
                "; down on the kingdom at about 44 degrees, so the world in front of\n"
                "; you is tipped up like a ramp and an upright screen seems to lean\n"
                "; back against it. -40, chosen in the headset (25 Sep), stands it up\n"
                "; in the world; 0 = upright. (30 laid it almost flat.)\n"
                "; -60 to 60.\n"
                "menu_tilt = -40\n"
                "\n"
                "; Carries the game's menu dimming out to the edge of your view.\n"
                "; The game darkens the world behind a menu by drawing into its\n"
                "; own 2D canvas, which here is a panel of a set angular size, so\n"
                "; without this the darkening stops at the panel edge and the\n"
                "; world stays bright around it.\n"
                "; 1.0 is exact. The surround is painted in the colour AND alpha\n"
                "; of the game's own backdrop quad, which on this game is a dark\n"
                "; navy at alpha 0.49, not black. It once shipped at 0.92 to hide\n"
                "; a rectangle edge: that edge was the missing blue, and 0.92\n"
                "; traded it for a red and green mismatch.\n"
                "; tools/dim_shape.ps1 re-runs the measurement, per channel.\n"
                "; This is a MULTIPLIER on whatever the game is doing, not a\n"
                "; fixed amount: 1 matches it exactly and joins without a seam,\n"
                "; 0.5 is half as dark, 0 turns it off. Screens the game does not\n"
                "; dim are left alone whatever this says.\n"
                "menu_dim = 1.0\n"
                "\n"
                "; Look for a menu's dimming backdrop in two extra places.\n"
                "; Normally the port only inspects immediate-mode draws and\n"
                "; display lists, and only recognises a backdrop by its dark\n"
                "; colour. With this on it also inspects vertex-array draws, and\n"
                "; accepts a translucent shape that covers essentially the whole\n"
                "; canvas whatever colour it carries.\n"
                "; Aimed at the blueprint DETAILS page, whose backdrop the normal\n"
                "; search cannot see. UNVERIFIED: it has not been shown to fix\n"
                "; that page, and inspecting more draws is how a screen that\n"
                "; should stay bright ends up dark. Off unless someone is\n"
                "; actively testing it.\n"
                "menu_dim_wide_search = 0\n"
                "\n"
                "; A second way of spotting the game's menu dimming, for the\n"
                "; screens that do it with a texture instead of a colour.\n"
                "; The normal detector looks for a near-black translucent quad.\n"
                "; Some screens -- the blueprint DETAILS page is one -- draw a\n"
                "; dark texture modulated by a WHITE vertex colour instead, so\n"
                "; the colour the port can read is white and the darkness is in\n"
                "; the texture, where it cannot be read at all.\n"
                "; With this on, a menu whose darkest translucent draw has a\n"
                "; usable alpha is dimmed by that alpha even when its colour is\n"
                "; not dark. Screens that draw nothing translucent, or draw it\n"
                "; at alpha 0, are still left alone -- the blueprint list itself\n"
                "; reads 0.00 and stays bright.\n"
                "; OFF by default: darkening a screen the game meant to leave\n"
                "; bright is worse than leaving one undimmed, and this rule has\n"
                "; been tried against only a handful of screens so far.\n"
                "menu_dim_alpha_only = 0\n"
                "\n"
                "; 1 = the 2D panel stands still in the world, where you were\n"
                "; looking when it appeared, so you can turn your head to read\n"
                "; the corners of it -- this game puts the What's Next list in\n"
                "; the bottom-left corner, and a panel welded to your face moves\n"
                "; that corner away exactly as fast as you turn towards it.\n"
                "; It re-anchors if you turn far enough that it leaves your view,\n"
                "; so it cannot be lost behind you.\n"
                "; 0 = the panel follows your head.\n"
                "hud_world_lock = 1\n"
                "\n"
                "; DIAGNOSTIC. Capture the 2D pass and blit it back over the\n"
                "; window at 1:1, with no headset and no VR session. A correct\n"
                "; capture is pixel-identical to panel_once = 0, so this is the\n"
                "; A/B that proves the blend rather than asserting it. Leave 0.\n"
                "panel_flat_test = 0\n"
                "\n"
                "; Skip scenery draws that cannot change a single pixel.\n"
                "; The game draws an invisible highlight overlay over most of the\n"
                "; world every frame -- fully transparent until something is\n"
                "; highlighted -- and in VR pays for it twice. When a draw is\n"
                "; provably invisible it is skipped; the moment the game makes it\n"
                "; visible, it is drawn again.\n"
                "; It checks itself with the graphics card before it skips anything,\n"
                "; and turns itself off if a check ever fails.\n"
                "; 0 off, 1 on (the default since 0.10.0), 2 alternates on and off to\n"
                "; measure the difference. 3 follows merge_group's cycle.\n"
                "skip_noop = 1\n"
                "\n"
                "; A desk measurement aid. With no headset connected, 1 makes the\n"
                "; port draw everything twice -- as it does for your two eyes -- into\n"
                "; the left and right halves of the window. It looks wrong on purpose;\n"
                "; it exists so the cost of the second eye can be measured without a\n"
                "; headset. Leave it at 0.\n"
                "desk_stereo = 0\n"
                "\n"
                "; Draw scenery that looks alike in one go instead of one piece at a\n"
                "; time -- the biggest part of 0.10.0's speed-up. 9 is the setting to\n"
                "; play with (the default). 0 draws everything the game's own way,\n"
                "; slower: use it, with skip_noop = 0, if anything ever looks wrong,\n"
                "; and say what and where. The other numbers are test modes:\n"
                "; 0 leaves everything as it is.\n"
                "; 1 is an audit: it picks the biggest group of scenery that shares\n"
                "; one look, checks that every piece of it really does, and draws it\n"
                "; two ways off screen to compare them pixel for pixel. Nothing you\n"
                "; see changes.\n"
                "; 2 merges every group that is safe to draw in any order, from\n"
                "; buffers on the graphics card.\n"
                "; 3 does the same for 10 seconds, then draws normally for 10,\n"
                "; repeating, and logs the frame rate of each half separately --\n"
                "; so one session shows the speed difference and whether anything\n"
                "; looks different.\n"
                "; 4 is a desk check: it draws normally but compares every merged\n"
                "; group pixel for pixel against the game's own drawing.\n"
                "; 5 draws every piece from its own buffer on the graphics card, in\n"
                "; its original place and order, alternating 10 s on and 10 s off.\n"
                "; 6 combines both: merges what is safe to merge, draws everything\n"
                "; else from its own buffer in its original place, alternating.\n"
                "; 7 cycles three states, 5 s each in VR: the game as it is, merging\n"
                "; only what is provably order-safe, and merging everything including\n"
                "; cut-out and soft-edged scenery -- with the frame rate of each.\n"
                "; 9 is the third of those, all the time: every group merged,\n"
                "; including cut-out and soft-edged scenery. Confirmed in the\n"
                "; headset 24 Sep 2026 -- nothing looked different, 74 -> 84 fps\n"
                "; with skip_noop = 1.\n"
                "merge_group = 9\n"
                "\n"
                "; Push merged scenery back by this many depth steps. A merged draw\n"
                "; computes positions on the CPU, so its depth can differ from the\n"
                "; engine's in the last bits, and a layer the game draws exactly on\n"
                "; top -- the snow on trees, rocks and crystals -- lost to it and\n"
                "; flickered. 16 steps is a hundredth of a game unit at 1000 units:\n"
                "; measured on a snowy save, merged and unmerged frames then differ\n"
                "; less than two unmerged frames do. 0 = off, which falls back to\n"
                "; keeping layered objects out of the merge (slower in the snow).\n"
                "merge_offset = 16\n"
                "\n"
                "; Track the world's placement in software rather than asking the\n"
                "; driver for it, which would stall the pipeline thousands of times a\n"
                "; frame. It checks itself against the driver once and says how far\n"
                "; apart they were.\n"
                "; It also measures how much of the scene is standing still:\n"
                "; buildings do not move, keflings do, and that ratio decides how\n"
                "; much work a merging renderer would have to redo each frame.\n"
                "; Diagnostic only. Changes nothing you can see.\n"
                "mv_shadow = 0\n"
                "\n"
                "; Keep our own copy of the geometry in each display list, read as\n"
                "; the engine compiles it. This draws nothing and changes nothing --\n"
                "; every call still goes through to the driver exactly as before.\n"
                "; It exists to answer two questions the merging design needs: how\n"
                "; many vertices a frame actually draws, and how often the engine\n"
                "; rebuilds its lists.\n"
                "; Diagnostic only. Leave it at 0 unless a measurement is being\n"
                "; taken.\n"
                "mesh_capture = 0\n"
                "\n"
                "; Count how many DISTINCT state groups a frame's world pass contains.\n"
                "; Consecutive draws never share state in this engine -- but that is\n"
                "; the order it happens to emit them in, and a batching renderer sorts\n"
                "; first. What matters is how many groups there are in the whole\n"
                "; frame, and this counts them, along with how many distinct display\n"
                "; lists are called and how often each repeats.\n"
                "; Diagnostic only. It changes nothing you can see and costs a little\n"
                "; time per draw, so leave it at 0 unless a measurement is being\n"
                "; taken.\n"
                "group_census = 0\n"
                "\n"
                "; Narrow how WIDE the engine culls, without changing what you see.\n"
                "; It builds its camera from one vertical angle times its own 16:9\n"
                "; shape, so making it tall enough for a headset also makes it 141\n"
                "; degrees wide -- and an eye needs about 116. Everything between is\n"
                "; drawn and never seen.\n"
                "; 1.0 is about right for a Quest 3; 1.7778 is the engine's own.\n"
                "; 0 leaves it alone. Your view cannot shrink: each eye is rendered\n"
                "; from the headset's frustum, not this one.\n"
                "; Measured worth little -- tripling the engine's frustum only\n"
                "; changes its draw count by a third -- so treat any gain as a\n"
                "; bonus.\n"
                "engine_aspect = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = switch engine_tanx on and off every 300 frames\n"
                "; and report what each arm submitted. Two separate launches cannot\n"
                "; be compared here -- the scene differs by hundreds of draws.\n"
                "engine_tanx_ab = 0\n"
                "\n"
                "; Narrow how wide the engine CULLS, without changing what you see.\n"
                "; The engine builds its camera from a single vertical angle and its\n"
                "; own 16:9 shape, so making it tall enough for a headset also makes\n"
                "; it 141 degrees WIDE -- and a Quest 3 eye only needs about 116.\n"
                "; Everything in between is drawn and never seen.\n"
                "; This is the horizontal tangent to cull to: 1.6 is about 116\n"
                "; degrees, 2.88 is what the engine picks on its own. 0 leaves it\n"
                "; alone. It cannot shrink your view: what you see is rendered from\n"
                "; the headset's own frustum, not the engine's.\n"
                "engine_tanx = 0\n"
                "\n"
                "; 1 = skip the graphics calls that set something to the value it\n"
                "; already has. This engine sets everything from scratch for every\n"
                "; object it draws: about 14 calls between every two draws, some\n"
                "; 38,000 a frame, and most of them change nothing.\n"
                "; Safe here because the engine's display lists were measured and\n"
                "; contain only geometry, so calling one cannot change the state\n"
                "; being tracked. The cache is dropped at the start of every frame,\n"
                "; on glPushAttrib and glPopAttrib, on a context switch and on a\n"
                "; texture-unit change.\n"
                "; The frame report says how many calls it skipped. F5 toggles it\n"
                "; while the game runs.\n"
                "state_filter = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = run the development scaffolding: the module\n"
                "; list, the camera and heap dumps, the GL call census, and a full\n"
                "; frame trace every time a menu opens.\n"
                "; Every one of those walks memory or writes files on the render\n"
                "; thread. Measured on a second test PC: the batch at frame 300\n"
                "; cost a 784 ms stall, and the trace armed by opening a menu is\n"
                "; about a hundred flushed log lines each time. Leave this off\n"
                "; unless you are asked for it.\n"
                "diagnostics = 0\n"
                "\n"
                "; Frames that take longer than this many milliseconds are\n"
                "; recorded, and the worst few are printed in the next frame\n"
                "; report with where their time went. A stall that is mostly\n"
                "; 'waiting on the compositor' is the headset link; one that is\n"
                "; mostly unaccounted is the game or the graphics driver.\n"
                "stall_ms = 25\n"
                "\n"
                "; 1 = let the engine decide what to draw from where your EYE is,\n"
                "; not from where its own camera sits.\n"
                "; head_cull already turns the engine's view to follow your head.\n"
                "; This does the same for your head's POSITION, which world_scale\n"
                "; multiplies by 500 -- so leaning 20 cm puts your eye 100 game\n"
                "; units away from the camera that chose what to draw, and the\n"
                "; nearest scenery at the edge of your view gets dropped. That is\n"
                "; the squares vanishing at the bottom when you tilt down, and it\n"
                "; comes and goes as you lean, which is how it was identified.\n"
                "; The picture is unchanged: the offset is added to the engine's\n"
                "; matrix and subtracted from the eye's, and the port checks that\n"
                "; on the first frame and prints the result.\n"
                "; If the world moves twice as far as your head, or swims when you\n"
                "; lean, press F6 or set this to 0 and tell me -- that would mean\n"
                "; one of those two halves is wrong.\n"
                "cull_follow_head = 1\n"
                "\n"
                "; 1 = ignore the engine's glFinish, which it calls once every\n"
                "; frame. That call stops the processor dead until the graphics\n"
                "; card has finished everything sent so far, so the two never\n"
                "; work at the same time and your frame costs processor time\n"
                "; PLUS card time instead of whichever is larger. Measured on a\n"
                "; Test PC: engine 10.2 ms, card 6.9 ms, frame 13.6 ms -- much\n"
                "; nearer the sum than the larger. In 2010 on a 60 Hz monitor\n"
                "; that call was harmless; in VR it wastes half the machine, and\n"
                "; the headset compositor already paces the frames.\n"
                "; If anything looks stale or flickers, set this to 0. F7\n"
                "; toggles it while the game runs and says so in the log.\n"
                "skip_glfinish = 1\n"
                "\n"
                "; DIAGNOSTIC, measured neutral. 1 = collect each glBegin/glEnd\n"
                "; block on the CPU and draw it as one array draw per eye instead\n"
                "; of forwarding the engine's ~150,000 vertex calls a frame and\n"
                "; compiling a display list per block. A/B/A on a paused frame,\n"
                "; 20 Sep 2026, test PC: 13.0 / 12.8 / 13.0 ms -- no difference,\n"
                "; with 92-byte and with 36-byte vertices alike. The driver's\n"
                "; immediate-mode path is already cheap; the engine's time is its\n"
                "; own work per vertex. The picture is identical either way.\n"
                "block_arrays = 0\n"
                "\n"
                "; 1 = draw the world for both eyes with ONE state switch per\n"
                "; draw instead of three: draw N goes left eye then right, draw\n"
                "; N+1 right then left, and the engine's own projection and\n"
                "; viewport are put back only when it is about to look at them.\n"
                "; Each eye still gets every draw in the engine's order, so the\n"
                "; picture is the same. 0 = switch to each eye and back for every\n"
                "; draw, the way it was before 20 Sep 2026; measured at 1.5 ms\n"
                "; of an 11-13 ms frame on the test PC.\n"
                "dup_alternate = 1\n"
                "\n"
                "; DIAGNOSTIC, and measured SLOWER. 1 = record each scene pass\n"
                "; into one display list while the first eye draws and replay it\n"
                "; for the second. On the test PC the driver spends about\n"
                "; 10 ms a frame compiling that list: 45 fps against 90. Kept so\n"
                "; the measurement can be repeated, not to be used.\n"
                "stereo_pass_list = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = time each part of every duplicated draw. About\n"
                "; eight clock reads a draw, half a millisecond a frame, so the\n"
                "; frame report's duplication split is only filled in with this\n"
                "; on -- and the numbers include the instrument's own cost.\n"
                "dup_profile = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = log which GL calls the engine makes between\n"
                "; glBegin and glEnd, how many vertices a block carries and which\n"
                "; primitive modes it uses. For deciding whether the blocks can be\n"
                "; drawn as arrays.\n"
                "block_census = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = the desktop window mirrors the RIGHT eye. The\n"
                "; left eye is the one the engine draws itself; the right is the\n"
                "; duplicated one, so this is how a duplication change is\n"
                "; photographed.\n"
                "mirror_eye = 0\n"
                "\n"
                "; 1 = draw the 2D pass ONCE into a canvas-sized target and\n"
                "; blit it to each eye, rather than re-issuing every 2D draw\n"
                "; per eye at eye resolution. The panel is a flat image and is\n"
                "; identical in both eyes; only the projection differs.\n"
                "; Measured in VR on the tech tree, 20 Sep: 61.9 -> 82.4 fps,\n"
                "; duplication 5.56 -> 1.41 ms, GPU 8.39 -> 4.82 ms, and 2D\n"
                "; duplications 425400 -> 0. In ordinary play 87.7 -> 88.9, so\n"
                "; no regression there. The picture was checked by toggling it\n"
                "; live on a paused frame: the difference is within head\n"
                "; jitter. Set to 0 to go back to drawing the 2D per eye.\n"
                "panel_once = 1\n"
                "\n"
                "; DIAGNOSTIC. 1 = paint the held dim's canvas fill on EVERY\n"
                "; full-screen menu, not only when the game has stopped drawing\n"
                "; its own backdrop. The held state is otherwise reachable only\n"
                "; through blueprints -> details -> back, which makes the one\n"
                "; thing that was broken hard to look at. With menu_dim_probe\n"
                "; the canvas fill is green and the surround blue, so a single\n"
                "; capture says which drew. Leave at 0.\n"
                "menu_dim_hold_force = 0\n"
                "\n"
                "; 1 = honour the engine's clip rectangle, mapped through the\n"
                "; panel transform. Scrolling lists -- the tips and blueprints\n"
                "; menus -- stay inside their box.\n"
                "; 0 = ignore it. Nothing can be hidden, but those lists then\n"
                "; draw over the whole panel instead of scrolling within it.\n"
                "; 2 = clip PLANES rather than a rectangle, and the default. Ascissor is axis aligned in SCREEN pixels, and since the panel\n"
                "; stands in the world the region to clip is a rotated, skewed\n"
                "; quad -- so at 1 a scrolling list leaks more of itself as you\n"
                "; roll your head. Planes are given in the engine's own\n"
                "; coordinates and are right at any head angle.\n"
                "; (It once blacked the whole screen, because the planes were\n"
                "; left enabled during the WORLD pass, where canvas coordinates\n"
                "; clip everything. They are now confined to the 2D pass.)\n"
                "; A clip rectangle can only HIDE things, so if UI goes missing\n"
                "; in a menu, try 1, then 0.\n"
                "scissor_mode = 2\n"
                "\n"
                "; --- controls ---\n"
                "; 1 = drive the game with the Quest controllers. The PC build\n"
                "; has no gamepad code left, so they are translated into the\n"
                "; keyboard and mouse it does read. Left stick moves, right\n"
                "; stick is the pointer, right trigger clicks, A is Enter,\n"
                "; B is Escape, menu is Escape.\n"
                "emulate_input = 1\n"
                "\n"
                "; 1 = this port reads the controllers itself. 0 = leave them to\n"
                "; Virtual Desktop's gamepad emulation.\n"
                "controllers = 1\n"
                "\n"
                "; 0 = whichever device moved last owns the cursor.\n"
                "; 1 = controller: the ray owns it and the mouse is ignored.\n"
                "; 2 = mouse and keyboard: the controller never touches it.\n"
                "; Hold both grips for a second to switch between 1 and 2\n"
                "; without leaving the game.\n"
                "input_mode = 1\n"
                "\n"
                "; What an AIMED cursor (pointer_mode 1) is measured against.\n"
                "; 0 = the 2D panel. Pointing at icons and menus is exact, but\n"
                "; the ground highlight runs AHEAD of where you point, by however\n"
                "; much wider the engine camera is than the panel -- at 75 and\n"
                "; 141 degrees that is about 1.9x.\n"
                "; 1 = the engine's camera, which is what the game unprojects the\n"
                "; cursor through to pick a tile. The highlight then lands where\n"
                "; you point, and icons are off by the same ratio the other way.\n"
                "; These agree only when hud_size equals the engine's frustum,\n"
                "; which puts the panel's corners out of reach. Pick for whether\n"
                "; you click tiles or icons more.\n"
                "pointer_space = 0\n"
                "\n"
                "; 3 = THE GRIP MOUSE (default): controller play as on the Xbox --\n"
                "; no cursor, A acts on what you look at. Hold the right grip and\n"
                "; the cursor follows where your hand points, and the trigger\n"
                "; clicks. In menus it always follows your hand.\n"
                "; 0 = the right stick nudges the cursor.\n"
                "; 1 = the cursor goes where the right controller points.\n"
                "; 2 = in menus the cursor goes where the controller points; in\n"
                "; the world the right stick nudges it.\n"
                "pointer_mode = 3\n"
                "\n"
                "; THE LEFTORIUM -- left-handed play. 1 = the cursor stick, the\n"
                "; click trigger, A/B and pointing move to the left controller;\n"
                "; walking, X/Y and the other trigger to the right. The menu\n"
                "; button stays on the left: it is the only one there is.\n"
                "leftorium = 0\n"
                "stick_deadzone = 0.25\n"
                "\n"
                "; Cursor speed for pointer_mode 0.\n"
                "mouse_speed = 14\n"
                "\n"
                "; How long the physical mouse keeps the cursor after it stops\n"
                "; moving. 0 = the controller always has it.\n"
                "pointer_yield_ms = 400\n"
                "\n"
                "; 0..1, for pointer_mode 1: how much of the previous position\n"
                "; to keep. A raw aim pose is jittery at a cursor's distance.\n"
                "pointer_smooth = 0.55\n"
                "\n"
                "; 1 = the left stick sends the arrow keys as well as WASD.\n"
                "move_arrows = 1\n"
                "\n"
                "; 1 = post key messages straight to the game window instead of\n"
                "; injecting them into the system input stream.\n"
                "key_post = 0\n"
                "\n"
                "; 1 = once, shortly after a level loads, drive the cursor into\n"
                "; the top-left corner. The game does not read where the cursor\n"
                "; is -- it accumulates how far the cursor MOVED into a position\n"
                "; of its own -- so the two drift apart and the highlight square\n"
                "; ends up on a different tile from your pointer. The gap cannot\n"
                "; be measured, but cornering both squeezes it out: ours stops at\n"
                "; the screen edge, the game's at its canvas edge, and they are\n"
                "; then in the same place.\n"
                "; You will see the cursor flick to the corner once. That is it.\n"
                "; OFF by default: it was built to fix the cursor and the ground\n"
                "; highlight disagreeing, and it did not -- that turned out to be\n"
                "; a SCALE mismatch, not an offset, so cornering cannot help.\n"
                "; Kept because the reasoning is sound for a genuine offset.\n"
                "cursor_sync = 0\n"
                "\n"
                "; 1 = hold the cursor at one point and ignore the mouse, the\n"
                "; way the Xbox version has no cursor and highlights whatever is\n"
                "; in front of you. Experimental.\n"
                "cursor_lock = 0\n"
                "\n"
                "; Where to hold it, as a fraction of the window.\n"
                "cursor_lock_x = 0.5\n"
                "cursor_lock_y = 0.5\n"
                "\n"
                "; --- the flat-screen mode (stereo = 0) ---\n"
                "; 1 = a world-locked stereo window you look around.\n"
                "; 0 = project into the headset's own field of view.\n"
                "screen_mode = 0\n"
                "screen_distance = 2.2\n"
                "screen_width = 3.2\n"
                "quad_distance = 2.5\n"
                "quad_width = 3.0\n"
                "\n"
                "; --- diagnostics ---\n"
                "; Keep dimming a full-screen menu the game has stopped dimming.\n"
                "; Opening the building flowchart dims the world; stepping into a\n"
                "; building's details keeps it dimmed; stepping back out (Back or\n"
                "; Escape) leaves it bright, because the game stops drawing its\n"
                "; own backdrop. Measured in the unmodified flat game on 20 Sep\n"
                "; 2026: the world behind that flowchart is at full brightness,\n"
                "; so this is the game's own bug, and it is far more obvious in a\n"
                "; headset. This holds the dim that was already found, in the\n"
                "; game's own colour, while a full-screen menu is still up. It\n"
                "; can only EXTEND a dim the game itself drew, never invent one,\n"
                "; so the worst it can do is leave a menu dimmed a little longer\n"
                "; than the game meant to. Verified in the eye: the held frame\n"
                "; and the real one match to the pixel.\n"
                "; Set to 0 to follow the game exactly, bug and all.\n"
                "menu_dim_hold = 1\n"
                "\n"
                "; Work out a menu's dimming by measuring it rather than by\n"
                "; recognising the draw that caused it.\n"
                "; This game draws every 2D element with a white vertex colour\n"
                "; and much of it through display lists, so there is no dark\n"
                "; quad to find and no extent to measure. Instead the port reads\n"
                "; the view at three points as the 2D pass begins and again when\n"
                "; it ends: whatever the game drew in between, the difference is\n"
                "; the dimming, and that is the figure the surround matches.\n"
                "; Costs one small readback every eighth frame while a menu is\n"
                "; open, and nothing at all otherwise.\n"
                "menu_dim_measure = 0\n"
                "\n"
                "; DIAGNOSTIC. Lists every translucent draw in a menu's 2D pass\n"
                "; -- colour, alpha, how much of the canvas it covers, and which\n"
                "; draw path it came by. For finding a menu backdrop the normal\n"
                "; search cannot see, without guessing at what it might look\n"
                "; like. Verbose; leave off unless hunting.\n"
                "dim_trace = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = paint the surround dim bright blue and log\n"
                "; the rectangle it covers, to see whether it drew and where.\n"
                "; Not a setting to play with.\n"
                "menu_dim_probe = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = the Y button sends the next key from a list\n"
                "; of candidates instead of its normal binding, and logs which.\n"
                "; For finding an action whose key is not documented.\n"
                "; Note for the next person: PUNCH has no key. Testing the retail\n"
                "; game key by key found that the only way to punch and demolish\n"
                "; a building is to hover the cursor over the fist icon in the\n"
                "; HUD and click it. Do not go looking for a binding.\n"
                "key_sweep = 0\n"
                "\n"
                "; DIAGNOSTIC. 1 = alternate the 2D layer between both eyes\n"
                "; and the left eye only every 300 frames, and log the frame\n"
                "; rate for each. The 2D is missing from one eye half the\n"
                "; time, so this is for measuring, not for playing.\n"
                "hud_ab = 0\n"
                "\n"
                "; 1 = dump the engine's string table to a file. Stalls the game\n"
                "; for a few seconds.\n"
                "dump_strings = 0\n"
                "\n"
                "; Do not edit: says which build wrote this file. A newer build\n"
                "; replaces the file rather than let a stale default win.\n"
                "config_version = %d\n", KV_CONFIG_VERSION);
        fclose(f);
    }
}

static void put_f(const char *path, const char *key, float v) {
    char buf[64];
    _snprintf(buf, sizeof(buf), "%g", (double)v);
    WritePrivateProfileStringA("vr", key, buf, path);
}

static void put_i(const char *path, const char *key, int v) {
    char buf[32];
    _snprintf(buf, sizeof(buf), "%d", v);
    WritePrivateProfileStringA("vr", key, buf, path);
}

/* Read everything into g_vrcfg.  Missing keys take their default, which is how
   a new key arrives without disturbing anything else. */
static void read_all(const char *path) {
    char buf[64];
#define GETF(key, def, field)                                                    GetPrivateProfileStringA("vr", key, def, buf, sizeof(buf), path);            g_vrcfg.field = (float)atof(buf)
#define GETI(key, def, field)                                                    GetPrivateProfileStringA("vr", key, def, buf, sizeof(buf), path);            g_vrcfg.field = atoi(buf)
    GETI("enabled", "1", enabled);
    GETI("stereo", "1", stereo);
    GETF("world_scale", "500", world_scale);
    GETF("converge", "800", converge);
    GETF("fov_scale", "1.0", fov_scale);
    GETF("fill_amount", "0", fill_amount);
    GETI("srgb_swapchain", "1", srgb_swapchain);
    GETI("match_canvas_to_monitor", "1", match_canvas_to_monitor);
    GETI("borderless", "1", borderless);
    GETI("offscreen", "1", offscreen);
    GETF("render_scale", "1.0", render_scale);
    GETI("head_cull", "1", head_cull);
    GETF("draw_distance", "3", draw_distance);
    GETF("engine_fov", "0", engine_fov);
    GETF("engine_fov_max", "150", engine_fov_max);
    GETI("fov_both_ends", "1", fov_both_ends);
    GETI("force_vsync", "1", force_vsync);
    GETF("mirror_fps", "30", mirror_fps);
    GETI("mirror_mode", "0", mirror_mode);
    GETF("hud_size", "75", hud_size);
    GETF("menu_size", "75", menu_size);
    GETI("menu_draws", "150", menu_draws);
    GETF("hud_distance", "1.5", hud_distance);
    GETF("menu_height", "0.5", menu_height);
    GETF("menu_tilt", "-40", menu_tilt);
    GETF("menu_dim", "1.0", menu_dim);
    GETI("menu_dim_wide_search", "0", menu_dim_wide_search);
    GETI("menu_dim_alpha_only", "0", menu_dim_alpha_only);
    GETI("hud_world_lock", "1", hud_world_lock);
    GETI("panel_flat_test", "0", panel_flat_test);
    GETI("skip_noop", "1", skip_noop);
    GETI("desk_stereo", "0", desk_stereo);
    GETI("merge_group", "9", merge_group);
    GETF("merge_offset", "16", merge_offset);
    GETI("mv_shadow", "0", mv_shadow);
    GETI("mesh_capture", "0", mesh_capture);
    GETI("group_census", "0", group_census);
    GETF("engine_aspect", "0", engine_aspect);
    GETI("engine_tanx_ab", "0", engine_tanx_ab);
    GETF("engine_tanx", "0", engine_tanx);
    GETI("state_filter", "0", state_filter);
    GETI("diagnostics", "0", diagnostics);
    GETF("stall_ms", "25", stall_ms);
    GETI("cull_follow_head", "1", cull_follow_head);
    GETI("skip_glfinish", "1", skip_glfinish);
    GETI("block_arrays", "0", block_arrays);
    GETI("dup_alternate", "1", dup_alternate);
    GETI("stereo_pass_list", "0", stereo_pass_list);
    GETI("dup_profile", "0", dup_profile);
    GETI("block_census", "0", block_census);
    GETI("mirror_eye", "0", mirror_eye);
    GETI("panel_once", "1", panel_once);
    GETI("menu_dim_hold_force", "0", menu_dim_hold_force);
    GETI("scissor_mode", "2", scissor_mode);
    GETI("emulate_input", "1", emulate_input);
    GETI("controllers", "1", controllers);
    GETI("input_mode", "1", input_mode);
    GETI("pointer_space", "0", pointer_space);
    GETI("pointer_mode", "3", pointer_mode);
    GETI("leftorium", "0", leftorium);
    GETF("stick_deadzone", "0.25", stick_deadzone);
    GETF("mouse_speed", "14", mouse_speed);
    GETF("pointer_yield_ms", "400", pointer_yield_ms);
    GETF("pointer_smooth", "0.55", pointer_smooth);
    GETI("move_arrows", "1", move_arrows);
    GETI("key_post", "0", key_post);
    GETI("cursor_sync", "0", cursor_sync);
    GETI("cursor_lock", "0", cursor_lock);
    GETF("cursor_lock_x", "0.5", cursor_lock_x);
    GETF("cursor_lock_y", "0.5", cursor_lock_y);
    GETI("screen_mode", "0", screen_mode);
    GETF("screen_distance", "2.2", screen_distance);
    GETF("screen_width", "3.2", screen_width);
    GETF("quad_distance", "2.5", quad_distance);
    GETF("quad_width", "3.0", quad_width);
    GETI("menu_dim_hold", "1", menu_dim_hold);
    GETI("menu_dim_measure", "0", menu_dim_measure);
    GETI("dim_trace", "0", dim_trace);
    GETI("menu_dim_probe", "0", menu_dim_probe);
    GETI("key_sweep", "0", key_sweep);
    GETI("hud_ab", "0", hud_ab);
    GETI("dump_strings", "0", dump_strings);
#undef GETF
#undef GETI
}

/* Write the whole of g_vrcfg back, so regenerating the file for a new version
   keeps whatever was set rather than silently reverting it. */
static void write_all(const char *path) {
    put_i(path, "enabled", g_vrcfg.enabled);
    put_i(path, "stereo", g_vrcfg.stereo);
    put_f(path, "world_scale", g_vrcfg.world_scale);
    put_f(path, "converge", g_vrcfg.converge);
    put_f(path, "fov_scale", g_vrcfg.fov_scale);
    put_f(path, "fill_amount", g_vrcfg.fill_amount);
    put_i(path, "srgb_swapchain", g_vrcfg.srgb_swapchain);
    put_i(path, "match_canvas_to_monitor", g_vrcfg.match_canvas_to_monitor);
    put_i(path, "borderless", g_vrcfg.borderless);
    put_i(path, "offscreen", g_vrcfg.offscreen);
    put_f(path, "render_scale", g_vrcfg.render_scale);
    put_i(path, "head_cull", g_vrcfg.head_cull);
    put_f(path, "draw_distance", g_vrcfg.draw_distance);
    put_f(path, "engine_fov", g_vrcfg.engine_fov);
    put_f(path, "engine_fov_max", g_vrcfg.engine_fov_max);
    put_i(path, "fov_both_ends", g_vrcfg.fov_both_ends);
    put_i(path, "force_vsync", g_vrcfg.force_vsync);
    put_f(path, "mirror_fps", g_vrcfg.mirror_fps);
    put_i(path, "mirror_mode", g_vrcfg.mirror_mode);
    put_f(path, "hud_size", g_vrcfg.hud_size);
    put_f(path, "menu_size", g_vrcfg.menu_size);
    put_i(path, "menu_draws", g_vrcfg.menu_draws);
    put_f(path, "hud_distance", g_vrcfg.hud_distance);
    put_f(path, "menu_height", g_vrcfg.menu_height);
    put_f(path, "menu_tilt", g_vrcfg.menu_tilt);
    put_f(path, "menu_dim", g_vrcfg.menu_dim);
    put_i(path, "menu_dim_wide_search", g_vrcfg.menu_dim_wide_search);
    put_i(path, "menu_dim_alpha_only", g_vrcfg.menu_dim_alpha_only);
    put_i(path, "hud_world_lock", g_vrcfg.hud_world_lock);
    put_i(path, "panel_flat_test", g_vrcfg.panel_flat_test);
    put_i(path, "skip_noop", g_vrcfg.skip_noop);
    put_i(path, "desk_stereo", g_vrcfg.desk_stereo);
    put_i(path, "merge_group", g_vrcfg.merge_group);
    put_f(path, "merge_offset", g_vrcfg.merge_offset);
    put_i(path, "mv_shadow", g_vrcfg.mv_shadow);
    put_i(path, "mesh_capture", g_vrcfg.mesh_capture);
    put_i(path, "group_census", g_vrcfg.group_census);
    put_f(path, "engine_aspect", g_vrcfg.engine_aspect);
    put_i(path, "engine_tanx_ab", g_vrcfg.engine_tanx_ab);
    put_f(path, "engine_tanx", g_vrcfg.engine_tanx);
    put_i(path, "state_filter", g_vrcfg.state_filter);
    put_i(path, "diagnostics", g_vrcfg.diagnostics);
    put_f(path, "stall_ms", g_vrcfg.stall_ms);
    put_i(path, "cull_follow_head", g_vrcfg.cull_follow_head);
    put_i(path, "skip_glfinish", g_vrcfg.skip_glfinish);
    put_i(path, "block_arrays", g_vrcfg.block_arrays);
    put_i(path, "dup_alternate", g_vrcfg.dup_alternate);
    put_i(path, "stereo_pass_list", g_vrcfg.stereo_pass_list);
    put_i(path, "dup_profile", g_vrcfg.dup_profile);
    put_i(path, "block_census", g_vrcfg.block_census);
    put_i(path, "mirror_eye", g_vrcfg.mirror_eye);
    put_i(path, "panel_once", g_vrcfg.panel_once);
    put_i(path, "menu_dim_hold_force", g_vrcfg.menu_dim_hold_force);
    put_i(path, "scissor_mode", g_vrcfg.scissor_mode);
    put_i(path, "emulate_input", g_vrcfg.emulate_input);
    put_i(path, "controllers", g_vrcfg.controllers);
    put_i(path, "input_mode", g_vrcfg.input_mode);
    put_i(path, "pointer_space", g_vrcfg.pointer_space);
    put_i(path, "pointer_mode", g_vrcfg.pointer_mode);
    put_i(path, "leftorium", g_vrcfg.leftorium);
    put_f(path, "stick_deadzone", g_vrcfg.stick_deadzone);
    put_f(path, "mouse_speed", g_vrcfg.mouse_speed);
    put_f(path, "pointer_yield_ms", g_vrcfg.pointer_yield_ms);
    put_f(path, "pointer_smooth", g_vrcfg.pointer_smooth);
    put_i(path, "move_arrows", g_vrcfg.move_arrows);
    put_i(path, "key_post", g_vrcfg.key_post);
    put_i(path, "cursor_sync", g_vrcfg.cursor_sync);
    put_i(path, "cursor_lock", g_vrcfg.cursor_lock);
    put_f(path, "cursor_lock_x", g_vrcfg.cursor_lock_x);
    put_f(path, "cursor_lock_y", g_vrcfg.cursor_lock_y);
    put_i(path, "screen_mode", g_vrcfg.screen_mode);
    put_f(path, "screen_distance", g_vrcfg.screen_distance);
    put_f(path, "screen_width", g_vrcfg.screen_width);
    put_f(path, "quad_distance", g_vrcfg.quad_distance);
    put_f(path, "quad_width", g_vrcfg.quad_width);
    put_i(path, "menu_dim_hold", g_vrcfg.menu_dim_hold);
    put_i(path, "menu_dim_measure", g_vrcfg.menu_dim_measure);
    put_i(path, "dim_trace", g_vrcfg.dim_trace);
    put_i(path, "menu_dim_probe", g_vrcfg.menu_dim_probe);
    put_i(path, "key_sweep", g_vrcfg.key_sweep);
    put_i(path, "hud_ab", g_vrcfg.hud_ab);
    put_i(path, "dump_strings", g_vrcfg.dump_strings);
}

void vr_config_load(void) {
    char path[MAX_PATH], buf[64];
    int ver;
    ini_path(path, sizeof(path));

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        write_default_config(path);
        read_all(path);
    } else {
        GetPrivateProfileStringA("vr", "config_version", "0", buf,
                                 sizeof(buf), path);
        ver = atoi(buf);
        read_all(path);
        /* Values this port SHIPPED WRONG, corrected once -- and only where
           the file still holds the wrong value, so a player who chose
           something else keeps their choice.  engine_fov 100 and
           draw_distance 2 were performance dials spent on 20 Sep 2026 and
           they cull inside the eye: ground tiles vanish at the bottom of
           the view on a downward tilt.  fov_both_ends 0 is the F9 A/B
           toggle archived as though it were a preference. */
        if (ver < 73) {
            int fixed = 0;
            if (g_vrcfg.engine_fov == 100.0f) { g_vrcfg.engine_fov = 0.0f; fixed++; }
            if (g_vrcfg.draw_distance == 2.0f) { g_vrcfg.draw_distance = 4.0f; fixed++; }
            if (g_vrcfg.fov_both_ends == 0) { g_vrcfg.fov_both_ends = 1; fixed++; }
            if (fixed)
                kv_log("VR: corrected %d setting(s) this port shipped wrong -- "
                       "engine_fov 100 -> 0 (derive it from the headset; 100 "
                       "is NARROWER than a Quest 3 eye, so the engine culled "
                       "ground tiles inside your own field of view), "
                       "draw_distance 2 -> 4, fov_both_ends 0 -> 1. Your own "
                       "values are untouched.", fixed);
        }
        /* 4 was this port's own default and is more far plane than these
           maps use; 3 chosen in the headset, 20 Sep 2026.  Same rule as above: only
           where the file still holds the value we shipped. */
        if (ver < 74 && g_vrcfg.draw_distance == 4.0f) {
            g_vrcfg.draw_distance = 3.0f;
            kv_log("VR: draw_distance 4 -> 3 (the far plane, 8800 -> 6600 "
                   "game units). This is not the setting that culls the "
                   "bottom of your view; engine_fov is, and it stays on "
                   "automatic.");
        }
        /* The grip mouse (3) is 1.0's control scheme. 0 (right stick) and 2
           (menu pointer) were earlier defaults, and every ini from before
           1.0 is a pre-release test install: a test PC kept 0 through an
           uninstall and a fresh setup and had no grip mouse (25 Sep). */
        if (ver < 92 && g_vrcfg.pointer_mode >= 0 && g_vrcfg.pointer_mode <= 2) {
            kv_log("VR: pointer_mode %d -> 3, the grip mouse: hold the right grip "
                   "to point and click, as 1.0 ships. The old value was an "
                   "earlier build's default.", g_vrcfg.pointer_mode);
            g_vrcfg.pointer_mode = 3;
        }
        if (ver < KV_CONFIG_VERSION) {
            /* Rewrite for the new comments and any new keys, then put the
               user's own values straight back.  An earlier version of this
               replaced the file wholesale and threw away a setting a test
               depended on, twice. */
            kv_log("VR: keflings_vr.ini was written by build %d; this build is "
                   "%d. Refreshing the file and keeping your settings.",
                   ver, KV_CONFIG_VERSION);
            write_default_config(path);
            write_all(path);
            read_all(path);
        }
    }

    if (g_vrcfg.world_scale < 1.0f) g_vrcfg.world_scale = 1.0f;
    if (g_vrcfg.fov_scale < 0.0f) g_vrcfg.fov_scale = 0.0f;
    if (g_vrcfg.fov_scale > 1.0f) g_vrcfg.fov_scale = 1.0f;

    /* Print every value a test can depend on.  A setting that silently read
       back as its default has already cost two rounds on this port. */
    kv_log("VR: config enabled=%d stereo=%d screen_mode=%d world_scale=%.1f "
           "fov_scale=%s srgb=%d", g_vrcfg.enabled, g_vrcfg.stereo,
           g_vrcfg.screen_mode, g_vrcfg.world_scale,
           g_vrcfg.fov_scale > 0.0f ? "manual" : "auto (fit engine FOV)",
           g_vrcfg.srgb_swapchain);
    kv_log("VR: config engine_fov=%.1f%s force_vsync=%d hud_distance=%.2f "
           "dump_strings=%d", g_vrcfg.engine_fov,
           g_vrcfg.engine_fov > 0.0f ? " (will patch the engine camera)"
                                     : " (engine left untouched)",
           g_vrcfg.force_vsync, g_vrcfg.hud_distance, g_vrcfg.dump_strings);
}

/* ---- small matrix helpers ---------------------------------------------- */
/* All matrices are column-major, m[col*4 + row], the layout GL wants. */

static void mat_mul(const float *a, const float *b, float *out) {
    int c, r, k;
    float t[16];
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    memcpy(out, t, sizeof(t));
}

/* An asymmetric frustum from OpenXR's four signed half-angles.  Scaling all
   four tangents keeps the optical centre where the runtime put it, which
   matters: an eye frustum's axis is not the centre of its image. */
static void mat_frustum(const XrFovf *fov, float sx, float sy, double zn,
                        double zf, float *m) {
    float tl = (float)tan(fov->angleLeft) * sx;
    float tr = (float)tan(fov->angleRight) * sx;
    float tu = (float)tan(fov->angleUp) * sy;
    float td = (float)tan(fov->angleDown) * sy;
    float w = tr - tl, h = tu - td;
    float n = (float)zn, f = (float)zf;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tr + tl) / w;
    m[9]  = (tu + td) / h;
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
}

/* The inverse of a pose, as a view matrix: R^-1 then -p, with the position
   converted from real metres into the game's world units. */
static void mat_pose_inverse(const XrPosef *p, float scale, float *m) {
    float x = p->orientation.x, y = p->orientation.y;
    float z = p->orientation.z, w = p->orientation.w;
    float px = p->position.x * scale;
    float py = p->position.y * scale;
    float pz = p->position.z * scale;
    float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    memset(m, 0, 16 * sizeof(float));
    /* R transposed, written column-major */
    m[0] = r00; m[4] = r10; m[8]  = r20;
    m[1] = r01; m[5] = r11; m[9]  = r21;
    m[2] = r02; m[6] = r12; m[10] = r22;
    /* translation = -R^T * p */
    m[12] = -(r00 * px + r10 * py + r20 * pz);
    m[13] = -(r01 * px + r11 * py + r21 * pz);
    m[14] = -(r02 * px + r12 * py + r22 * pz);
    m[15] = 1.0f;
}

/* ---- GL helpers -------------------------------------------------------- */
static int bind_gl_ext(void) {
    PROC (WINAPI *gpa)(LPCSTR) =
        (PROC (WINAPI *)(LPCSTR))proxy_real_proc("wglGetProcAddress");
    if (!gpa) return 0;
    p_glReadBuffer = (void *)proxy_real_proc("glReadBuffer");
    p_wglGetCurrentContext = (void *)proxy_real_proc("wglGetCurrentContext");
    p_glDisable = (void *)proxy_real_proc("glDisable");
    p_glEnable = (void *)proxy_real_proc("glEnable");
    p_glIsEnabled = (void *)proxy_real_proc("glIsEnabled");
    p_glDrawBuffer = (void *)proxy_real_proc("glDrawBuffer");
    p_glGenTextures = (void *)proxy_real_proc("glGenTextures");
    p_glDeleteTextures = (void *)proxy_real_proc("glDeleteTextures");
    p_glBindTexture = (void *)proxy_real_proc("glBindTexture");
    p_glTexImage2D = (void *)proxy_real_proc("glTexImage2D");
    p_glTexParameteri = (void *)proxy_real_proc("glTexParameteri");
    p_glColorMask = (void *)proxy_real_proc("glColorMask");
    p_glClearColor = (void *)proxy_real_proc("glClearColor");
    p_glClear = (void *)proxy_real_proc("glClear");
    p_glGetIntegerv = (void *)proxy_real_proc("glGetIntegerv");
    p_glReadPixels = (void *)proxy_real_proc("glReadPixels");
    p_glGetError = (void *)proxy_real_proc("glGetError");
    p_glViewport = (void *)proxy_real_proc("glViewport");
    if (!p_glReadBuffer || !p_wglGetCurrentContext || !p_glDisable ||
        !p_glEnable || !p_glIsEnabled) {
        kv_log("VR: could not resolve base GL entry points");
        return 0;
    }
#define GET(v, n) v = (void *)gpa(n); if (!v) { kv_log("VR: missing %s", n); return 0; }
    GET(p_glGenFramebuffers, "glGenFramebuffers");
    GET(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    GET(p_glBindFramebuffer, "glBindFramebuffer");
    GET(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    GET(p_glBlitFramebuffer, "glBlitFramebuffer");
    GET(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    GET(p_glGenRenderbuffers, "glGenRenderbuffers");
    GET(p_glBindRenderbuffer, "glBindRenderbuffer");
    GET(p_glRenderbufferStorage, "glRenderbufferStorage");
    GET(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
#undef GET
    /* Optional: the port works without timing, so these are not fatal. */
    p_glGenQueries = (void *)gpa("glGenQueries");
    p_glBeginQuery = (void *)gpa("glBeginQuery");
    p_glEndQuery = (void *)gpa("glEndQuery");
    p_glGetQueryObjectuiv = (void *)gpa("glGetQueryObjectuiv");
    p_glGetQueryObjectui64v = (void *)gpa("glGetQueryObjectui64v");
    return 1;
}

static void client_size(HDC hdc, int *w, int *h) {
    RECT rc;
    HWND wnd = WindowFromDC(hdc);
    *w = 0; *h = 0;
    if (wnd && GetClientRect(wnd, &rc)) {
        *w = rc.right - rc.left;
        *h = rc.bottom - rc.top;
    }
}

/* Copy a rectangle of the back buffer into a swapchain image.  1:1 and with
   sRGB conversion suppressed: a scaling blit out of a possibly multisampled
   default framebuffer is invalid and is rejected silently every frame, and the
   source bytes are already sRGB-encoded. */
static void blit_to_swapchain(GLuint tex, int sx, int sy, int sw, int sh,
                              int dw, int dh) {
    GLboolean srgb_was;
    /* Read from wherever the scene actually was drawn. */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    p_glReadBuffer(g_scene_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, tex, 0);
    if (p_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        static int said;
        if (!said) { kv_log("VR: swapchain framebuffer incomplete"); said = 1; }
    } else {
        srgb_was = p_glIsEnabled(GL_FRAMEBUFFER_SRGB);
        if (srgb_was) p_glDisable(GL_FRAMEBUFFER_SRGB);
        p_glBlitFramebuffer(sx, sy, sx + sw, sy + sh, 0, 0, dw, dh,
                            GL_COLOR_BUFFER_BIT,
                            (sw == dw && sh == dh) ? GL_NEAREST : GL_LINEAR);
        if (srgb_was) p_glEnable(GL_FRAMEBUFFER_SRGB);
    }
    /* Put the engine's target back, not the window: the next thing to draw
       is the engine, and it has no idea any of this happened. */
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene_fbo);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
}

/* Somewhere for the engine to draw that is not the window.  Colour plus a
   depth-stencil buffer, because the default framebuffer had both and the
   engine depth-tests everything.  Returns 0 and leaves g_scene_fbo at 0 on any
   failure, which puts the whole port straight back on the back buffer. */
static int make_scene_target(int w, int h) {   /* declared above */
    GLint was = 0;
    if (!p_glGenRenderbuffers || !p_glGenTextures) return 0;
    p_glGenFramebuffers(1, &g_scene_fbo);
    p_glGenTextures(1, &g_scene_tex);
    p_glGenRenderbuffers(1, &g_scene_depth);
    if (!g_scene_fbo || !g_scene_tex || !g_scene_depth) { g_scene_fbo = 0; return 0; }

    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
    p_glBindTexture(GL_TEXTURE_2D, g_scene_tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
    p_glBindTexture(GL_TEXTURE_2D, (GLuint)was);

    p_glBindRenderbuffer(GL_RENDERBUFFER, g_scene_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);

    p_glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_scene_tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, g_scene_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        kv_log("VR: the %dx%d scene target is incomplete; falling back to the "
               "game's back buffer, which means the window sets the resolution",
               w, h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_scene_fbo = 0;
        return 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_scene_w = w;
    g_scene_h = h;
    kv_log("VR: scene target %dx%d -- the eyes are cut from THIS now, not "
           "from the game window, so the desktop no longer sets the VR "
           "resolution", w, h);
    return 1;
}


/* ---- the 2D panel, rendered once ------------------------------------- */
/* The panel is a flat image and is IDENTICAL in both eyes -- only the
   projection that places it differs. Drawing it per eye therefore pays twice
   for one picture, at eye resolution rather than canvas resolution, and on a
   menu that is ~2100 alpha-blended quads over 20 megapixels twice.

   Render it once here instead, then draw this texture as a quad per eye. */
static GLuint g_panel_fbo, g_panel_tex, g_panel_depth;
static int    g_panel_w, g_panel_h;
static GLint  g_panel_prev_fbo;
static int    g_panel_bound;

static void destroy_panel_target(void) {
    if (g_panel_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_panel_fbo);
    if (g_panel_tex && p_glDeleteTextures) p_glDeleteTextures(1, &g_panel_tex);
    /* No glDeleteRenderbuffers is resolved anywhere in this file and the
       panel is recreated at most once or twice in a session, so the buffer is
       left to the context teardown rather than importing a call for it. */
    g_panel_fbo = g_panel_tex = g_panel_depth = 0;
    g_panel_w = g_panel_h = 0;
}

static int make_panel_target(int w, int h) {
    GLint was = 0;
    if (!p_glGenFramebuffers || !p_glGenTextures || !p_glGenRenderbuffers)
        return 0;
    destroy_panel_target();
    p_glGenFramebuffers(1, &g_panel_fbo);
    p_glGenTextures(1, &g_panel_tex);
    p_glGenRenderbuffers(1, &g_panel_depth);
    if (!g_panel_fbo || !g_panel_tex || !g_panel_depth) {
        destroy_panel_target();
        return 0;
    }

    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
    p_glBindTexture(GL_TEXTURE_2D, g_panel_tex);
    /* LINEAR: the panel is resampled into the eye, and the eye is larger than
       the canvas, so this is a magnify. */
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
    p_glBindTexture(GL_TEXTURE_2D, (GLuint)was);

    /* The 2D pass may depth-test against itself. Cheap to provide and it
       removes a whole class of "one element is missing" question. */
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_panel_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);

    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_panel_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_panel_tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, g_panel_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        kv_log("VR: the %dx%d panel target is incomplete; the 2D pass stays "
               "per-eye, which is correct but costs a menu about 7 ms", w, h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
        destroy_panel_target();
        return 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
    g_panel_w = w;
    g_panel_h = h;
    kv_log("VR: 2D panel target %dx%d -- the menu is drawn once here instead "
           "of twice at %dx%d per eye", w, h, g_eyew, g_eyeh);
    return 1;
}

/* Begin capturing the 2D pass. Returns 0 if the caller should carry on
   drawing per eye as before -- every failure path must leave the game
   working, just slower. */
int vr_panel_begin(int w, int h, int clear) {
    /* The flat diagnostic needs the target without a session: the capture and
       the blend are what it is testing, and neither involves VR. */
    if ((!g_running || !g_stereo) && !g_vrcfg.panel_flat_test) return 0;
    if (w < 16 || h < 16) return 0;
    if (w != g_panel_w || h != g_panel_h) {
        if (!make_panel_target(w, h)) return 0;
    }
    if (g_panel_bound) return 1;
    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g_panel_prev_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_panel_fbo);
    if (p_glViewport) p_glViewport(0, 0, g_panel_w, g_panel_h);
    /* Only on the FRAME's first capture. A frame's 2D content is spread over
       several passes with scene passes between them, so later passes rebind
       and accumulate -- clearing on each one left the panel holding whatever
       the last pass happened to draw, which was the parchment alone. */
    if (clear) {
        /* Transparent, so everything the engine does not draw shows the world
           through it. A clear obeys the colour mask and the scissor test, and
           the 2D pass is exactly where the engine leaves both set. */
        p_glDisable(GL_SCISSOR_TEST);
        p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    g_panel_bound = 1;
    return 1;
}

void vr_panel_end(void) {
    if (!g_panel_bound) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_panel_prev_fbo);
    g_panel_bound = 0;
}

/* Read one pixel straight out of the panel and say what is stored there.
   Inferring the alpha from the final image got me 0.43 where the theory
   said 0.24 or 0.49, which means the theory is wrong -- so stop inferring
   and read the number. Called a handful of times, not every frame: a
   readback stalls the pipeline. */
void vr_panel_probe(float fx, float fy) {
    /* A GRID, not a point. The single sample read alpha 0 and I could not
       tell whether that meant the backdrop is absent from the panel or that
       I had computed the wrong pixel -- the harness samples a desktop
       screenshot while this reads canvas coordinates, and the canvas is
       2560x1440 inside a 1280x720 window, so the two do not correspond.
       A map of the whole canvas cannot be aimed wrongly. */
    unsigned char px[4];
    GLint was = 0;
    int ix, iy;
    char row[64];
    (void)fx; (void)fy;
    if (!g_panel_fbo || !p_glReadPixels) return;
    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_panel_fbo);
    p_glReadBuffer(GL_COLOR_ATTACHMENT0);
    kv_log("PANEL PROBE: alpha across the %dx%d canvas, 0-9 (top row is the"
           " top of the screen). All zeros means the 2D pass being captured"
           " does not contain the game's backdrop at all.",
           g_panel_w, g_panel_h);
    for (iy = 0; iy < 9; iy++) {
        int n = 0;
        for (ix = 0; ix < 9; ix++) {
            int x = (int)((ix + 0.5) * g_panel_w / 9.0);
            int y = (int)((8 - iy + 0.5) * g_panel_h / 9.0);
            p_glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            row[n++] = (char)('0' + (px[3] * 9) / 255);
            row[n++] = ' ';
        }
        row[n] = 0;
        kv_log("  %s", row);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
}

int vr_panel_tex(void) { return (int)g_panel_tex; }
int vr_panel_is_bound(void) { return g_panel_bound; }
void vr_panel_size(int *w, int *h) { *w = g_panel_w; *h = g_panel_h; }

/* Did the engine actually draw into our target?  Everything upstream of this
   reports success whether it did or not, and the only symptom is a black
   headset.  Sampled twice because the first frames are a loading screen and
   are black for good reasons. */
void vr_check_scene_target(unsigned frame) {
    static int checks, empty;
    unsigned char px[64 * 64 * 4];
    int i, nonzero = 0;
    if (!g_scene_fbo || !p_glReadPixels || checks >= 2) return;
    if (frame != 150 && frame != 600) return;
    checks++;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    p_glReadBuffer(GL_COLOR_ATTACHMENT0);
    p_glReadPixels(g_scene_w / 4 - 32, g_scene_h / 2 - 32, 64, 64,
                   GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < 64 * 64 * 4; i += 4)
        if (px[i] | px[i + 1] | px[i + 2]) nonzero++;
    kv_log("SCENE TARGET: sample %d of the left eye -- %d of 4096 pixels have "
           "colour", checks, nonzero);
    if (!nonzero) empty++;
    if (checks == 2 && empty == 2)
        kv_log("SCENE TARGET: nothing has been drawn into it in two samples. "
               "The engine is rendering somewhere else and the headset will be "
               "black. Set offscreen=0 in keflings_vr.ini to go back to the "
               "back buffer.");
    else if (checks == 2)
        kv_log("SCENE TARGET: the engine is drawing into it, so the eyes are "
               "%dx%d rather than halves of the window.", g_eyew, g_eyeh);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    p_glReadBuffer(GL_COLOR_ATTACHMENT0);
}

/* Time the engine's frame on the GPU's own clock.  A CPU measurement cannot
   tell a busy CPU from a CPU waiting for the GPU, and that is exactly the
   question. */
void vr_gpu_frame_begin(void) {
    if (!p_glGenQueries || !p_glBeginQuery) return;
    if (!g_gpu_q[0]) {
        p_glGenQueries(2, g_gpu_q);
        if (!g_gpu_q[0] || !g_gpu_q[1]) { p_glGenQueries = NULL; return; }
    }
    p_glBeginQuery(GL_TIME_ELAPSED, g_gpu_q[g_gpu_slot]);
    g_gpu_open = 1;
}

void vr_gpu_frame_end(void) {
    int other;
    GLuint ready = 0;
    unsigned __int64 ns = 0;
    if (!g_gpu_open || !p_glEndQuery) return;
    p_glEndQuery(GL_TIME_ELAPSED);
    g_gpu_open = 0;

    /* Read the one we filled LAST frame, and only if it is already done.
       Asking for the query we just ended would block until the GPU drains --
       measuring the pipeline by stalling the pipeline. */
    other = !g_gpu_slot;
    if (g_gpu_armed && p_glGetQueryObjectuiv && p_glGetQueryObjectui64v) {
        p_glGetQueryObjectuiv(g_gpu_q[other], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready) {
            p_glGetQueryObjectui64v(g_gpu_q[other], GL_QUERY_RESULT, &ns);
            g_gpu_sum += (double)ns / 1000000.0;
            g_gpu_n++;
        }
    }
    g_gpu_slot = other;
    g_gpu_armed = 1;
}

double vr_gpu_ms(void) {
    double v = g_gpu_n ? g_gpu_sum / (double)g_gpu_n : -1.0;
    g_gpu_sum = 0.0;
    g_gpu_n = 0;
    return v;
}

unsigned vr_scene_fbo(void) { return (unsigned)g_scene_fbo; }

/* GL_BACK is not a legal buffer on a framebuffer object, and the engine asks
   for it by name.  Translate while ours is bound; pass everything else
   through. */
unsigned vr_map_buffer(unsigned buf) {
    if (g_scene_fbo && (buf == GL_BACK || buf == GL_FRONT ||
                        buf == GL_BACK_LEFT || buf == GL_FRONT_LEFT))
        return GL_COLOR_ATTACHMENT0;
    return buf;
}

/* Called once a frame, after the window has been presented: everything the
   engine draws from here lands in our target instead of the window. */
void vr_bind_scene_target(void) {
    if (!g_scene_fbo) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    if (p_glDrawBuffer) p_glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (p_glReadBuffer) p_glReadBuffer(GL_COLOR_ATTACHMENT0);
}

/* The desktop window IS the game's own back buffer, and the two eyes were
   rendered side by side into it -- so presenting it raw shows a squashed
   stereo pair.  Copy the left eye out and present that instead.

   This runs AFTER vr_finish_frame, so both eyes have already reached their
   swapchains and the back buffer is ours to overwrite.  It runs only on the
   frames the mirror is actually presented (30 a second, not 90), so the two
   blits cost about a fiftieth of a frame.

   Note the shapes of the two blits.  The capture is 1:1 out of the default
   framebuffer, because a SCALING blit out of a possibly multisampled default
   framebuffer is invalid and is rejected silently every frame.  The scale
   happens on the way back out, where the source is our own texture. */
void vr_mirror_present(HDC hdc) {
    int gw, gh, dx, dy, dw, dh, sx, sy, sw, sh;
    GLboolean srgb_was, scissor_was;

    if (!g_stereo || !g_fbo || g_eyew <= 0 || g_eyeh <= 0) return;
    /* "present the back buffer untouched" only means anything while the
       engine still draws there. */
    if (g_vrcfg.mirror_mode >= 2 && !g_scene_fbo) return;
    if (!p_glDrawBuffer || !p_glGenTextures || !p_glBindTexture ||
        !p_glTexImage2D || !p_glTexParameteri || !p_glColorMask ||
        !p_glClearColor || !p_glClear || !p_glGetIntegerv) {
        static int said;
        if (!said) {
            said = 1;
            kv_log("mirror: base GL entry points missing, leaving the desktop "
                   "window as the raw side-by-side pair");
        }
        return;
    }

    client_size(hdc, &gw, &gh);
    if (gw <= 0 || gh <= 0) return;
    /* The game makes a window a little larger than the desktop, and the part
       hanging off it is not a mirror of anything.  Named scrw/scrh rather than
       sw/sh: those already mean the SOURCE rectangle a few lines down, and two
       pairs of nearly identical names around a blit is how a mirror ends up
       showing the wrong part of the eye. */
    {
        int scrw = GetSystemMetrics(SM_CXSCREEN);
        int scrh = GetSystemMetrics(SM_CYSCREEN);
        if (scrw > 0 && scrw < gw) gw = scrw;
        if (scrh > 0 && scrh < gh) gh = scrh;
    }

    /* Sized to the destination, so the blit that touches the window is always
       1:1 and multisampling has nothing to object to. */
    if (g_mirror_tex && (g_mirror_w != gw || g_mirror_h != gh)) {
        p_glDeleteTextures(1, &g_mirror_tex);
        g_mirror_tex = 0;
    }
    if (!g_mirror_tex) {
        GLint was = 0;
        p_glGenTextures(1, &g_mirror_tex);
        if (!g_mirror_tex) return;
        p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
        p_glBindTexture(GL_TEXTURE_2D, g_mirror_tex);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gw, gh, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glBindTexture(GL_TEXTURE_2D, (GLuint)was);
        g_mirror_w = gw;
        g_mirror_h = gh;
        kv_log("mirror: left eye %dx%d -> %dx%d window, through a scratch "
               "texture because the window is multisampled and will not accept "
               "a scaling blit", g_eyew, g_eyeh, gw, gh);
    }



    /* mode 1 keeps the whole eye and leaves bars.  mode 0 -- the default --
       takes the largest rectangle of the WINDOW's shape out of the middle of
       the eye and fills the window with it.  That costs the top and bottom of
       the eye's view, which is the trade every headset mirror makes and is why
       they look like an ordinary game instead of a letterboxed one. */
    /* The scene target holds the eyes side by side; the right eye starts
       one eye-width in. */
    sx = (g_vrcfg.mirror_eye > 0 && g_nview > 1) ? g_scenew : 0;
    sy = 0; sw = g_scenew; sh = g_sceneh;
    dx = 0; dy = 0; dw = gw; dh = gh;
    /* SCENE dimensions throughout: sx/sy/sw/sh is a rectangle inside the
       scene target, which render_scale can make smaller than the swapchain.
       Clamping it against the swapchain instead would read past the edge of
       what was drawn. */
    if (g_vrcfg.mirror_mode == 1) {
        float k = (float)gw / (float)g_scenew;
        float ky = (float)gh / (float)g_sceneh;
        if (ky < k) k = ky;
        dw = (int)(g_scenew * k + 0.5f);
        dh = (int)(g_sceneh * k + 0.5f);
        dx = (gw - dw) / 2;
        dy = (gh - dh) / 2;
    } else {
        /* crop the source to the window's aspect */
        if ((double)g_scenew / g_sceneh > (double)gw / gh) {
            sw = (int)((double)g_sceneh * gw / gh + 0.5);
            if (sw > g_scenew) sw = g_scenew;
        } else {
            sh = (int)((double)g_scenew * gh / gw + 0.5);
            if (sh > g_sceneh) sh = g_sceneh;
        }
        sx += (g_scenew - sw) / 2;
        sy = (g_sceneh - sh) / 2;
    }

    /* A pass must set its own state.  The engine leaves scissor and colour
       mask however its last draw wanted them, and a clear obeys both -- so a
       clear written without these is silently suppressed and the bars keep
       last frame's half of the stereo pair. */
    scissor_was = p_glIsEnabled(GL_SCISSOR_TEST);
    if (scissor_was) p_glDisable(GL_SCISSOR_TEST);
    srgb_was = p_glIsEnabled(GL_FRAMEBUFFER_SRGB);
    if (srgb_was) p_glDisable(GL_FRAMEBUFFER_SRGB);
    /* First hop: whatever the scene was drawn into, scaled down into our own
       texture.  Both single-sampled, so the scale is legal. */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    p_glReadBuffer(g_scene_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_mirror_tex, 0);
    p_glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (p_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        p_glBlitFramebuffer(sx, sy, sx + sw, sy + sh, dx, dy, dx + dw, dy + dh,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR);

    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glDrawBuffer(GL_BACK);
    /* Unconditionally, and with the colour mask forced open: the engine leaves
       both the mask and the scissor however its last draw wanted them, and a
       clear obeys both, so a clear written without this is silently suppressed
       and any part of the window the blit does not cover keeps a stale frame.

       This was briefly a distinctive blue, as a positive control while the blit
       was being refused -- if the window had come back blue the clear was
       landing and the blit was the fault. It came back black, which said
       neither was reaching the window, and the error code then named it. */
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    /* Second hop: our texture onto the window, same size both sides. */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_mirror_tex, 0);
    p_glReadBuffer(GL_COLOR_ATTACHMENT0);
    {
        static int said;
        GLenum st = p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        if (p_glGetError) while (p_glGetError() != GL_NO_ERROR) {}
        if (st == GL_FRAMEBUFFER_COMPLETE)
            p_glBlitFramebuffer(0, 0, gw, gh, 0, 0, gw, gh,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (!said) {
            GLenum err = p_glGetError ? p_glGetError() : 0;
            GLint sb = 0, sm = 0;
            said = 1;
            if (p_glGetIntegerv) {
                p_glGetIntegerv(GL_SAMPLE_BUFFERS, &sb);
                p_glGetIntegerv(GL_SAMPLES, &sm);
            }
            kv_log("mirror: eye rect %d,%d %dx%d -> %dx%d texture -> the "
                   "window 1:1 | error 0x%X | the window has %d sample "
                   "buffers, %d samples (non-zero is why a scaling blit "
                   "straight into it was refused)",
                   sx, sy, sw, sh, gw, gh, (unsigned)err, (int)sb, (int)sm);
        }
    }
    /* Back to the engine's target, not to the window. */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    p_glReadBuffer(g_scene_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene_fbo);
    if (g_scene_fbo && p_glDrawBuffer) p_glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (srgb_was) p_glEnable(GL_FRAMEBUFFER_SRGB);
    if (scissor_was) p_glEnable(GL_SCISSOR_TEST);
}

/* ---- bring-up ---------------------------------------------------------- */
/* Pull "library_path" out of a runtime or layer manifest.  A hand-rolled
   scan rather than a json parser: the file is small, the key is unique, and a
   dependency for one string would be worse than the string. */
/* Pull a string value out of a runtime or layer manifest.  A hand-rolled scan
   rather than a json parser: the files are small, the keys are unique, and a
   dependency for two strings would be worse than the strings. */
static int json_string_value(const char *json, const char *key,
                             char *out, size_t outsz) {
    FILE *f = fopen(json, "rb");
    char buf[4096], *p, *q, *dst;
    size_t n, klen = strlen(key);
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p + klen, ':');
    if (!p) return 0;
    while (*p && *p != '"') p++;
    if (!*p) return 0;
    p++;
    dst = out;
    q = p;
    while (*q && *q != '"' && (size_t)(dst - out) < outsz - 1) {
        if (*q == '\\' && q[1] == '\\') q++;   /* json escapes the separator */
        *dst++ = *q++;
    }
    *dst = 0;
    return out[0] != 0;
}

static int json_library_path(const char *json, char *out, size_t outsz) {
    return json_string_value(json, "library_path", out, outsz);
}

#if 0
static int json_library_path_unused(const char *json, char *out, size_t outsz) {
    FILE *f = fopen(json, "rb");
    char buf[4096], *p, *q, *dst;
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    p = strstr(buf, "library_path");
    if (!p) return 0;
    p = strchr(p + 12, ':');
    if (!p) return 0;
    while (*p && *p != '"') p++;
    if (!*p) return 0;
    p++;
    dst = out;
    q = p;
    while (*q && *q != '"' && (size_t)(dst - out) < outsz - 1) {
        if (*q == '\\' && q[1] == '\\') q++;   /* json escapes the separator */
        *dst++ = *q++;
    }
    *dst = 0;
    return out[0] != 0;
}
#endif

/* Resolve a manifest-relative library path against the manifest's folder, the
   way the loader does. */
static void resolve_beside(const char *json, const char *lib,
                           char *out, size_t outsz) {
    const char *slash;
    if (lib[0] && (lib[1] == ':' || lib[0] == '\\')) {
        lstrcpynA(out, lib, (int)outsz);
        return;
    }
    slash = strrchr(json, '\\');
    if (!slash) { lstrcpynA(out, lib, (int)outsz); return; }
    /* The manifest usually writes ".\\name".  Drop the "./" rather than
       printing a path with a stray "\\.\\" in the middle of it -- a path a
       reader cannot paste into Explorer is a path they cannot check. */
    if (lib[0] == '.' && lib[1] == '\\') lib += 2;
    _snprintf(out, outsz, "%.*s\\%s", (int)(slash - json), json, lib);
}

/* Say what a Windows error MEANS, for the handful that actually come back from
   LoadLibrary on a runtime dll.  A bare number in a log sent from a machine I
   cannot log into is another round trip and another evening. */
static const char *load_error_meaning(DWORD e) {
    switch (e) {
    case 126:  return "ERROR_MOD_NOT_FOUND -- the dll is there, but something "
                      "IT needs is not. For a 32-bit runtime dll this is "
                      "almost always the 32-bit (x86) Visual C++ "
                      "redistributable missing on this machine; installing "
                      "vc_redist.x86.exe fixes it";
    case 193:  return "ERROR_BAD_EXE_FORMAT -- wrong bitness. This is a 32-bit "
                      "game and it has been handed a 64-bit runtime dll";
    case 5:    return "ERROR_ACCESS_DENIED -- the file cannot be read. "
                      "Antivirus, or permissions on that folder";
    case 2:    return "ERROR_FILE_NOT_FOUND";
    case 1114: return "ERROR_DLL_INIT_FAILED -- it loaded, and its own startup "
                      "code failed. Usually the headset software is not "
                      "running";
    default:   return "look this code up in the Windows system error list";
    }
}

/* Do what the loader does, and report what happened.  Loading the runtime dll
   into this process is safe here: bring-up has already failed, nothing is
   using it, and it is freed again immediately. */
static void probe_library(const char *full) {
    HMODULE m;
    DWORD e;
    long bytes;
    FILE *f = fopen(full, "rb");
    if (!f) {
        kv_log("      CANNOT OPEN IT FOR READING (errno %d)  <<< this is the "
               "file access failure", errno);
        return;
    }
    fseek(f, 0, SEEK_END);
    bytes = ftell(f);
    fclose(f);
    SetLastError(0);
    m = LoadLibraryA(full);
    if (m) {
        kv_log("      %ld bytes, loads cleanly -- this dll is NOT the problem",
               bytes);
        FreeLibrary(m);
        return;
    }
    e = GetLastError();
    kv_log("      %ld bytes, readable, but LoadLibrary FAILED with error "
           "%lu: %s", bytes, e, load_error_meaning(e));
    kv_log("      <<< THIS is what the OpenXR loader hit. It reports it as "
           "XR_ERROR_FILE_ACCESS_ERROR without saying which file.");
}

static void report_one_manifest(const char *what, const char *json,
                                int ours) {
    char lib[MAX_PATH], full[MAX_PATH];
    if (GetFileAttributesA(json) == INVALID_FILE_ATTRIBUTES) {
        kv_log("  %s manifest MISSING: %s   <<< the registry points at a file "
               "that is not there; deleting that registry value fixes it",
               what, json);
        return;
    }
    if (!json_library_path(json, lib, sizeof(lib))) {
        kv_log("  %s manifest present but has no readable library_path: %s",
               what, json);
        return;
    }
    resolve_beside(json, lib, full, sizeof(full));
    kv_log("%s %s", what, json);
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        kv_log("      -> %s  <<< THIS DLL IS MISSING", full);
        return;
    }
    kv_log("      -> %s", full);
    /* Only probe what this process could load.  A 32-bit program loading a
       64-bit dll always fails with error 193; reporting that on a healthy
       machine buries the one line that matters. */
    if (!ours) {
        kv_log("      (64-bit; a 32-bit game cannot use it, and never tries)");
        return;
    }
    probe_library(full);
}

/* Every file the loader has to open, from the registry, each one checked.
   Only after a failure. */
static int process_is_elevated(void);

static void report_xr_environment(void) {
    /* The same key path in both registry views.  A 32-bit process reading
       HKLM/SOFTWARE/Khronos is silently redirected into WOW6432Node, which
       is why the first version of this report printed the 32-bit runtime
       twice and called one of them the 64-bit one.  Ask for each view by
       name. */
    static const char *ROOT = "SOFTWARE\\Khronos\\OpenXR\\1";
    static const REGSAM views[2] = { KEY_WOW64_32KEY, KEY_WOW64_64KEY };
    static const char *names[2] = { "32-bit", "64-bit" };
    const char *env;
    int r;

    kv_log("XR ENVIRONMENT: the loader returns XR_ERROR_FILE_ACCESS_ERROR for "
           "one of two reasons -- it found no runtime manifest, or "
           "LoadLibrary failed on a dll it did find. Below is each file it "
           "would open, actually opened.");
    kv_log("  (this game is 32-bit, so only the 32-bit entries matter; the "
           "64-bit ones are listed for comparison)");
    kv_log("  running as administrator: %s",
           process_is_elevated() ? "YES" : "no");
    if (process_is_elevated())
        kv_log("    <<< an elevated game makes the loader IGNORE "
               "XR_RUNTIME_JSON, so the other runtimes tried above were not "
               "really tried -- each one re-ran the same registry entry. "
               "Right-click the exe, Properties, Compatibility, and untick "
               "\"Run this program as an administrator\".");

    env = getenv("XR_RUNTIME_JSON");
    if (env && env[0]) {
        kv_log("  XR_RUNTIME_JSON is set and OVERRIDES the registry: %s", env);
        report_one_manifest("   override runtime", env, 1);
    }

    for (r = 0; r < 2; r++) {
        HKEY k;
        char sub[512];
        LONG rc;

        /* The per-user hive first, because the loader reads it too and a
           stale entry there outranks a healthy machine-wide install. */
        rc = RegOpenKeyExA(HKEY_CURRENT_USER, ROOT, 0,
                           KEY_READ | views[r], &k);
        if (rc == ERROR_SUCCESS) {
            char v[MAX_PATH];
            DWORD sz = sizeof(v), type = 0;
            if (RegQueryValueExA(k, "ActiveRuntime", NULL, &type,
                                 (LPBYTE)v, &sz) == ERROR_SUCCESS) {
                v[sz < sizeof(v) ? sz : sizeof(v) - 1] = 0;
                kv_log("  %s ActiveRuntime for THIS USER (outranks the "
                       "machine-wide one):", names[r]);
                report_one_manifest("   ", v, r == 0);
            }
            RegCloseKey(k);
        }

        rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, ROOT, 0,
                           KEY_READ | views[r], &k);
        if (rc != ERROR_SUCCESS) {
            kv_log("  %s: no OpenXR registration at all (error %ld)",
                   names[r], rc);
        } else {
            char v[MAX_PATH];
            DWORD sz = sizeof(v), type = 0;
            if (RegQueryValueExA(k, "ActiveRuntime", NULL, &type,
                                 (LPBYTE)v, &sz) == ERROR_SUCCESS) {
                v[sz < sizeof(v) ? sz : sizeof(v) - 1] = 0;
                kv_log("  %s ActiveRuntime:", names[r]);
                report_one_manifest("   ", v, r == 0);
            } else {
                kv_log("  %s: registered, but no ActiveRuntime value -- no "
                       "runtime is selected for this bitness", names[r]);
            }
            RegCloseKey(k);
        }

        /* Implicit API layers: overlays, capture tools, ReShade.  An implicit
           layer that fails to load fails xrCreateInstance with the same
           error, and a registry entry outliving its uninstall is the classic
           cause.  Print the empty case too -- a silent section is not
           evidence of an empty one. */
        /* Implicit layers registered for this user only, same reasoning. */
        _snprintf(sub, sizeof(sub), "%s\\ApiLayers\\Implicit", ROOT);
        rc = RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0,
                           KEY_READ | views[r], &k);
        if (rc == ERROR_SUCCESS) {
            DWORD i = 0;
            for (;;) {
                char name[MAX_PATH];
                DWORD nsz = sizeof(name), type = 0, data = 0;
                DWORD dsz = sizeof(data);
                if (RegEnumValueA(k, i++, name, &nsz, NULL, &type,
                                  (LPBYTE)&data, &dsz) != ERROR_SUCCESS) break;
                kv_log("  %s implicit API layer for THIS USER, %s:", names[r],
                       data ? "disabled" : "ENABLED");
                if (!data) report_one_manifest("   ", name, r == 0);
            }
            RegCloseKey(k);
        }

        _snprintf(sub, sizeof(sub), "%s\\ApiLayers\\Implicit", ROOT);
        rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub, 0,
                           KEY_READ | views[r], &k);
        if (rc != ERROR_SUCCESS) {
            kv_log("  %s implicit API layers: none registered", names[r]);
        } else {
            DWORD i = 0, found = 0;
            for (;;) {
                char name[MAX_PATH];
                DWORD nsz = sizeof(name), type = 0, data = 0;
                DWORD dsz = sizeof(data);
                if (RegEnumValueA(k, i++, name, &nsz, NULL, &type,
                                  (LPBYTE)&data, &dsz) != ERROR_SUCCESS) break;
                found++;
                /* value data 0 means enabled, non-zero means disabled */
                if (data) {
                    kv_log("  %s implicit API layer, disabled: %s",
                           names[r], name);
                } else {
                    kv_log("  %s implicit API layer, ENABLED -- this one is "
                           "loaded into the game:", names[r]);
                    report_one_manifest("   ", name, r == 0);
                }
            }
            if (!found)
                kv_log("  %s implicit API layers: none registered", names[r]);
            RegCloseKey(k);
        }
    }
}

static int load_loader(void) {
    char path[MAX_PATH], *p;
    GetModuleFileNameA(g_self, path, MAX_PATH);
    p = strrchr(path, '\\');
    if (p) p[1] = 0;
    lstrcatA(path, "openxr_loader.dll");
    g_loader = LoadLibraryA(path);
    if (!g_loader) {
        kv_log("VR: openxr_loader.dll not found beside the game (%s)", path);
        return 0;
    }
    xrGIPA = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader,
                                                       "xrGetInstanceProcAddr");
    if (!xrGIPA) { kv_log("VR: loader has no xrGetInstanceProcAddr"); return 0; }
    return 1;
}

static int bind_xr(XrInstance inst) {
#define GET(n)                                                               \
    if (XR_FAILED(xrGIPA(inst, #n, (PFN_xrVoidFunction *)&f_##n)) || !f_##n) {\
        kv_log("VR: missing entry point %s", #n); return 0; }
    GET(xrCreateInstance) GET(xrDestroyInstance) GET(xrGetSystem)
    GET(xrGetSystemProperties) GET(xrCreateSession) GET(xrDestroySession)
    GET(xrCreateReferenceSpace) GET(xrDestroySpace)
    GET(xrEnumerateSwapchainFormats) GET(xrCreateSwapchain)
    GET(xrDestroySwapchain) GET(xrEnumerateSwapchainImages)
    GET(xrAcquireSwapchainImage) GET(xrWaitSwapchainImage)
    GET(xrReleaseSwapchainImage) GET(xrBeginSession) GET(xrEndSession)
    GET(xrWaitFrame) GET(xrBeginFrame) GET(xrEndFrame) GET(xrPollEvent)
    GET(xrResultToString) GET(xrGetInstanceProperties) GET(xrLocateViews)
    GET(xrEnumerateViewConfigurationViews)
    GET(xrCreateActionSet) GET(xrDestroyActionSet) GET(xrCreateAction)
    GET(xrStringToPath) GET(xrSuggestInteractionProfileBindings)
    GET(xrAttachSessionActionSets) GET(xrSyncActions)
    GET(xrGetActionStateBoolean) GET(xrGetActionStateFloat)
    GET(xrGetActionStateVector2f)
    GET(xrCreateActionSpace) GET(xrLocateSpace)
#undef GET
    return 1;
}

/* The selected 32-bit runtime's manifest path, read once.  Two callers want
   it now -- the log line, and the fallback that must skip the runtime which
   already failed -- and re-reading the registry in each is how they drift
   apart. */
static const char *active_runtime_json(void) {
    static char rt[MAX_PATH];
    static int done;
    HKEY k;
    DWORD cb = sizeof(rt), ty = 0;
    if (done) return rt;
    done = 1;
    rt[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0,
                      KEY_READ | KEY_WOW64_32KEY, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "ActiveRuntime", NULL, &ty, (BYTE *)rt, &cb)
            != ERROR_SUCCESS) rt[0] = 0;
        RegCloseKey(k);
    }
    return rt;
}

/* Which VR host software is running.  A runtime dll can be perfectly
   installed and still refuse to start because the program behind it is not
   up, and in a log that is indistinguishable from a broken install.  The
   read-me says to start the headset software first; this says whether it
   was. */
static void report_vr_processes(void) {
    static const char *known[] = {
        "VirtualDesktop.Streamer.exe", "vrserver.exe", "vrmonitor.exe",
        "OVRServer_x64.exe", "OculusClient.exe", "wmrhost.exe",
        "SteamVRMonitor.exe", NULL
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    int found = 0, i;
    if (snap == INVALID_HANDLE_VALUE) return;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            for (i = 0; known[i]; i++) {
                if (lstrcmpiA(pe.szExeFile, known[i]) == 0) {
                    kv_log("  running: %s", pe.szExeFile);
                    found++;
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    if (!found)
        kv_log("  NO VR host software is running -- no Virtual Desktop "
               "Streamer, no SteamVR, no Oculus service. Start the headset "
               "software first, then the game.");
}

/* Every runtime Windows knows about for this bitness, not only the selected
   one.  XR_RUNTIME_JSON overrides the registry for this process alone, so
   trying one changes nothing on the machine. */
static XrResult try_other_runtimes(const XrInstanceCreateInfo *ci,
                                   const char *failed) {
    HKEY k;
    DWORD i = 0;
    XrResult last = XR_ERROR_RUNTIME_UNAVAILABLE;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
                      KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) {
        kv_log("VR: no other 32-bit runtimes are registered on this machine, "
               "so there is nothing to fall back to.");
        return last;
    }
    for (;;) {
        char name[MAX_PATH];
        DWORD nsz = sizeof(name), type = 0, data = 0, dsz = sizeof(data);
        XrResult r;
        if (RegEnumValueA(k, i++, name, &nsz, NULL, &type,
                          (LPBYTE)&data, &dsz) != ERROR_SUCCESS) break;
        if (data) continue;                       /* non-zero means disabled */
        if (failed && lstrcmpiA(name, failed) == 0) continue;
        kv_log("VR: trying another installed runtime: %s", name);
        SetEnvironmentVariableA("XR_RUNTIME_JSON", name);
        r = f_xrCreateInstance(ci, &g_instance);
        if (XR_SUCCEEDED(r)) {
            kv_log("VR: that one works. Using it for this session. The "
                   "machine's selected runtime is unchanged -- to make this "
                   "permanent, select it in that software's own settings.");
            RegCloseKey(k);
            return r;
        }
        kv_log("VR:   it failed too: %s", xr_str(r));
        last = r;
        SetEnvironmentVariableA("XR_RUNTIME_JSON", NULL);
    }
    RegCloseKey(k);
    return last;
}

/* Is this process elevated?  It decides something not obvious: the loader
   ignores XR_RUNTIME_JSON in a high-integrity process, so the per-process
   runtime override our fallback relies on silently does nothing and every
   retry re-runs the identical broken case. */
static int process_is_elevated(void) {
    HANDLE t = NULL;
    TOKEN_ELEVATION e;
    DWORD cb = sizeof(e);
    int r = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    if (GetTokenInformation(t, TokenElevation, &e, cb, &cb))
        r = e.TokenIsElevated != 0;
    CloseHandle(t);
    return r;
}

/* Make the loader name the file it could not read.

   It has a diagnostic mode that prints the filename and the reason, and it
   writes to stdout -- which in a GUI process may have no handle at all, hence
   freopen rather than dup2.  Only ever called after bring-up has already
   failed, so the extra xrCreateInstance costs a frame that was lost anyway
   and nothing on a working machine is touched. */
static void capture_loader_debug(const XrInstanceCreateInfo *ci) {
    char tmp[MAX_PATH], line[1024];
    const char *appdata = getenv("LOCALAPPDATA");
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r;
    FILE *f;

    if (!appdata || !appdata[0]) return;
    _snprintf(tmp, MAX_PATH, "%s\\KeflingsVR\\loader-debug.txt", appdata);

    kv_log("XR LOADER LOG: asking the loader itself which file it could not "
           "read. Everything between here and 'ends' is the loader talking, "
           "not us.");

    fflush(stdout);
    fflush(stderr);
    if (!freopen(tmp, "w", stdout)) {
        kv_log("  (could not capture the loader's output)");
        return;
    }
    freopen(tmp, "a", stderr);

    SetEnvironmentVariableA("XR_LOADER_DEBUG", "all");
    r = f_xrCreateInstance(ci, &inst);
    SetEnvironmentVariableA("XR_LOADER_DEBUG", NULL);

    fflush(stdout);
    fflush(stderr);
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);

    if (XR_SUCCEEDED(r) && f_xrDestroyInstance) f_xrDestroyInstance(inst);

    f = fopen(tmp, "r");
    if (!f) {
        kv_log("  (the loader printed nothing we could capture)");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == 0x0A || line[n - 1] == 0x0D)) line[--n] = 0;
        if (n) kv_log("  | %s", line);
    }
    fclose(f);
    kv_log("XR LOADER LOG: ends.");
}

/* Turn off implicit API layers that cannot load in THIS process.

   An implicit layer is pulled into every OpenXR application whether it asked
   or not. One registered for the wrong bitness -- OBS's OpenXR mirror does
   exactly this, registering in both views while shipping only a 64-bit dll --
   fails to load and takes xrCreateInstance down with it, reported as
   XR_ERROR_FILE_ACCESS_ERROR naming nothing.

   A layer manifest may declare `disable_environment`, an environment variable
   whose presence makes the loader skip it. Setting that for our own process
   changes nothing on the machine and affects no other application.

   Only layers that genuinely cannot load are touched. A working overlay is
   one the user installed on purpose. */
static void disable_unloadable_layers(void) {
    static const char *ROOT = "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
    HKEY roots[2];
    const char *rootname[2];
    int h;

    roots[0] = HKEY_CURRENT_USER;   rootname[0] = "this user";
    roots[1] = HKEY_LOCAL_MACHINE;  rootname[1] = "machine-wide";

    for (h = 0; h < 2; h++) {
        HKEY k;
        DWORD i = 0;
        /* Our own bitness only: the 64-bit registrations are correct for
           64-bit applications and none of our business. */
        if (RegOpenKeyExA(roots[h], ROOT, 0,
                          KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS)
            continue;
        for (;;) {
            char name[MAX_PATH], lib[MAX_PATH], full[MAX_PATH], var[128];
            DWORD nsz = sizeof(name), type = 0, data = 0, dsz = sizeof(data);
            HMODULE m;
            DWORD err;

            if (RegEnumValueA(k, i++, name, &nsz, NULL, &type,
                              (LPBYTE)&data, &dsz) != ERROR_SUCCESS) break;
            if (data) continue;                  /* already disabled */
            if (!json_library_path(name, lib, sizeof(lib))) continue;
            resolve_beside(name, lib, full, sizeof(full));

            SetLastError(0);
            m = LoadLibraryA(full);
            if (m) { FreeLibrary(m); continue; } /* it works; leave it be */
            err = GetLastError();

            if (json_string_value(name, "disable_environment", var,
                                  sizeof(var))) {
                SetEnvironmentVariableA(var, "1");
                kv_log("VR: the %s implicit API layer %s cannot load in a "
                       "32-bit game (error %lu), so it has been switched off "
                       "for this process via %s. Nothing on the machine was "
                       "changed and no other application is affected.",
                       rootname[h], name, err, var);
            } else {
                kv_log("VR: the %s implicit API layer %s cannot load in a "
                       "32-bit game (error %lu) and its manifest offers no "
                       "way to switch it off. It will fail xrCreateInstance "
                       "for EVERY 32-bit OpenXR application on this PC. "
                       "Remove or disable its 32-bit registration under "
                       "HKCU/HKLM SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1\\"
                       "ApiLayers\\Implicit.", rootname[h], name, err);
            }
        }
        RegCloseKey(k);
    }
}

static int create_instance(void) {
    XrInstanceCreateInfo ci;
    XrInstanceProperties props;
    const char *exts[1];

    if (XR_FAILED(xrGIPA(XR_NULL_HANDLE, "xrCreateInstance",
                         (PFN_xrVoidFunction *)&f_xrCreateInstance))) {
        kv_log("VR: loader would not give xrCreateInstance");
        return 0;
    }
    if (XR_FAILED(xrGIPA(XR_NULL_HANDLE, "xrResultToString",
                         (PFN_xrVoidFunction *)&f_xrResultToString)))
        f_xrResultToString = NULL;

    /* Which runtime is active is the single most useful line in a log file,
       and users routinely do not know: VDXR, SteamVR and the Oculus runtime
       all present as "OpenXR" and are chosen in another application. */
    kv_log("VR: 32-bit ActiveRuntime = %s",
           active_runtime_json()[0] ? active_runtime_json()
                                    : "(none registered)");

    /* Before anything is created: an implicit layer that cannot load will
       fail instance creation and name nothing. */
    disable_unloadable_layers();

    exts[0] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    memset(&ci, 0, sizeof(ci));
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    /* Ask for 1.0, NOT XR_CURRENT_API_VERSION.  These headers are 1.1.x and
       VDXR implements 1.0.x; a 1.0 runtime rejects a 1.1 request outright with
       XR_ERROR_API_VERSION_UNSUPPORTED.  Nothing here needs 1.1. */
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    lstrcpynA(ci.applicationInfo.applicationName, "A Kingdom for Keflings VR",
              XR_MAX_APPLICATION_NAME_SIZE);
    ci.applicationInfo.applicationVersion = 1;
    lstrcpynA(ci.applicationInfo.engineName, "wahoolib", XR_MAX_ENGINE_NAME_SIZE);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;

    {
        XrResult r = f_xrCreateInstance(&ci, &g_instance);
        if (XR_FAILED(r)) {
            kv_log("VR: xrCreateInstance failed: %s", xr_str(r));
            report_vr_processes();
            r = try_other_runtimes(&ci, active_runtime_json());
            if (XR_FAILED(r)) {
                capture_loader_debug(&ci);
                return 0;
            }
        }
    }
    if (!bind_xr(g_instance)) return 0;

    memset(&props, 0, sizeof(props));
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (f_xrGetInstanceProperties(g_instance, &props) == XR_SUCCESS)
        kv_log("VR: runtime \"%s\" %u.%u.%u", props.runtimeName,
               (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
               (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
               (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
    return 1;
}

static int create_session(HDC hdc, HGLRC rc) {
    XrSystemGetInfo sgi;
    XrSystemProperties sp;
    XrGraphicsRequirementsOpenGLKHR req;
    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = NULL;
    XrGraphicsBindingOpenGLWin32KHR bind;
    XrSessionCreateInfo sci;
    XrReferenceSpaceCreateInfo rsci;

    memset(&sgi, 0, sizeof(sgi));
    sgi.type = XR_TYPE_SYSTEM_GET_INFO;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(f_xrGetSystem(g_instance, &sgi, &g_system), "xrGetSystem");

    memset(&sp, 0, sizeof(sp));
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (f_xrGetSystemProperties(g_instance, g_system, &sp) == XR_SUCCESS)
        kv_log("VR: system \"%s\"", sp.systemName);

    /* Mandatory before xrCreateSession with the GL extension: skipping it
       makes session creation fail with no useful diagnosis. */
    xrGIPA(g_instance, "xrGetOpenGLGraphicsRequirementsKHR",
           (PFN_xrVoidFunction *)&getReq);
    if (!getReq) {
        kv_log("VR: runtime has no xrGetOpenGLGraphicsRequirementsKHR");
        return 0;
    }
    memset(&req, 0, sizeof(req));
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
    XRCHECK(getReq(g_instance, g_system, &req),
            "xrGetOpenGLGraphicsRequirementsKHR");

    memset(&bind, 0, sizeof(bind));
    bind.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
    bind.hDC = hdc;
    bind.hGLRC = rc;

    memset(&sci, 0, sizeof(sci));
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &bind;
    sci.systemId = g_system;
    XRCHECK(f_xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession");

    memset(&rsci, 0, sizeof(rsci));
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XRCHECK(f_xrCreateReferenceSpace(g_session, &rsci, &g_space),
            "xrCreateReferenceSpace");
    return 1;
}

static int pick_format(void) {
    int64_t *formats;
    uint32_t n = 0, i;
    g_format = 0;
    XRCHECK(f_xrEnumerateSwapchainFormats(g_session, 0, &n, NULL),
            "xrEnumerateSwapchainFormats(count)");
    formats = (int64_t *)calloc(n ? n : 1, sizeof(int64_t));
    XRCHECK(f_xrEnumerateSwapchainFormats(g_session, n, &n, formats),
            "xrEnumerateSwapchainFormats");
    /* Prefer SRGB8_ALPHA8.  The game's back buffer already holds sRGB-encoded,
       display-ready bytes.  Declaring the swapchain linear makes the runtime
       treat them as linear and encode them a second time on output, which is
       milky washed-out colour in the headset while the monitor looks perfect. */
    if (g_vrcfg.srgb_swapchain)
        for (i = 0; i < n; i++)
            if (formats[i] == GL_SRGB8_ALPHA8) { g_format = GL_SRGB8_ALPHA8; break; }
    if (!g_format)
        for (i = 0; i < n; i++)
            if (formats[i] == GL_RGBA8) { g_format = GL_RGBA8; break; }
    if (!g_format && n) g_format = formats[0];
    kv_log("VR: %u swapchain formats, using 0x%llx%s", n,
           (unsigned long long)g_format,
           g_format == GL_SRGB8_ALPHA8 ? " (SRGB8_ALPHA8)" : "");
    free(formats);
    return g_format != 0;
}

static int make_scene_target(int w, int h);

static int make_swapchain(int w, int h, XrSwapchain *out,
                          XrSwapchainImageOpenGLKHR **imgs, uint32_t *count) {
    XrSwapchainCreateInfo sci;
    uint32_t n = 0, i;
    memset(&sci, 0, sizeof(sci));
    sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = g_format;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    XRCHECK(f_xrCreateSwapchain(g_session, &sci, out), "xrCreateSwapchain");
    XRCHECK(f_xrEnumerateSwapchainImages(*out, 0, &n, NULL),
            "xrEnumerateSwapchainImages(count)");
    *imgs = (XrSwapchainImageOpenGLKHR *)calloc(n, sizeof(**imgs));
    /* Each element must carry the GL variant's type tag.  Aliasing the struct
       without also changing the enum compiles and links perfectly and is
       rejected only by this one call, long after the mistake. */
    for (i = 0; i < n; i++) (*imgs)[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    XRCHECK(f_xrEnumerateSwapchainImages(*out, n, &n,
                                         (XrSwapchainImageBaseHeader *)*imgs),
            "xrEnumerateSwapchainImages");
    *count = n;
    return 1;
}

static int create_targets(int gw, int gh) {
    uint32_t n = 0, i;
    XrViewConfigurationView vcv[MAXEYE];

    if (!pick_format()) return 0;

    if (!g_vrcfg.stereo) {
        if (!make_swapchain(gw, gh, &g_quad_sc, &g_quad_img, &g_quad_n))
            return 0;
        g_quad_w = gw; g_quad_h = gh;
        kv_log("VR: virtual screen swapchain %dx%d, %u images", gw, gh, g_quad_n);
        p_glGenFramebuffers(1, &g_fbo);
        return 1;
    }

    XRCHECK(f_xrEnumerateViewConfigurationViews(
                g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0, &n, NULL), "xrEnumerateViewConfigurationViews(count)");
    if (n > MAXEYE) n = MAXEYE;
    for (i = 0; i < n; i++) {
        memset(&vcv[i], 0, sizeof(vcv[i]));
        vcv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    XRCHECK(f_xrEnumerateViewConfigurationViews(
                g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                n, &n, vcv), "xrEnumerateViewConfigurationViews");
    g_nview = n;
    kv_log("VR: %u views; runtime recommends %ux%u per eye", n,
           vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);

    /* The eye size.  This used to be half the game's back buffer, which
       quietly handed the choice to the desktop: a 2560x1440 monitor gave
       1287x1431 an eye against a runtime asking for 3072x3264, and the
       compositor upscaled the difference.  Take the runtime's number instead
       and give the engine somewhere of that size to draw.

       The back buffer is still the fallback, so a target that cannot be
       created costs sharpness and nothing else. */
    g_eyew = gw / 2;
    g_eyeh = gh;
    g_scenew = g_eyew;
    g_sceneh = g_eyeh;
    if (g_vrcfg.offscreen) {
        float sc = g_vrcfg.render_scale;
        int ew, eh;
        if (sc < 0.25f) sc = 0.25f;
        if (sc > 2.0f) sc = 2.0f;
        /* The swapchain is the runtime's size, always.  Only the target the
           engine draws into is scaled. */
        ew = (int)vcv[0].recommendedImageRectWidth;
        eh = (int)vcv[0].recommendedImageRectHeight;
        g_scenew = (int)(ew * sc + 0.5f);
        g_sceneh = (int)(eh * sc + 0.5f);
        if (g_scenew < 64) g_scenew = 64;
        if (g_sceneh < 64) g_sceneh = 64;
        if (sc != 1.0f)
            kv_log("VR: render_scale is %.2f, so the eye buffers are "
                   "%dx%d instead of the runtime's recommended %ux%u. "
                   "MEASURED 20 Sep on VirtualDesktopXR: any value "
                   "other than 1.0 collapsed the frame -- the eye copy "
                   "and submit went from 0.45 ms to 143.9 ms and the "
                   "game ran at 5.6 fps. Our own blit stays 1:1, so "
                   "this is the runtime taking a slow path for a "
                   "swapchain that is not the size it asked for. If "
                   "the frame rate is terrible, set render_scale back "
                   "to 1.0 -- that is almost certainly the cause.",
                   sc, ew, eh,
                   vcv[0].recommendedImageRectWidth,
                   vcv[0].recommendedImageRectHeight);
        if (ew > 0 && eh > 0 && make_scene_target(g_scenew * 2, g_sceneh)) {
            g_eyew = ew;
            g_eyeh = eh;
        } else {
            /* No target: the engine draws into the back buffer halves, and
               those are the scene size too. */
            g_scenew = g_eyew;
            g_sceneh = g_eyeh;
        }
    }
    for (i = 0; i < g_nview; i++)
        if (!make_swapchain(g_eyew, g_eyeh, &g_eye_sc[i], &g_eye_img[i],
                            &g_eye_n[i])) return 0;
    kv_log("VR: eye swapchains %dx%d (side-by-side in a %dx%d back buffer)",
           g_eyew, g_eyeh, gw, gh);
    if (g_scenew != g_eyew || g_sceneh != g_eyeh)
        kv_log("VR: rendering each eye at %dx%d and upscaling into the %dx%d "
               "swapchain (render_scale %.2f). The swapchain stays the size "
               "the runtime asked for -- handing it anything else is what "
               "cost 143 ms a frame.",
               g_scenew, g_sceneh, g_eyew, g_eyeh, g_vrcfg.render_scale);
    p_glGenFramebuffers(1, &g_fbo);
    g_stereo = 1;
    return 1;
}

static int vr_start(HDC hdc, HGLRC rc) {
    int w, h;
    client_size(hdc, &w, &h);
    if (w <= 0 || h <= 0) {
        kv_log("VR: window has no client area yet, deferring");
        return 0;
    }
    if (!bind_gl_ext()) return 0;
    if (!load_loader()) return 0;
    if (!create_instance()) return 0;
    if (!create_session(hdc, rc)) return 0;
    if (!create_targets(w, h)) return 0;
    if (!g_vrcfg.controllers)
        kv_log("INPUT: not binding the controllers, so Virtual Desktop's "
               "gamepad emulation can have them");
    else if (create_actions()) open_shared_input();
    else if (1)
        kv_log("INPUT: controllers unavailable; the game still runs");
    g_rc = rc;
    kv_log("VR: session up, game framebuffer %dx%d, mode=%s", w, h,
           g_vrcfg.stereo ? "stereo diorama" : "virtual screen");
    return 1;
}

/* ---- teardown ---------------------------------------------------------- */
static void vr_stop(void) {
    int i;
    /* Renderer resources first, then spaces and session, then the instance.
       Destroying a session before the swapchains that are its children is the
       usual way to make teardown fail or hang. */
    if (g_fbo && p_glDeleteFramebuffers) { p_glDeleteFramebuffers(1, &g_fbo); g_fbo = 0; }
    for (i = 0; i < MAXEYE; i++) {
        if (g_eye_sc[i] && f_xrDestroySwapchain) f_xrDestroySwapchain(g_eye_sc[i]);
        g_eye_sc[i] = XR_NULL_HANDLE;
        free(g_eye_img[i]); g_eye_img[i] = NULL; g_eye_n[i] = 0;
    }
    if (g_quad_sc && f_xrDestroySwapchain) f_xrDestroySwapchain(g_quad_sc);
    g_quad_sc = XR_NULL_HANDLE;
    free(g_quad_img); g_quad_img = NULL; g_quad_n = 0;
    if (g_actions && f_xrDestroyActionSet) f_xrDestroyActionSet(g_actions);
    g_actions = XR_NULL_HANDLE; g_actions_ready = 0;
    if (g_aim_space && f_xrDestroySpace) f_xrDestroySpace(g_aim_space);
    g_aim_space = XR_NULL_HANDLE;
    if (g_aim_space_l && f_xrDestroySpace) f_xrDestroySpace(g_aim_space_l);
    g_aim_space_l = XR_NULL_HANDLE;
    if (g_space && f_xrDestroySpace) f_xrDestroySpace(g_space);
    g_space = XR_NULL_HANDLE;
    if (g_session && f_xrDestroySession) f_xrDestroySession(g_session);
    g_session = XR_NULL_HANDLE;
    if (g_instance && f_xrDestroyInstance) f_xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
    g_running = g_frame_open = g_views_valid = g_stereo = 0;
    g_rc = NULL;
    kv_log("VR: session torn down after %u frames "
           "(fail: wait=%u begin=%u acquire=%u submit=%u locate=%u)",
           g_vr_frames, g_fail_wait, g_fail_begin, g_fail_acquire,
           g_fail_submit, g_fail_locate);
}

void vr_shutdown(void) {
    if (g_instance) vr_stop();
    if (g_loader) { FreeLibrary(g_loader); g_loader = NULL; }
}

void vr_on_delete_context(HGLRC rc) {
    if (g_instance && rc == g_rc) {
        kv_log("VR: the GL context the session is bound to is being destroyed; "
               "ending VR (normal at quit, also happens on a resolution change)");
        vr_stop();
    }
}

void vr_on_make_current(HDC hdc, HGLRC rc) { (void)hdc; (void)rc; }

/* ---- events ------------------------------------------------------------ */
static void poll_events(void) {
    XrEventDataBuffer ev;
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (f_xrPollEvent(g_instance, &ev) != XR_SUCCESS) return;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *s =
                (XrEventDataSessionStateChanged *)&ev;
            g_state = s->state;
            kv_log("VR: session state -> %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY && !g_running) {
                XrSessionBeginInfo bi;
                memset(&bi, 0, sizeof(bi));
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(f_xrBeginSession(g_session, &bi))) g_running = 1;
                else kv_log("VR: xrBeginSession failed");
            } else if (g_state == XR_SESSION_STATE_STOPPING && g_running) {
                f_xrEndSession(g_session);
                g_running = 0;
            } else if (g_state == XR_SESSION_STATE_EXITING ||
                       g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_frame_open = 0;
                vr_stop();
                g_failed = 1;
                return;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            /* The player held the headset's own recenter. The world follows
               LOCAL space by itself; the menus' anchor has to be taken again.
               Ten frames, so the new pose is in effect when it is. */
            vr_recenter("the headset's own recenter", 10);
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            kv_log("VR: instance loss pending");
            g_frame_open = 0;
            vr_stop();
            g_failed = 1;
            return;
        }
    }
}

/* Every begun frame must be ended.  A frame left open turns the next
   Wait/Begin into a call-order violation, which runtimes report as a null
   dereference inside the loader rather than as an error code. */
static void end_open_frame_with_nothing(void) {
    XrFrameEndInfo fei;
    if (!g_frame_open) return;
    memset(&fei, 0, sizeof(fei));
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = g_predicted;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = 0;
    f_xrEndFrame(g_session, &fei);
    g_frame_open = 0;
}

/* ---- what the proxy asks ---------------------------------------------- */
int vr_stereo_active(void) {
    return g_vrcfg.enabled && !g_failed && g_running && g_stereo && g_views_valid;
}

int vr_eye_count(void) { return (int)g_nview; }
int vr_eye_width(void) { return g_eyew; }
int vr_eye_height(void) { return g_eyeh; }

void vr_eye_viewport(int eye, int *x, int *y, int *w, int *h) {
    /* Inside the SCENE target -- that is where the engine draws. */
    *x = eye * g_scenew;
    *y = 0;
    *w = g_scenew;
    *h = g_sceneh;
}

/* How much of the headset's field of view we are entitled to fill.  In auto
   mode that is however much fits inside the frustum the engine itself renders:
   asking for more does not show more world, it shows the same world with the
   culled parts missing at the edges. */
/* Where the panel stands while it is world-locked: a yaw and a position,
   captured when the panel appears.  Yaw only -- a panel that inherits the
   head's pitch and roll is a panel that hangs at an angle for the rest of the
   session. */
static float g_anchor_yaw, g_anchor_pos[3];
static int   g_anchor_valid, g_panel_was_up;

static void anchor_capture(const char *why) {
    const XrQuaternionf *q = &g_view[0].pose.orientation;
    float fx, fz;
    /* The head's forward, flattened onto the floor plane. */
    fx = 2.0f * (q->x * q->z + q->w * q->y);
    fz = 1.0f - 2.0f * (q->x * q->x + q->y * q->y);
    g_anchor_yaw = (float)atan2(fx, fz);
    g_anchor_pos[0] = g_view[0].pose.position.x;
    g_anchor_pos[1] = g_view[0].pose.position.y;
    g_anchor_pos[2] = g_view[0].pose.position.z;
    g_anchor_valid = 1;
    kv_log("PANEL: anchored at %.0f degrees (%s)",
           g_anchor_yaw * 57.295779513082323, why);
}

/* RECENTER (24 Sep 2026). The anchor is taken when a panel opens -- but the
   game's HUD counts as a panel and is up the whole time, so in practice it was
   taken once at launch and the menus stayed at that angle all session. Holding
   the Quest's own recenter did not help: it moves the whole LOCAL space, and
   the stored anchor moves with it. Both grips now ask for a new anchor, and so
   does the runtime's recenter event. Delayed a few frames so the capture sees
   the pose AFTER the runtime has applied its change. */
static int g_recenter_in;
void vr_recenter(const char *why, int delay_frames) {
    g_recenter_in = delay_frames > 0 ? delay_frames : 1;
    kv_log("PANEL: recenter asked for (%s) -- the menus will re-anchor in "
           "front of you", why);
}

/* Where the panel is standing, for anything that has to aim at it.  Without
   this the pointer intersects a plane in front of the TRACKING ORIGIN, which is
   where the panel used to be and is not where it is. */
/* menu_tilt in radians, clamped; 0 unless the panel stands in the world. */
float vr_panel_tilt(void) {
    float t = g_vrcfg.menu_tilt;
    if (!g_vrcfg.hud_world_lock || !kv_scene_drawn()) return 0.0f;
    if (t < -60.0f) t = -60.0f; else if (t > 60.0f) t = 60.0f;
    return t * 0.017453292519943295f;
}
/* menu_height in metres; 0 over a splash or loading screen, as the tilt. */
float vr_panel_height(void) {
    float h = g_vrcfg.menu_height;
    if (!g_vrcfg.hud_world_lock || !kv_scene_drawn()) return 0.0f;
    if (h < -2.0f) h = -2.0f; else if (h > 2.0f) h = 2.0f;
    return h;
}
int vr_panel_anchor(float *yaw, float *pos) {
    if (!g_anchor_valid || !g_vrcfg.hud_world_lock) return 0;
    *yaw = g_anchor_yaw;
    pos[0] = g_anchor_pos[0];
    pos[1] = g_anchor_pos[1];
    pos[2] = g_anchor_pos[2];
    return 1;
}

/* Called ONCE per frame.  Building this into the projection would run it per
   eye, which halves every threshold in it without saying so. */
void vr_panel_anchor_frame(int panel_up) {
    float dot;
    if (!g_views_valid || g_nview < 1 || !g_vrcfg.hud_world_lock) return;
    if (g_recenter_in > 0 && --g_recenter_in == 0) {
        anchor_capture("recentered");
    } else if (panel_up && (!g_panel_was_up || !g_anchor_valid)) {
        anchor_capture("panel opened");
    } else if (panel_up) {
        /* Never losable: if the head has turned far enough that the panel has
           left the view completely, bring it back rather than leaving the
           player facing an empty room. */
        const XrQuaternionf *q = &g_view[0].pose.orientation;
        float fx = 2.0f * (q->x * q->z + q->w * q->y);
        float fz = 1.0f - 2.0f * (q->x * q->x + q->y * q->y);
        float yaw = (float)atan2(fx, fz);
        float d = yaw - g_anchor_yaw;
        while (d > 3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        if (d < 0.0f) d = -d;
        dot = (float)cos(d);
        {
            static unsigned tick;
            if ((tick++ % 180) == 0) {
                /* Where the player LOOKS against where the panel IS, up and down: the
                   question behind "I have to look down to see the menus". */
                float pitch = (float)asin(2.0f * (q->w * q->x - q->y * q->z));
                float dy = g_anchor_pos[1] + vr_panel_height() - g_view[0].pose.position.y;
                float elev = (float)atan2(dy, g_vrcfg.hud_distance > 0.2f ? g_vrcfg.hud_distance : 0.2f);
                kv_log("PANEL: head is %.0f degrees off the anchor (cos %.3f). "
                       "A figure pinned at 1.000 would mean the anchor is "
                       "being re-captured every frame and the panel is still "
                       "following the head.",
                       d * 57.295779513082323, dot);
                kv_log("PANEL: head pitch %+.0f degrees; the panel's centre is "
                       "%+.0f degrees from the eye (%+.2f m), tilt %.0f, "
                       "height %+.2f", pitch * 57.295779513082323,
                       elev * 57.295779513082323, dy, g_vrcfg.menu_tilt,
                       g_vrcfg.menu_height);
            }
        }
        if (d > 1.57079633f)         /* 90 degrees: it is fully out of view */
            anchor_capture("head turned away from it");
    }
    g_panel_was_up = panel_up;
}

float vr_needed_engine_fovy(void) {
    float tu, td, t, half, dev, w;
    if (!g_views_valid || g_nview < 1) return 0.0f;
    tu = (float)fabs(tan(g_view[0].fov.angleUp));
    td = (float)fabs(tan(g_view[0].fov.angleDown));
    t = (tu > td) ? tu : td;
    if (t <= 0.0f) return 0.0f;
    half = (float)atan(t);              /* the headset's own half angle */

    /* How far the head is off the TRACKING origin.  Reported either way,
       because it is what says whether the next clause is earning its keep. */
    w = g_view[0].pose.orientation.w;
    if (w < 0.0f) w = -w;
    if (w > 1.0f) w = 1.0f;
    dev = 2.0f * (float)acos(w);        /* total rotation from the origin */
    g_head_dev_deg = dev * 57.295779513082323f;

    /* With head_cull OFF the engine's camera does not move, so the region you
       can see swings outside its frustum as you turn and the holes come back on
       the side you turned towards -- the only remedy then is to widen the
       engine by however far the head has turned.  That is enormously
       expensive: at 59 degrees off axis it asks for 150, and a 152 x 131 degree
       cone on a game built for 45 draws several times the geometry, nearly all
       of it outside the eye.

       With head_cull ON the engine's view is premultiplied by the head's
       rotation, so its camera axis IS the gaze and the deviation is always
       zero.  Covering the headset's own field of view is then exactly enough,
       and every degree beyond it is drawn and never seen. */
    if (!g_vrcfg.head_cull) half += dev;
    half *= 1.06f;                      /* margin for roll and for prediction */

    /* A frustum cannot be built from a half angle approaching 90 degrees, and
       the cost of drawing one climbs fast.  Clamp, and say so. */
    {
    float cap = g_vrcfg.engine_fov_max * 0.5f * 0.017453292519943295f;
    if (cap > 1.48352986f) cap = 1.48352986f;   /* 85 degrees; a frustum cannot
                                                   be built much beyond this */
    if (half > cap) {
        half = cap;
        g_fov_clamped = 1;
    } else {
        g_fov_clamped = 0;
    }
    }
    return half * 2.0f * 57.295779513082323f;
}

/* Does the engine's cull frustum actually cover the eye, and how far is the
   eye from the camera the engine culls with?  Two different failures, and
   only the first is fixed by widening the camera. */
void vr_cull_coverage_report(double engine_near) {
    float tu, td, tl, tr, px, py, pz, off_m, off_u, near_deg, mid_deg;
    const float R2D = 57.295779513082323f;
    if (!g_views_valid || g_nview < 1) {
        kv_log("  cull coverage: no located views yet, nothing to measure");
        return;
    }
    tu = (float)fabs(tan(g_view[0].fov.angleUp));
    td = (float)fabs(tan(g_view[0].fov.angleDown));
    tl = (float)fabs(tan(g_view[0].fov.angleLeft));
    tr = (float)fabs(tan(g_view[0].fov.angleRight));
    if (g_eng_tanx <= 0.0f || g_eng_tany <= 0.0f) {
        kv_log("  cull coverage: the engine's projection has not been read "
               "yet");
        return;
    }
    kv_log("  cull coverage (eye tangent / engine tangent, over 1.00 means "
           "the engine is NOT drawing what that edge of your view can see): "
           "up %.2f down %.2f left %.2f right %.2f%s",
           tu / g_eng_tany, td / g_eng_tany, tl / g_eng_tanx,
           tr / g_eng_tanx,
           (tu > g_eng_tany || td > g_eng_tany || tl > g_eng_tanx ||
            tr > g_eng_tanx)
               ? "  <<< an edge is UNCOVERED: raise engine_fov"
               : "  (all covered, so holes at an edge are NOT the camera "
                 "width)");

    /* The other half, and the one widening cannot fix.  The engine culls
       from ITS camera; our eye transform adds the head's position scaled by
       world_scale, so any distance from where the session started puts the
       eye somewhere the engine never culled for.  Worst on near geometry,
       which is the bottom of the view when you look down. */
    px = g_view[0].pose.position.x;
    py = g_view[0].pose.position.y;
    pz = g_view[0].pose.position.z;
    off_m = (float)sqrt(px * px + py * py + pz * pz);
    off_u = off_m * g_vrcfg.world_scale * (g_mag > 0.0f ? g_mag : 1.0f);
    if (engine_near < 1.0) engine_near = 70.0;
    near_deg = (float)atan2(off_u, engine_near) * R2D;
    mid_deg = (float)atan2(off_u, 1000.0) * R2D;
    kv_log("  cull parallax: your head is %.2f m from where the session "
           "started, which world_scale turns into %.0f GAME UNITS between "
           "your eye and the camera the engine culls with. That is %.0f "
           "degrees of view the engine never considered at its near plane "
           "(%.0f units) and %.0f degrees at 1000 units. Widening "
           "engine_fov cannot fix this one; standing where you started, or "
           "restarting the session where you play, makes it zero.",
           off_m, off_u, near_deg, engine_near, mid_deg);
}

float vr_engine_tanx(void) { return g_eng_tanx; }
float vr_engine_tany(void) { return g_eng_tany; }

void vr_set_engine_fov(float tan_x, float tan_y) {
    g_eng_tanx = tan_x;
    g_eng_tany = tan_y;
}

/* Fit horizontally and vertically INDEPENDENTLY.  The engine's frustum is wide
   and short -- about 73 x 45 degrees -- so a single uniform scale would be
   pinned by the vertical and throw away most of the horizontal view for
   nothing.  Non-uniform is not a distortion here, because the fov we declare
   to the compositor is the fov we actually rendered with. */
static void fov_scale_now(int eye, float *out_sx, float *out_sy) {
    float mx, my;
    *out_sx = *out_sy = 1.0f;
    if (g_vrcfg.fov_scale > 0.0f) {
        *out_sx = *out_sy = g_vrcfg.fov_scale;
        return;
    }
    if (g_eng_tanx <= 0.0f || g_eng_tany <= 0.0f) return;
    mx = (float)fabs(tan(g_view[eye].fov.angleLeft));
    if ((float)fabs(tan(g_view[eye].fov.angleRight)) > mx)
        mx = (float)fabs(tan(g_view[eye].fov.angleRight));
    my = (float)fabs(tan(g_view[eye].fov.angleUp));
    if ((float)fabs(tan(g_view[eye].fov.angleDown)) > my)
        my = (float)fabs(tan(g_view[eye].fov.angleDown));
    if (mx <= 0.0f || my <= 0.0f) return;
    *out_sx = g_eng_tanx / mx;
    *out_sy = g_eng_tany / my;
    if (*out_sx > 1.0f) *out_sx = 1.0f;
    if (*out_sy > 1.0f) *out_sy = 1.0f;
}

/* Disparity, in NDC x, for content that should appear at hud_distance.  The
   eye separation is taken from the located poses rather than assumed, so it is
   this headset's real IPD. */
int vr_screen_mode(void) { return g_vrcfg.screen_mode != 0; }

float vr_converge_world(void) { return g_vrcfg.converge; }

/* Half the measured IPD, converted into game world units.  Taken from the two
   located eye poses rather than assumed, so it is this headset's real value. */
float vr_eye_offset_world(int eye) {
    float dx, dy, dz, ipd;
    if (g_nview < 2 || !g_views_valid) return 0.0f;
    dx = g_view[1].pose.position.x - g_view[0].pose.position.x;
    dy = g_view[1].pose.position.y - g_view[0].pose.position.y;
    dz = g_view[1].pose.position.z - g_view[0].pose.position.z;
    ipd = (float)sqrt(dx * dx + dy * dy + dz * dz);
    return ((eye == 0) ? -0.5f : 0.5f) * ipd * g_vrcfg.world_scale;
}

float vr_hud_ndc_shift(int eye) {
    float dx, dy, dz, ipd, tl, tr, dt, d;
    if (g_nview < 2 || !g_views_valid) return 0.0f;
    d = g_vrcfg.hud_distance;
    if (d < 0.2f) return 0.0f;
    dx = g_view[1].pose.position.x - g_view[0].pose.position.x;
    dy = g_view[1].pose.position.y - g_view[0].pose.position.y;
    dz = g_view[1].pose.position.z - g_view[0].pose.position.z;
    ipd = (float)sqrt(dx * dx + dy * dy + dz * dz);

    /* Displacement in TANGENT units for something at distance d, converted
       into this eye's NDC through its own (asymmetric) frustum width. */
    dt = ((eye == 0) ? 1.0f : -1.0f) * (ipd * 0.5f) / d;
    tl = (float)tan(g_view[eye].fov.angleLeft);
    tr = (float)tan(g_view[eye].fov.angleRight);
    if (tr - tl <= 0.0f) return 0.0f;
    return 2.0f * dt / (tr - tl);
}

void vr_hud_projection(int eye, float canvas_aspect, float size_deg,
                       int world_lock, float *m16) {
    float tl, tr, tu, td, hw, hh, sx, sy, cx, cy;
    int i;
    for (i = 0; i < 16; i++) m16[i] = (i % 5) ? 0.0f : 1.0f;
    if (eye < 0 || eye >= (int)g_nview || !g_views_valid) return;
    if (canvas_aspect <= 0.0f) canvas_aspect = 1.7778f;

    tl = (float)tan(g_view[eye].fov.angleLeft);
    tr = (float)tan(g_view[eye].fov.angleRight);
    tu = (float)tan(g_view[eye].fov.angleUp);
    td = (float)tan(g_view[eye].fov.angleDown);

    /* Half-width in tangent units, and the height that KEEPS THE CANVAS
       ASPECT.  Deriving the height from the eye instead is what stretched the
       menu by nearly four times. */
    if (size_deg < 5.0f) size_deg = 5.0f;
    if (size_deg > 170.0f) size_deg = 170.0f;
    hw = (float)tan(size_deg * 0.5 * 0.017453292519943295);
    hh = hw / canvas_aspect;

    sx = 2.0f * hw / (tr - tl);
    sy = 2.0f * hh / (tu - td);
    /* An eye frustum is asymmetric, so its forward direction is NOT the middle
       of its image.  2D centred on the buffer is centred on nothing. */
    cx = -(tr + tl) / (tr - tl);
    cy = -(tu + td) / (tu - td);

    /* ---- fixed in space ------------------------------------------------
       Build the canvas as a real rectangle standing at hud_distance in front
       of the tracking origin, and run it through the eye's own view and
       projection.  Because the placement is in the world rather than in the
       eye, turning the head slides the head across the panel instead of
       carrying the panel along -- which is the whole point: the cursor then
       stays over the world point the game thinks it is over. */
    if (world_lock && g_vrcfg.hud_world_lock) {
        float proj[16], view[16], panel[16], tmp[16];
        float d = g_vrcfg.hud_distance;
        int k;
        if (d < 0.2f) d = 0.2f;

        /* The panel is a readable size again, NOT the engine frustum.
           It had to match the frustum only so that a cursor away from the
           centre would land on what it appeared to point at.  With the cursor
           pinned to the centre that requirement is gone -- the centre of any
           frustum is the same direction -- and matching a 150 degree engine
           camera was throwing the screen corners out to where they could not
           be seen or reached. */

        /* The true eye frustum, unscaled: this panel is real geometry in
           metres, not a picture being fitted to the field of view. */
        mat_frustum(&g_view[eye].fov, 1.0f, 1.0f, 0.05, 100.0, proj);
        mat_pose_inverse(&g_view[eye].pose, 1.0f, view);

        for (k = 0; k < 16; k++) panel[k] = 0.0f;
        panel[0]  = hw * d;      /* canvas x in [-1,1] -> metres across */
        panel[5]  = hh * d;
        {   /* menu_tilt: the top leans back (away, -z) about the centre
               line. pointer.c's grip_pointer undoes exactly this. */
            float t = vr_panel_tilt();
            panel[5] = hh * d * (float)cos(t);
            panel[6] = -hh * d * (float)sin(t);
        }
        panel[10] = 0.0f;        /* flatten: every 2D element on one plane */
        panel[13] = vr_panel_height();   /* raised above the eye, metres */
        panel[14] = -d;          /* d metres in front of the anchor */
        panel[15] = 1.0f;

        /* Stand it where the player was looking when it opened, not in front
           of the tracking origin.  The origin is only the right place if the
           player happens to be facing the way they were when the session
           started; any other direction and the panel is behind them, which is
           why this branch was off by default. */
        if (g_anchor_valid) {
            float a[16], c = (float)cos(g_anchor_yaw), sn = (float)sin(g_anchor_yaw);
            for (k = 0; k < 16; k++) a[k] = (k % 5) ? 0.0f : 1.0f;
            a[0] = c;  a[8]  = sn;
            a[2] = -sn; a[10] = c;
            a[12] = g_anchor_pos[0];
            a[13] = g_anchor_pos[1];
            a[14] = g_anchor_pos[2];
            mat_mul(a, panel, tmp);
            memcpy(panel, tmp, sizeof(panel));
        }

        mat_mul(proj, view, tmp);
        mat_mul(tmp, panel, m16);

        g_panel_hw = hw * d;     /* metres, for the pointer */
        g_panel_hh = hh * d;
        g_panel_dist = d;
        g_panel_ok = 1;
        {
            /* Reported on a change, not once: the first draw happens before
               the engine has shown a scene projection, so a one-shot report
               captures the fallback size and then claims it is the engine
               frustum -- which is exactly what it did. */
            static float said_hw;
            if (hw != said_hw) {
                said_hw = hw;
                kv_log("HUD: the in-game screen is drawn at "
                       "tan %.4f x %.4f (%.1f x %.1f degrees), fixed in space "
                       "at %.2f m%s",
                       hw, hh,
                       2.0 * atan(hw) * 57.295779513082323,
                       2.0 * atan(hh) * 57.295779513082323, d,
                       (g_eng_tanx > 0.0f && hw == g_eng_tanx)
                           ? " -- matching the engine frustum, so the cursor "
                             "and the highlight box are the same direction"
                           : " -- NOT the engine frustum yet (it has not drawn "
                             "a scene), so the cursor will not agree with the "
                             "highlight box on these frames");
                kv_log("HUD: its corners sit %.0f degrees across and %.0f up "
                       "and down. Anything past about 45 is at the rim of a "
                       "Quest 3 view, which is where the save and zoom icons "
                       "stop being reachable.",
                       atan(hw) * 57.295779513082323,
                       atan(hh) * 57.295779513082323);
            }
        }
        return;
    }

    /* Recorded, not recomputed elsewhere: the menu and the HUD use
       different sizes, so a second copy of this calculation would be
       right for one of them and wrong for the other. */
    g_panel_hw = hw;
    g_panel_hh = hh;
    g_panel_dist = 0.0f;         /* 0 means head locked, in tangent units */
    g_panel_ok = 1;

    m16[0]  = sx;
    m16[5]  = sy;
    m16[12] = cx + vr_hud_ndc_shift(eye);
    m16[13] = cy;
}

int vr_panel_geometry(float *half_w, float *half_h, float *distance) {
    if (!g_panel_ok) return 0;
    *half_w = g_panel_hw;
    *half_h = g_panel_hh;
    *distance = g_panel_dist;
    return 1;
}

/* The head, as the midpoint of the two eyes. */
int vr_head_pose(float *pos, float *quat) {
    if (!g_views_valid || g_nview < 1) return 0;
    if (g_nview >= 2) {
        pos[0] = 0.5f * (g_view[0].pose.position.x +
                         g_view[1].pose.position.x);
        pos[1] = 0.5f * (g_view[0].pose.position.y +
                         g_view[1].pose.position.y);
        pos[2] = 0.5f * (g_view[0].pose.position.z +
                         g_view[1].pose.position.z);
    } else {
        pos[0] = g_view[0].pose.position.x;
        pos[1] = g_view[0].pose.position.y;
        pos[2] = g_view[0].pose.position.z;
    }
    /* The two eyes share an orientation, so either will do. */
    quat[0] = g_view[0].pose.orientation.x;
    quat[1] = g_view[0].pose.orientation.y;
    quat[2] = g_view[0].pose.orientation.z;
    quat[3] = g_view[0].pose.orientation.w;
    return 1;
}

/* How much the engine's picture may be magnified before it runs out of
   headset to fill.  Uniform on both axes: stretching each axis to its own
   limit is anamorphic and reads as the world being the wrong SHAPE. */
static float magnification(int eye) {
    float sx, sy, mx, my, mmax;
    fov_scale_now(eye, &sx, &sy);
    if (sx <= 0.0f || sy <= 0.0f) return 1.0f;
    mx = 1.0f / sx;
    my = 1.0f / sy;
    mmax = (mx < my) ? mx : my;
    if (mmax < 1.0f) mmax = 1.0f;
    return 1.0f + g_vrcfg.fill_amount * (mmax - 1.0f);
}


/* The head's rotation as a matrix, and its inverse.  One source for both the
   engine's view and the eye transform: if they disagree by so much as a degree
   the whole world tilts. */
static void quat_to_m16(const XrQuaternionf *q, int inverse, float *m) {
    float x = q->x, y = q->y, z = q->z, w = q->w;
    int i;
    if (inverse) { x = -x; y = -y; z = -z; }
    for (i = 0; i < 16; i++) m[i] = (i % 5) ? 0.0f : 1.0f;
    m[0]  = 1.0f - 2.0f * (y * y + z * z);
    m[1]  = 2.0f * (x * y + z * w);
    m[2]  = 2.0f * (x * z - y * w);
    m[4]  = 2.0f * (x * y - z * w);
    m[5]  = 1.0f - 2.0f * (x * x + z * z);
    m[6]  = 2.0f * (y * z + x * w);
    m[8]  = 2.0f * (x * z + y * w);
    m[9]  = 2.0f * (y * z - x * w);
    m[10] = 1.0f - 2.0f * (x * x + y * y);
}

/* Rh-1, to pre-multiply the engine's view matrix so its culling turns with the
   head.  Returns 0 when there is no head pose, and the caller then leaves the
   engine alone rather than guessing. */
int vr_cull_rotation(float *m16) {
    if (!g_views_valid || g_nview < 1 || !g_vrcfg.head_cull) return 0;
    quat_to_m16(&g_view[0].pose.orientation, 1, m16);
    /* ...and the head's POSITION, so the engine's frustum apex sits at the
       eye rather than at its own camera.  A = R . T(-p): the translation
       column is -(R . p), in the same column-major layout. */
    if (g_vrcfg.cull_follow_head) {
        float s = g_vrcfg.world_scale * (g_mag > 0.0f ? g_mag : 1.0f);
        float px = g_view[0].pose.position.x * s;
        float py = g_view[0].pose.position.y * s;
        float pz = g_view[0].pose.position.z * s;
        m16[12] = -(m16[0] * px + m16[4] * py + m16[8]  * pz);
        m16[13] = -(m16[1] * px + m16[5] * py + m16[9]  * pz);
        m16[14] = -(m16[2] * px + m16[6] * py + m16[10] * pz);
    }
    return 1;
}

void vr_eye_projection(int eye, double znear, double zfar, float *m16) {
    float proj[16], view[16];
    if (eye < 0 || eye >= (int)g_nview) { memset(m16, 0, 16 * sizeof(float)); return; }
    fov_scale_now(eye, &g_fov_sx, &g_fov_sy);
    g_mag = magnification(eye);
    mat_frustum(&g_view[eye].fov, g_fov_sx, g_fov_sy, znear, zfar, proj);
    /* Magnifying makes everything look nearer, so the eye separation has to
       grow with it or the depth the eyes report contradicts the size they
       see -- which is felt as "the world is not right" rather than seen. */
    {
        /* With cull_follow_head the common head offset has been handed to the
           engine, so the eye projection must carry only what is LEFT: this
           eye's displacement from view[0], which is the stereo separation.
           Taking it out here is the other half of that change, and applying
           only one half moves the world twice as far as the head. */
        XrPosef pe = g_view[eye].pose;
        if (g_vrcfg.cull_follow_head && g_vrcfg.head_cull && g_nview >= 1) {
            pe.position.x -= g_view[0].pose.position.x;
            pe.position.y -= g_view[0].pose.position.y;
            pe.position.z -= g_view[0].pose.position.z;
        }
        mat_pose_inverse(&pe, g_vrcfg.world_scale * g_mag, view);
    }
    /* Cancel the rotation that has been pushed into the engine's view, so the
       picture stays exactly where it was while the engine's culling follows
       the head.  Same rotation, opposite side. */
    if (g_vrcfg.head_cull && g_views_valid && g_nview >= 1) {
        float rh[16], tmp[16];
        quat_to_m16(&g_view[0].pose.orientation, 0, rh);
        mat_mul(view, rh, tmp);
        memcpy(view, tmp, sizeof(view));
    }
    mat_mul(proj, view, m16);

    /* The positive control, once per session.  Build what the vertex
       transform WAS (all of the head pose in the eye projection, rotation
       only in the engine's matrix) and what it IS now (head offset moved
       into the engine's matrix), and print the largest element they differ
       by. The two are algebraically identical, so anything but ~0 means one
       of the two halves is wrong and the world will move with the head. */
    if (g_vrcfg.cull_follow_head && g_vrcfg.head_cull && eye == 0 &&
        !g_cull_checked && g_views_valid && g_nview >= 1) {
        float rh0[16], a_old[16], a_new[16], p_old[16], p_new[16];
        float full_old[16], full_new[16], voff[16];
        float s = g_vrcfg.world_scale * g_mag;
        float px, py, pz, worst = 0.0f;
        int i;
        g_cull_checked = 1;
        quat_to_m16(&g_view[0].pose.orientation, 0, rh0);
        /* what the engine used to get, and what it gets now */
        quat_to_m16(&g_view[0].pose.orientation, 1, a_old);
        memcpy(a_new, a_old, sizeof(a_new));
        px = g_view[0].pose.position.x * s;
        py = g_view[0].pose.position.y * s;
        pz = g_view[0].pose.position.z * s;
        a_new[12] = -(a_new[0] * px + a_new[4] * py + a_new[8]  * pz);
        a_new[13] = -(a_new[1] * px + a_new[5] * py + a_new[9]  * pz);
        a_new[14] = -(a_new[2] * px + a_new[6] * py + a_new[10] * pz);
        /* the eye projection, old (absolute pose) and new (relative) */
        mat_pose_inverse(&g_view[0].pose, s, voff);
        mat_mul(voff, rh0, p_old);
        mat_mul(proj, p_old, p_old);
        {
            XrPosef pr = g_view[0].pose;
            pr.position.x -= g_view[0].pose.position.x;
            pr.position.y -= g_view[0].pose.position.y;
            pr.position.z -= g_view[0].pose.position.z;
            mat_pose_inverse(&pr, s, voff);
        }
        mat_mul(voff, rh0, p_new);
        mat_mul(proj, p_new, p_new);
        mat_mul(p_old, a_old, full_old);
        mat_mul(p_new, a_new, full_new);
        for (i = 0; i < 16; i++) {
            float d = full_old[i] - full_new[i];
            if (d < 0.0f) d = -d;
            if (d > worst) worst = d;
        }
        kv_log("CULL FOLLOW: the engine now culls from your eye rather than "
               "its own camera (head %.0f game units away). Self-check: the "
               "vertex transform differs from the old one by at most %.6f -- "
               "it must be near zero, because the offset added to the "
               "engine's matrix is the same one taken out of the eye's. A "
               "large number here means the world will move with your head; "
               "press F6 or set cull_follow_head = 0.",
               (float)sqrt((double)(px * px + py * py + pz * pz)), worst);
    }
}


/* ---- Quest controllers, through the OpenXR action system ---------------- */
/* Every action needs a UNIQUE localizedActionName.  Two actions sharing one
   kills the second silently: it creates, it binds, and the button simply never
   does anything. */
static XrPath xpath(const char *s) {
    XrPath p = XR_NULL_PATH;
    if (f_xrStringToPath) f_xrStringToPath(g_instance, s, &p);
    return p;
}

static XrAction make_action(XrActionType type, const char *name,
                            const char *localized) {
    XrActionCreateInfo ci;
    XrAction a = XR_NULL_HANDLE;
    memset(&ci, 0, sizeof(ci));
    ci.type = XR_TYPE_ACTION_CREATE_INFO;
    ci.actionType = type;
    lstrcpynA(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(ci.localizedActionName, localized,
              XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    if (XR_FAILED(f_xrCreateAction(g_actions, &ci, &a)))
        kv_log("INPUT: could not create action %s", name);
    return a;
}

static int create_actions(void) {
    XrActionSetCreateInfo asci;
    XrInteractionProfileSuggestedBinding sug;
    XrActionSuggestedBinding b[16];
    XrSessionActionSetsAttachInfo att;
    int n = 0;

    memset(&asci, 0, sizeof(asci));
    asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    lstrcpynA(asci.actionSetName, "keflings", XR_MAX_ACTION_SET_NAME_SIZE);
    lstrcpynA(asci.localizedActionSetName, "A Kingdom for Keflings",
              XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XRCHECK(f_xrCreateActionSet(g_instance, &asci, &g_actions),
            "xrCreateActionSet");

    a_move   = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "move",   "Move");
    a_look   = make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "look",   "Look");
    a_select = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,  "select", "Select");
    a_back   = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,  "back",   "Back");
    a_menu   = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu",   "Open menu");
    a_gripL  = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "gripleft",  "Left grip");
    a_gripR  = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "gripright", "Right grip");
    a_btna   = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "btna", "A button");
    a_btnb   = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "btnb", "B button");
    a_btnx   = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "btnx", "X button");
    a_btny   = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "btny", "Y button");
    a_trigL  = make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigleft",
                           "Left trigger");
    a_stickL = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "stickleft",
                           "Zoom out");
    a_stickR = make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "stickright",
                           "Zoom in");
    a_aimR   = make_action(XR_ACTION_TYPE_POSE_INPUT, "aimright",
                           "Right hand aim");
    a_aimL   = make_action(XR_ACTION_TYPE_POSE_INPUT, "aimleft",
                           "Left hand aim");

    b[n].action = a_move;   b[n++].binding = xpath("/user/hand/left/input/thumbstick");
    b[n].action = a_look;   b[n++].binding = xpath("/user/hand/right/input/thumbstick");
    b[n].action = a_select; b[n++].binding = xpath("/user/hand/right/input/trigger/value");
    b[n].action = a_trigL;  b[n++].binding = xpath("/user/hand/left/input/trigger/value");
    b[n].action = a_menu;   b[n++].binding = xpath("/user/hand/left/input/menu/click");
    b[n].action = a_gripL;  b[n++].binding = xpath("/user/hand/left/input/squeeze/value");
    b[n].action = a_gripR;  b[n++].binding = xpath("/user/hand/right/input/squeeze/value");
    b[n].action = a_btna;   b[n++].binding = xpath("/user/hand/right/input/a/click");
    b[n].action = a_btnb;   b[n++].binding = xpath("/user/hand/right/input/b/click");
    b[n].action = a_btnx;   b[n++].binding = xpath("/user/hand/left/input/x/click");
    b[n].action = a_btny;   b[n++].binding = xpath("/user/hand/left/input/y/click");
    b[n].action = a_stickL; b[n++].binding = xpath("/user/hand/left/input/thumbstick/click");
    b[n].action = a_stickR; b[n++].binding = xpath("/user/hand/right/input/thumbstick/click");
    b[n].action = a_aimR;   b[n++].binding = xpath("/user/hand/right/input/aim/pose");
    b[n].action = a_aimL;   b[n++].binding = xpath("/user/hand/left/input/aim/pose");

    memset(&sug, 0, sizeof(sug));
    sug.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    sug.interactionProfile = xpath("/interaction_profiles/oculus/touch_controller");
    sug.suggestedBindings = b;
    sug.countSuggestedBindings = n;
    XRCHECK(f_xrSuggestInteractionProfileBindings(g_instance, &sug),
            "xrSuggestInteractionProfileBindings");

    memset(&att, 0, sizeof(att));
    att.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    att.countActionSets = 1;
    att.actionSets = &g_actions;
    XRCHECK(f_xrAttachSessionActionSets(g_session, &att),
            "xrAttachSessionActionSets");

    {
        XrActionSpaceCreateInfo asc;
        memset(&asc, 0, sizeof(asc));
        asc.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        asc.action = a_aimR;
        asc.subactionPath = XR_NULL_PATH;
        asc.poseInActionSpace.orientation.w = 1.0f;
        if (!a_aimR || !f_xrCreateActionSpace ||
            XR_FAILED(f_xrCreateActionSpace(g_session, &asc,
                                            &g_aim_space))) {
            g_aim_space = XR_NULL_HANDLE;
            kv_log("INPUT: no aim pose space; pointing with the "
                   "controller falls back to the right stick");
        }
        asc.action = a_aimL;
        if (!a_aimL || !f_xrCreateActionSpace ||
            XR_FAILED(f_xrCreateActionSpace(g_session, &asc, &g_aim_space_l)))
            g_aim_space_l = XR_NULL_HANDLE;
    }

    g_actions_ready = 1;
    kv_log("INPUT: Touch controller bindings attached (%d suggested)", n);
    return 1;
}

static float read_float(XrAction a) {
    XrActionStateGetInfo gi;
    XrActionStateFloat st;
    if (!a) return 0.0f;
    memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.action = a;
    memset(&st, 0, sizeof(st)); st.type = XR_TYPE_ACTION_STATE_FLOAT;
    if (XR_FAILED(f_xrGetActionStateFloat(g_session, &gi, &st))) return 0.0f;
    return st.isActive ? st.currentState : 0.0f;
}

static int read_bool(XrAction a) {
    XrActionStateGetInfo gi;
    XrActionStateBoolean st;
    if (!a) return 0;
    memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.action = a;
    memset(&st, 0, sizeof(st)); st.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    if (XR_FAILED(f_xrGetActionStateBoolean(g_session, &gi, &st))) return 0;
    return st.isActive && st.currentState;
}

/* isActive is the whole diagnosis for a control that does nothing.
   INACTIVE means the runtime has not bound this action to anything on the
   hardware -- our binding is wrong, or something else has taken the control.
   ACTIVE but always zero means it is bound and simply is not being moved, or
   the value is being consumed before it reaches us.  Those need opposite
   fixes, and from outside they look identical. */
static void read_vec2_named(XrAction a, float *x, float *y, const char *name,
                            int *reported) {
    XrActionStateGetInfo gi;
    XrActionStateVector2f st;
    XrResult r;
    *x = 0.0f; *y = 0.0f;
    if (!a) {
        if (reported && !*reported) {
            *reported = 1;
            kv_log("INPUT: the %s action does not exist at all", name);
        }
        return;
    }
    memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.action = a;
    memset(&st, 0, sizeof(st)); st.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    r = f_xrGetActionStateVector2f(g_session, &gi, &st);
    if (reported && !*reported) {
        *reported = 1;
        kv_log("INPUT: %s -> %s%s", name,
               XR_FAILED(r) ? "the runtime refused to read it"
                            : (st.isActive ? "ACTIVE (bound to the hardware)"
                                           : "INACTIVE -- the runtime has NOT "
                                             "bound it to anything"),
               XR_FAILED(r) ? "" : "");
    }
    if (XR_FAILED(r)) return;
    if (!st.isActive) return;
    *x = st.currentState.x; *y = st.currentState.y;
}

static void read_vec2(XrAction a, float *x, float *y) {
    read_vec2_named(a, x, y, "stick", NULL);
}

/* The furthest either stick has been pushed all session.  A snapshot every
   few seconds cannot tell "the stick sends nothing" from "the stick happened
   to be centred when I looked", and those need opposite fixes.  A peak cannot
   be missed. */
static float g_move_peak, g_look_peak;

static void note_peak(float x, float y, float *peak) {
    float m = (x < 0 ? -x : x);
    float n = (y < 0 ? -y : y);
    if (n > m) m = n;
    if (m > *peak) *peak = m;
}

static void sync_actions(void) {
    XrActionsSyncInfo si;
    XrActiveActionSet active;
    if (!g_actions_ready) return;
    memset(&active, 0, sizeof(active));
    active.actionSet = g_actions;
    active.subactionPath = XR_NULL_PATH;
    memset(&si, 0, sizeof(si));
    si.type = XR_TYPE_ACTIONS_SYNC_INFO;
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(f_xrSyncActions(g_session, &si))) return;

    {
        /* NOT on the first sync.  An action reads INACTIVE until the runtime
           has bound the profile and the session has focus, so reporting it
           immediately says "unbound" about a control that works perfectly --
           which it did, and sent me chasing the wrong thing.  Wait until the
           session has settled. */
        static int said_move, said_look, settle;
        int *pm = (++settle > 200) ? &said_move : NULL;
        int *pl = (settle > 200) ? &said_look : NULL;
        read_vec2_named(a_move, &g_vrin.move_x, &g_vrin.move_y,
                        "left thumbstick (move)", pm);
        read_vec2_named(a_look, &g_vrin.look_x, &g_vrin.look_y,
                        "right thumbstick (look)", pl);
    }
    g_vrin.select = read_bool(a_select);
    g_vrin.back   = read_bool(a_back);
    g_vrin.menu   = read_bool(a_menu);
    g_vrin.grip_l = read_float(a_gripL);
    g_vrin.grip_r = read_float(a_gripR);
    g_vrin.trig_l = read_float(a_trigL);
    g_vrin.stick_click_l = read_bool(a_stickL);
    g_vrin.stick_click_r = read_bool(a_stickR);
    g_vrin.btn_a  = read_bool(a_btna);
    g_vrin.btn_b  = read_bool(a_btnb);
    g_vrin.btn_x  = read_bool(a_btnx);
    g_vrin.btn_y  = read_bool(a_btny);
    /* THE LEFTORIUM (left-handed play), 24 Sep 2026. Swapped here, once, as
       the controllers are read, so everything downstream -- keys, cursor,
       pointer -- follows without knowing: the cursor stick, the click
       trigger, A/B and the pointing hand move to the left controller;
       walking, X/Y and the other trigger to the right. The grips and the
       stick clicks swap with them. The menu button exists on the left
       controller only and stays where it is. The aim hand is swapped where
       it is located, in vr_prepare_frame. */
    if (g_vrcfg.leftorium) {
        float tf; int ti;
        tf = g_vrin.move_x; g_vrin.move_x = g_vrin.look_x; g_vrin.look_x = tf;
        tf = g_vrin.move_y; g_vrin.move_y = g_vrin.look_y; g_vrin.look_y = tf;
        {   /* select is the right trigger read as a button; trig_l is the
               left trigger as a value, used at 0.5. Swap them through that. */
            int lt = g_vrin.trig_l > 0.5f;
            g_vrin.trig_l = g_vrin.select ? 1.0f : 0.0f;
            g_vrin.select = lt;
        }
        tf = g_vrin.grip_l; g_vrin.grip_l = g_vrin.grip_r; g_vrin.grip_r = tf;
        ti = g_vrin.stick_click_l; g_vrin.stick_click_l = g_vrin.stick_click_r;
        g_vrin.stick_click_r = ti;
        ti = g_vrin.btn_a; g_vrin.btn_a = g_vrin.btn_x; g_vrin.btn_x = ti;
        ti = g_vrin.btn_b; g_vrin.btn_b = g_vrin.btn_y; g_vrin.btn_y = ti;
    }
    note_peak(g_vrin.move_x, g_vrin.move_y, &g_move_peak);
    note_peak(g_vrin.look_x, g_vrin.look_y, &g_look_peak);
    g_vrin.valid  = 1;
    publish_input();
    if (g_vrin.move_x || g_vrin.move_y || g_vrin.look_x || g_vrin.look_y ||
        g_vrin.select || g_vrin.menu || g_vrin.btn_a ||
        g_vrin.btn_b || g_vrin.btn_x || g_vrin.btn_y)
        g_vrin.seen = 1;
}

void vr_input_report(void) {
    if (!g_actions_ready) {
        kv_log("  controllers: action set not attached");
        return;
    }
    kv_log("  controllers: move(%.2f %.2f) look(%.2f %.2f) select=%d back=%d "
           "menu=%d ABXY=%d%d%d%d grip(%.2f %.2f) peak(L %.2f R %.2f)%s",
           g_vrin.move_x, g_vrin.move_y,
           g_vrin.look_x, g_vrin.look_y, g_vrin.select, g_vrin.back,
           g_vrin.menu, g_vrin.btn_a, g_vrin.btn_b, g_vrin.btn_x,
           g_vrin.btn_y, g_vrin.grip_l, g_vrin.grip_r,
           g_move_peak, g_look_peak,
           g_vrin.seen ? "" : "  <- nothing seen yet; press something");
}

/* ---- the frame --------------------------------------------------------- */
void vr_prepare_frame(HDC hdc) {
    XrFrameWaitInfo fwi;
    XrFrameState fs;
    XrFrameBeginInfo fbi;
    XrViewLocateInfo vli;
    XrViewState vs;
    uint32_t n = 0, i;

    if (!g_vrcfg.enabled || g_failed) return;

    if (!g_instance) {
        HGLRC rc;
        if (!bind_gl_ext()) { g_failed = 1; return; }
        rc = p_wglGetCurrentContext();
        if (!rc) return;
        if (!vr_start(hdc, rc)) {
            kv_log("VR: bring-up failed; the game continues flat");
    report_xr_environment();
            vr_stop();
            g_failed = 1;
            return;
        }
    }

    poll_events();
    g_views_valid = 0;
    if (g_failed || !g_running) return;

    sync_actions();
    end_open_frame_with_nothing();

    memset(&fwi, 0, sizeof(fwi));
    fwi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&fs, 0, sizeof(fs));
    fs.type = XR_TYPE_FRAME_STATE;
    if (XR_FAILED(f_xrWaitFrame(g_session, &fwi, &fs))) { g_fail_wait++; return; }
    g_predicted = fs.predictedDisplayTime;

    memset(&fbi, 0, sizeof(fbi));
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (XR_FAILED(f_xrBeginFrame(g_session, &fbi))) { g_fail_begin++; return; }
    g_frame_open = 1;

    if (!fs.shouldRender || !g_stereo) return;

    for (i = 0; i < g_nview; i++) {
        memset(&g_view[i], 0, sizeof(g_view[i]));
        g_view[i].type = XR_TYPE_VIEW;
    }
    memset(&vli, 0, sizeof(vli));
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_space;
    memset(&vs, 0, sizeof(vs));
    vs.type = XR_TYPE_VIEW_STATE;
    n = g_nview;
    /* Where the right controller points, in the same space and at the
       same instant as the views.  Located here rather than in the input
       sync so that it shares the predicted display time -- a pose from a
       different instant puts the cursor where the hand used to be. */
    {
    /* The Leftorium points with the left hand. */
    XrSpace aim = (g_vrcfg.leftorium && g_aim_space_l) ? g_aim_space_l : g_aim_space;
    if (aim && f_xrLocateSpace) {
        XrSpaceLocation sl;
        memset(&sl, 0, sizeof(sl));
        sl.type = XR_TYPE_SPACE_LOCATION;
        g_vrin.aim_valid = 0;
        if (XR_SUCCEEDED(f_xrLocateSpace(aim, g_space,
                                         g_predicted, &sl)) &&
            (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            const XrQuaternionf *q = &sl.pose.orientation;
            /* An aim pose points along its own -Z. */
            g_vrin.aim_pos[0] = sl.pose.position.x;
            g_vrin.aim_pos[1] = sl.pose.position.y;
            g_vrin.aim_pos[2] = sl.pose.position.z;
            g_vrin.aim_dir[0] = -2.0f * (q->x * q->z + q->w * q->y);
            g_vrin.aim_dir[1] = -2.0f * (q->y * q->z - q->w * q->x);
            g_vrin.aim_dir[2] = -(1.0f - 2.0f * (q->x * q->x +
                                                 q->y * q->y));
            g_vrin.aim_valid = 1;
        }
    }
    }

    if (XR_FAILED(f_xrLocateViews(g_session, &vli, &vs, n, &n, g_view))) {
        g_fail_locate++;
        return;
    }
    /* Leave g_views_valid clear rather than drawing from a pose that is not
       tracked: the proxy then draws the frame mono, which is wrong but stable,
       instead of stereo built on garbage. */
    if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return;
    g_views_valid = 1;
}

/* Where the submit half of the frame actually goes.  Accumulated in
   ticks, drained by vr_finish_breakdown once per report. */
static LONGLONG g_ft_acq, g_ft_wait, g_ft_blit, g_ft_end;
static LARGE_INTEGER g_ft_qpf;

#define FT_BEGIN(v) LARGE_INTEGER v; QueryPerformanceCounter(&v)
#define FT_END(v, acc) do { LARGE_INTEGER _e; QueryPerformanceCounter(&_e); \
                            (acc) += _e.QuadPart - (v).QuadPart; } while (0)

/* ms per frame for each stage, then reset.  `frames` is how many swaps the
   figures cover. */
/* The window size as of the last submitted frame.  Recorded rather than
   queried, so the number in the report is the one the frame actually
   used. */
static int g_last_win_w, g_last_win_h;

void vr_eye_and_window(int *eyew, int *eyeh, int *winw, int *winh) {
    *eyew = g_eyew;
    *eyeh = g_eyeh;
    *winw = g_last_win_w;
    *winh = g_last_win_h;
}

void vr_finish_breakdown(unsigned frames, double *acq, double *wait,
                         double *blit, double *end) {
    double k;
    if (!g_ft_qpf.QuadPart) QueryPerformanceFrequency(&g_ft_qpf);
    k = (frames && g_ft_qpf.QuadPart)
            ? 1000.0 / (double)g_ft_qpf.QuadPart / (double)frames : 0.0;
    *acq = g_ft_acq * k;
    *wait = g_ft_wait * k;
    *blit = g_ft_blit * k;
    *end = g_ft_end * k;
    g_ft_acq = g_ft_wait = g_ft_blit = g_ft_end = 0;
}

void vr_finish_frame(HDC hdc) {
    XrFrameEndInfo fei;
    XrSwapchainImageAcquireInfo ai;
    XrSwapchainImageWaitInfo wi;
    XrSwapchainImageReleaseInfo ri;
    XrCompositionLayerProjection proj;
    XrCompositionLayerProjectionView pv[MAXEYE];
    XrCompositionLayerQuad quad;
    XrCompositionLayerQuad eyequad[MAXEYE];
    const XrCompositionLayerBaseHeader *layers[MAXEYE];
    uint32_t idx = 0, i;
    int gw, gh;

    if (!g_vrcfg.enabled || g_failed || !g_running || !g_frame_open) return;

    memset(&fei, 0, sizeof(fei));
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = g_predicted;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = 0;

    client_size(hdc, &gw, &gh);
    g_last_win_w = gw;
    g_last_win_h = gh;

    if (g_stereo && g_views_valid) {
        memset(pv, 0, sizeof(pv));
        for (i = 0; i < g_nview; i++) {
            memset(&ai, 0, sizeof(ai));
            ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            {
                FT_BEGIN(t0);
                if (XR_FAILED(f_xrAcquireSwapchainImage(g_eye_sc[i], &ai,
                                                        &idx))) {
                    g_fail_acquire++;
                    end_open_frame_with_nothing();
                    return;
                }
                FT_END(t0, g_ft_acq);
            }
            memset(&wi, 0, sizeof(wi));
            wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
            wi.timeout = XR_INFINITE_DURATION;
            {
                FT_BEGIN(t1);
                f_xrWaitSwapchainImage(g_eye_sc[i], &wi);
                FT_END(t1, g_ft_wait);
            }

            {
                FT_BEGIN(t2);
                blit_to_swapchain(g_eye_img[i][idx].image,
                                  (int)i * g_scenew, 0, g_scenew, g_sceneh,
                                  g_eyew, g_eyeh);
                FT_END(t2, g_ft_blit);
            }

            memset(&ri, 0, sizeof(ri));
            ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            f_xrReleaseSwapchainImage(g_eye_sc[i], &ri);

            /* The declared pose and fov must be the ones the image was drawn
               with, or the compositor's reprojection adds an error of its own.
               These are exactly what vr_eye_projection used. */
            pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[i].pose = g_view[i].pose;
            pv[i].fov = g_view[i].fov;
            /* Declare the fov the image was actually drawn with, whatever the
               auto fit settled on this frame.  A declared fov that disagrees
               with the rendered one is a reprojection error the compositor
               applies faithfully every frame. */
            /* When filling the view we deliberately declare a WIDER fov than
               we rendered.  The compositor then stretches the engine's own
               73x45 picture across the whole headset: magnified, but with no
               culled holes and no black surround. */
            /* Declare the rendered frustum, magnified uniformly.  Declaring
               more than was rendered is what stretches the picture; keeping
               the factor the same on both axes is what stops it distorting. */
            {
                float sx = g_fov_sx * g_mag, sy = g_fov_sy * g_mag;
                if (sx > 1.0f) sx = 1.0f;
                if (sy > 1.0f) sy = 1.0f;
                pv[i].fov.angleLeft  = (float)atan(tan(g_view[i].fov.angleLeft)  * sx);
                pv[i].fov.angleRight = (float)atan(tan(g_view[i].fov.angleRight) * sx);
                pv[i].fov.angleUp    = (float)atan(tan(g_view[i].fov.angleUp)    * sy);
                pv[i].fov.angleDown  = (float)atan(tan(g_view[i].fov.angleDown)  * sy);
            }
            pv[i].subImage.swapchain = g_eye_sc[i];
            pv[i].subImage.imageRect.offset.x = 0;
            pv[i].subImage.imageRect.offset.y = 0;
            pv[i].subImage.imageRect.extent.width = g_eyew;
            pv[i].subImage.imageRect.extent.height = g_eyeh;
            pv[i].subImage.imageArrayIndex = 0;
        }
        if (g_vrcfg.screen_mode) {
            /* A world-locked stereo window.  Both eyes see the same rectangle
               in space, each showing its own image, so identical content lands
               at the window's distance and fuses by construction -- which is
               exactly what the menus need.  The world's own depth comes from
               the disparity between the two images. */
            float aspect = (g_eng_tany > 0.0f) ? (g_eng_tanx / g_eng_tany) : 1.7778f;
            memset(eyequad, 0, sizeof(eyequad));
            for (i = 0; i < g_nview; i++) {
                eyequad[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                eyequad[i].space = g_space;
                eyequad[i].eyeVisibility = (i == 0) ? XR_EYE_VISIBILITY_LEFT
                                                    : XR_EYE_VISIBILITY_RIGHT;
                eyequad[i].subImage.swapchain = g_eye_sc[i];
                eyequad[i].subImage.imageRect.extent.width = g_eyew;
                eyequad[i].subImage.imageRect.extent.height = g_eyeh;
                eyequad[i].pose.orientation.w = 1.0f;
                eyequad[i].pose.position.z = -g_vrcfg.screen_distance;
                eyequad[i].size.width = g_vrcfg.screen_width;
                eyequad[i].size.height = g_vrcfg.screen_width / aspect;
                layers[i] = (const XrCompositionLayerBaseHeader *)&eyequad[i];
            }
            fei.layerCount = g_nview;
            fei.layers = layers;
        } else {
            memset(&proj, 0, sizeof(proj));
            proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            proj.space = g_space;
            proj.viewCount = g_nview;
            proj.views = pv;
            layers[0] = (const XrCompositionLayerBaseHeader *)&proj;
            fei.layerCount = 1;
            fei.layers = layers;
        }
    } else if (!g_stereo && g_quad_sc) {
        float aspect = (g_quad_h > 0) ? (float)g_quad_w / (float)g_quad_h : 1.7778f;
        memset(&ai, 0, sizeof(ai));
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        if (XR_FAILED(f_xrAcquireSwapchainImage(g_quad_sc, &ai, &idx))) {
            g_fail_acquire++;
            end_open_frame_with_nothing();
            return;
        }
        memset(&wi, 0, sizeof(wi));
        wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        wi.timeout = XR_INFINITE_DURATION;
        f_xrWaitSwapchainImage(g_quad_sc, &wi);

        blit_to_swapchain(g_quad_img[idx].image, 0, 0, gw, gh,
                          g_quad_w, g_quad_h);

        memset(&ri, 0, sizeof(ri));
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        f_xrReleaseSwapchainImage(g_quad_sc, &ri);

        /* A quad layer IS the placement: its texture is painted flat and the
           quad's own size carries the picture's aspect. */
        memset(&quad, 0, sizeof(quad));
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.space = g_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_quad_sc;
        quad.subImage.imageRect.extent.width = g_quad_w;
        quad.subImage.imageRect.extent.height = g_quad_h;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -g_vrcfg.quad_distance;
        quad.size.width = g_vrcfg.quad_width;
        quad.size.height = g_vrcfg.quad_width / aspect;
        layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
        fei.layerCount = 1;
        fei.layers = layers;
    }

    {
        FT_BEGIN(t3);
        if (XR_FAILED(f_xrEndFrame(g_session, &fei))) {
            FT_END(t3, g_ft_end);
            goto submit_failed;
        }
        FT_END(t3, g_ft_end);
    }
    goto submitted;

submit_failed:
    {
        if (!g_fail_submit && g_vrcfg.screen_mode)
            kv_log("VR: the runtime refused the frame.  Per-eye quad layers are "
                   "not supported by every runtime; if every frame fails, set "
                   "screen_mode = 0 in keflings_vr.ini.");
        g_fail_submit++;
    }
submitted:
    g_frame_open = 0;
    g_vr_frames++;

    if (g_vr_frames == 120)
        kv_log("VR: fitted eye FOV to the engine: scale x=%.3f y=%.3f "
               "(engine tan %.4f x %.4f)", g_fov_sx, g_fov_sy,
               g_eng_tanx, g_eng_tany);
    if ((g_vr_frames % 900) == 0)
        kv_log("VR: %u frames (fail: wait=%u begin=%u acquire=%u submit=%u "
               "locate=%u)", g_vr_frames, g_fail_wait, g_fail_begin,
               g_fail_acquire, g_fail_submit, g_fail_locate);
}
