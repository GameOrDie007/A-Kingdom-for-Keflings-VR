/* A Kingdom for Keflings VR -- OpenXR side. */
#ifndef KEFLINGS_VR_H
#define KEFLINGS_VR_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Called from the proxy, always on the thread that owns the GL context. */
void vr_config_load(void);
void vr_on_make_current(HDC hdc, HGLRC rc);
void vr_on_delete_context(HGLRC rc);

/* Called from wglSwapBuffers BEFORE the real swap, while the back buffer
   still holds the frame the game just drew: copies each eye's half out and
   submits the frame. */
void vr_finish_frame(HDC hdc);
/* Where the submit half of the frame went, in ms per swap, then reset.
   Four separate stories were competing for one timer; this settles it. */
void vr_finish_breakdown(unsigned frames, double *acq, double *wait,
                         double *blit, double *end);
void vr_eye_and_window(int *eyew, int *eyeh, int *winw, int *winh);

/* Called from wglSwapBuffers AFTER the real swap, to wait on the compositor
   and locate the views for the frame the game is ABOUT to draw.  The head pose
   has to be known before the draws happen, not after. */
void vr_prepare_frame(HDC hdc);

/* True once views are located and the eye projections are valid, i.e. the
   proxy should be duplicating draws this frame. */
int  vr_stereo_active(void);
int  vr_eye_count(void);
int  vr_eye_width(void);
int  vr_eye_height(void);

/* Eye viewport within the game's own back buffer (side-by-side). */
void vr_eye_viewport(int eye, int *x, int *y, int *w, int *h);

/* The eye's projection, with the head pose folded in, ready to be loaded
   straight into GL_PROJECTION.  near/far come from the pass the engine set up,
   so its own depth range is preserved. */
void vr_eye_projection(int eye, double znear, double zfar, float *m16);

/* The engine's own half-angle tangents, taken from the projection it just
   built.  With fov_scale = 0 (auto) the eye frustum is fitted inside these, so
   nothing the engine culled can fall inside what we ask the headset to show. */
void vr_set_engine_fov(float tan_x, float tan_y);
/* The engine frustum the game unprojects the cursor through. */
float vr_engine_tanx(void);
float vr_engine_tany(void);

/* The vertical field of view, in degrees, the ENGINE needs so that nothing it
   culls could have been visible in this headset.  Derived from the runtime's
   own reported per-eye angles, so it is exactly wide enough and not a degree
   more -- every extra degree is geometry drawn and then never seen. */
float vr_needed_engine_fovy(void);
void  vr_cull_coverage_report(double engine_near);
extern float g_head_dev_deg;   /* how far your head is off the camera axis */
extern int   g_fov_clamped;    /* 1 = turned too far to cover completely */

/* Horizontal NDC shift that puts screen-space 2D at hud_distance metres
   instead of at infinity.  Identical content in both eyes has zero disparity,
   which the brain reads as infinitely far -- fine on its own, and a hard
   conflict when it sits in front of a world that is an arm's length away. */
float vr_hud_ndc_shift(int eye);

/* Build the projection for the engine's 2D pass: place its canvas as a panel
   of a chosen angular width, with the canvas's OWN aspect, centred on the
   eye's forward axis, converged at hud_distance.  The engine hands its 2D in
   canvas-NDC (its ortho lives in the modelview), so this maps [-1,1] square
   onto that panel. */
void vr_hud_projection(int eye, float canvas_aspect, float size_deg,
                       int world_lock,
                       float *m16);

/* Non-zero when the eyes are presented on a world-locked stereo window rather
   than as a projection layer.  In that mode the render must keep the ENGINE's
   own camera direction and frustum exactly -- head rotation looks AROUND the
   window instead of through it -- because the engine culls to its own camera
   and nothing can make it draw what it has already thrown away. */
int   vr_screen_mode(void);

/* This eye's sideways offset in game world units, and the distance at which
   the two eyes should agree (also world units).  The proxy applies both to the
   engine's own projection. */
float vr_eye_offset_world(int eye);
float vr_converge_world(void);

/* What the Quest controllers are doing this frame. */
typedef struct {
    float move_x, move_y;      /* left thumbstick  */
    float look_x, look_y;      /* right thumbstick */
    int   select, back, menu;
    int   stick_click_l, stick_click_r;  /* zoom out / zoom in */
    float trig_l;          /* the left trigger, on its own: it is the
                              right mouse button, not a second select */
    int   btn_a, btn_b, btn_x, btn_y;   /* the four face buttons */
    float grip_l, grip_r;
    /* Where the right controller points, in the same space the views
       are located in.  aim_valid is 0 when it is not tracked -- set
       down, out of range -- and the pointer must fall back rather than
       fling the cursor at a stale pose. */
    int   aim_valid;
    float aim_pos[3];
    float aim_dir[3];
    int   valid, seen;
} VrInput;
extern VrInput g_vrin;
void vr_input_report(void);
/* The 2D panel as vr_hud_projection actually drew it, and the head it is
   anchored to.  Both return 0 if there is nothing valid yet. */
/* The panel as last drawn.  half_w and half_h are METRES when it is world
   locked and tangents when it is head locked, and distance says which:
   a positive distance means world locked. */
int  vr_panel_geometry(float *half_w, float *half_h, float *distance);
int  vr_head_pose(float *pos, float *quat);
int  vr_cull_rotation(float *m16);
void vr_mirror_present(HDC hdc);
void vr_bind_scene_target(void);
void vr_panel_anchor_frame(int panel_up);  /* ONCE per frame, not per eye */
void vr_recenter(const char *why, int delay_frames);  /* re-anchor the menus in front */
float vr_panel_tilt(void);                     /* menu_tilt, radians */
float vr_panel_height(void);                   /* menu_height, metres */
int  vr_panel_anchor(float *yaw, float *pos);   /* where the panel stands */
void vr_gpu_frame_begin(void);
void vr_gpu_frame_end(void);
double vr_gpu_ms(void);          /* mean GPU ms/frame since the last call */
void vr_gpu_frame_begin(void);
void vr_gpu_frame_end(void);
double vr_gpu_ms(void);          /* mean GPU ms/frame since the last call */
/* The 2D panel, rendered once at canvas resolution instead of twice at eye
   resolution. vr_panel_begin returns 0 if the caller must fall back to
   drawing per eye. */
int  vr_panel_begin(int w, int h, int clear);
void vr_panel_end(void);
void vr_panel_probe(float fx, float fy);
int  vr_panel_tex(void);
int  vr_panel_is_bound(void);
void vr_panel_size(int *w, int *h);

void vr_check_scene_target(unsigned frame);
/* Plain types: this header is included where gl.h is not. */
unsigned vr_scene_fbo(void);          /* 0 = drawing into the back buffer */
unsigned vr_map_buffer(unsigned buf); /* GL_BACK -> our attachment */

void vr_shutdown(void);

/* settings, read from keflings_vr.ini beside the exe */
typedef struct {
    int   enabled;
    float quad_distance;   /* metres in front of the player */
    float quad_width;      /* metres */
    int   srgb_swapchain;  /* 1 = declare the swapchain sRGB (correct for
                              this game); 0 to A/B it without a rebuild */
    int   stereo;          /* 0 = stage 1 flat screen, 1 = stereo diorama */
    float world_scale;     /* game world units per real metre.  THE diorama
                              knob: larger makes the kingdom read as a smaller
                              model nearer your face, because the eyes are
                              further apart in world units. */
    float fill_amount;     /* 0..1.  How much of the gap between what the
                              engine draws (73x45) and what the headset shows
                              is closed by magnifying the image.  0 = exact,
                              letterboxed.  1 = as large as it can go without
                              distorting.  The stretch is always UNIFORM: an
                              axis-by-axis stretch is anamorphic and reads as
                              the world being the wrong shape, not merely
                              zoomed. */
    float menu_dim;        /* 0..1, how dark to make the view OUTSIDE the
                              panel while a menu is up, so the game's own
                              backdrop does not stop at the panel edge.
                              0 turns it off. */
    int   menu_dim_wide_search; /* 1 = also inspect vertex-array draws for
                              the backdrop, and accept a translucent shape
                              covering the whole canvas whatever its colour.
                              Unverified; off by default. */
    int   menu_dim_alpha_only; /* 1 = also treat a menu whose darkest
                              translucent draw has a usable ALPHA as dimmed,
                              even when its colour is not dark. Some screens
                              (the blueprint details page) draw a dark texture
                              modulated by a WHITE vertex colour, so the only
                              colour the port can read is white and the
                              darkness is somewhere it cannot look. Off by
                              default: darkening a screen the game meant to
                              leave bright is worse than leaving one alone. */
    int   menu_dim_hold;   /* 1 = keep dimming a full-screen menu after the
                              game stops drawing its own backdrop, which it
                              does on the way back out of a details page.
                              Only ever extends a dim already found. */
    int   panel_flat_test; /* DIAGNOSTIC. Capture the 2D pass and blit it
                              straight back over the window at 1:1, with no
                              headset. A correct capture is pixel-identical
                              to the feature being off, so this is the A/B
                              that says whether the blend is right. */
    int   panel_once;      /* 1 = draw the 2D pass ONCE into a canvas-sized
                              target and blit it to each eye, instead of
                              re-issuing every 2D draw per eye at eye
                              resolution. A menu is ~2100 blocks. */
    int   stereo_pass_list; /* DIAGNOSTIC. 1 = record each scene pass into
                              one display list and replay it for eye 1.
                              Measured SLOWER: list compilation is ~10 ms a
                              frame on this driver. */
    int   skip_noop;       /* Skip world draws that provably change nothing:
                              lit, alpha-tested GREATER, MODULATE, with an
                              alpha source of exactly 0 -- the engine's
                              invisible highlight overlays. GPU-verified by
                              occlusion queries before it trusts itself.
                              0 off, 1 on, 2 alternate (A/B). */
    int   desk_stereo;     /* DIAGNOSTIC. 1 = with no VR session, duplicate
                              every draw exactly as the headset path does,
                              each eye half the window -- the two-eye CPU
                              cost, measurable at the desk. */
    int   merge_group;     /* STAGE 3. 0 = off. 1 = AUDIT: choose the
                              largest opaque state group, check every member
                              shares the state the key does not cover, and
                              draw it both ways offscreen to compare pixels
                              -- the screen does not change. 2 = MERGE: that
                              group is drawn from one pre-built buffer. */
    int   leftorium;       /* 1 = left-handed: the hands swap as they are read */
    float merge_offset;    /* glPolygonOffset on merged draws, 0 = off: a
                              layer drawn exactly on top of merged scenery
                              (snow) must not lose to its CPU-computed depth. */
    int   mv_shadow;       /* DIAGNOSTIC (stage 2). 1 = track the modelview
                              in software instead of asking GL for it, check
                              it once against GL, and measure how much of the
                              world is in the same place as it was last
                              frame. Changes nothing you can see. */
    int   mesh_capture;    /* DIAGNOSTIC (stage 1). 1 = keep our own copy of
                              every display list's geometry, read out of the
                              engine's client arrays as it compiles the list.
                              Observe only: everything is still forwarded and
                              the picture cannot move. Reports vertices per
                              frame and list re-compiles per frame, which are
                              what the merging design needs to know. */
    int   group_census;    /* DIAGNOSTIC. 1 = count the distinct state
                              groups and display lists in each frame's world
                              pass. This is what decides whether a batching
                              renderer is worth building: consecutive draws
                              never share state, but that is the engine's
                              emission order, and a batcher sorts first. Its
                              own key, so it can run without `diagnostics`
                              and the frame stalls that brings. */
    float engine_aspect;   /* Narrow the engine's own camera aspect, which
                              is what it culls horizontally with. 0 = leave
                              it alone. Its 16:9 shape makes a headset-tall
                              frustum 141 degrees wide where 116 covers the
                              eye. */
    int   engine_tanx_ab;  /* DIAGNOSTIC. 1 = alternate engine_tanx on and
                              off every 300 frames and report the draws a
                              frame for each. Cross-launch comparison is
                              useless here: the scene is never identical. */
    float engine_tanx;     /* Narrow the engine's HORIZONTAL culling to this
                              tangent. It builds its frustum from one fovy
                              and its own 16:9 aspect, so covering the eye
                              vertically over-covers it horizontally by more
                              than two to one -- geometry submitted that
                              nobody can see. 0 = leave it alone. */
    int   state_filter;    /* 1 = drop GL state calls that set the value
                              already set. The engine makes ~14 of them
                              between every two draws and re-sets most to
                              what they already are. */
    int   diagnostics;     /* 1 = run the development scaffolding: module and
                              camera dumps, the GL census, and a full frame
                              trace when a menu opens. All of it walks memory
                              or writes files ON THE RENDER THREAD and costs
                              hundreds of milliseconds. Off in a normal
                              build. */
    float stall_ms;        /* frames longer than this are recorded, and the
                              worst few printed with their own split in the
                              next frame report. */
    int   cull_follow_head; /* 1 = put the head's POSITION into the matrix
                              the engine culls with, as head_cull already
                              does for its rotation, and take it back out of
                              the eye projection. Without it the engine culls
                              from its own camera while the eye is hundreds
                              of game units away, and near geometry at the
                              edge of view is dropped. */
    int   skip_glfinish;   /* 1 = swallow the engine's once-per-frame
                              glFinish, which otherwise stops the CPU until
                              the GPU has drained and prevents the two from
                              overlapping at all. */
    int   block_arrays;    /* 1 = capture each glBegin/glEnd block into a CPU
                              buffer and draw it as one array draw per eye,
                              instead of forwarding ~150k vertex calls a
                              frame and compiling a display list per block. */
    int   dup_alternate;   /* 1 = alternate the eye order per draw and defer
                              the restore of the engine's state, so each
                              duplicated draw costs one state switch, not
                              three. */
    int   dup_profile;     /* DIAGNOSTIC. 1 = time every duplicated draw's
                              parts with QueryPerformanceCounter, ~8 reads a
                              draw, about 0.5 ms a frame of instrument. */
    int   mirror_eye;      /* DIAGNOSTIC. 1 = the desktop mirror shows the
                              RIGHT eye instead of the left. */
    int   block_census;    /* DIAGNOSTIC. 1 = count every GL call made inside
                              glBegin/glEnd, by name, and the primitive
                              modes and block sizes. */
    int   menu_dim_hold_force; /* DIAGNOSTIC. 1 = paint the held dim's canvas
                              fill on every full-screen menu, so the held
                              picture can be looked at without driving
                              through blueprints -> details -> back. With
                              menu_dim_probe the fill is green and the
                              surround blue. */
    int   menu_dim_measure; /* 1 = derive the menu dimming by sampling the
                              view before and after the 2D pass, rather than
                              by trying to recognise the draw. */
    int   dim_trace;       /* DIAGNOSTIC. 1 = list every translucent draw in
                              a menu's 2D pass: colour, alpha, coverage and
                              which draw path it arrived by. */
    int   menu_dim_probe;  /* DIAGNOSTIC. 1 = paint the surround in blue and
                              log the rectangle, to see whether it drew and
                              where. */
    int   key_sweep;       /* DIAGNOSTIC. 1 = the Y button sends the next
                              candidate key instead of its normal binding,
                              and logs which one. For finding an action whose
                              key is not documented anywhere. */
    int   hud_ab;          /* DIAGNOSTIC.  1 = alternate the 2D layer between
                              both eyes and the left eye only, every 300
                              frames, and report the frame rate for each.
                              The 2D is missing from one eye half the time,
                              so this is not a setting to play with. */
    int   dump_strings;    /* 1 = walk the address space and dump the
                              engine string table.  Off by default: it
                              stalls the render thread for seconds. */
    float engine_fov;      /* degrees. 0 = leave the engine alone. Anything
                              else is written over its own camera so it
                              DRAWS and CULLS to a wider view -- the only
                              thing that can remove the holes. */
    int   emulate_input;   /* 1 = drive the game with the Quest
                              controllers, as keyboard and mouse.
                              The game has no gamepad code left, so
                              this is the only route in. */
    float stick_deadzone;
    float mouse_speed;     /* pixels per frame at full stick */
    int   controllers;     /* 1 = this port binds the Quest controllers
                              through OpenXR.  0 = leave them alone, so
                              Virtual Desktop's own gamepad emulation can
                              present them as an Xbox pad, which the game
                              already supports natively. */
    int   scissor_mode;    /* 0 = ignore the engine scissor while drawing 2D
                              per eye (safe: nothing can be clipped away).
                              1 = map it through the panel transform (exact
                              when right, hides content when wrong). */
    int   move_arrows;     /* 1 = the left stick sends the arrow keys as
                              well as WASD, since which pair moves the
                              giant is not yet known. */
    int   key_post;        /* 1 = post key messages straight to the game
                              window. 0 = inject them into the system
                              input stream and let Windows route them,
                              which measured as reaching nothing. */
    int   fov_both_ends;   /* 1 = widen the engine camera at the end of
                              the frame too, not only when it builds its
                              projection.  The engine resets the value
                              every frame. */
    int   head_cull;       /* 1 = turn the engine view by the head so its
                              culling follows the gaze, cancelling the
                              rotation in the eye transform so the picture
                              does not move. */
    int   cursor_sync;     /* 1 = once, shortly after the level is up, drive
                              the cursor into the top-left corner so the
                              game's own cursor clamps there too and the
                              offset between them is squeezed out. Only
                              meaningful while the canvas and the window are
                              the same size. */
    int   cursor_lock;     /* 1 = hold the cursor at one point on the
                              screen, the way the Xbox version has no
                              cursor at all and highlights what is in
                              front of you.  The mouse is ignored. */
    float cursor_lock_x;   /* where to hold it, as a fraction of the
                              window: 0.5, 0.5 is the middle. */
    float cursor_lock_y;
    int   input_mode;      /* 0 = whichever device moved last owns the
                              cursor.  1 = CONTROLLER: the ray always owns
                              it and the physical mouse is ignored.
                              2 = MOUSE AND KEYBOARD: the controller never
                              touches the cursor.
                              Click both thumbsticks together to switch
                              between 1 and 2 without leaving the game. */
    int   pointer_space;   /* what the aimed cursor is measured against.
                              0 = the 2D panel: pointing at UI is exact, and
                              the world highlight runs ahead of the pointer by
                              however much wider the engine camera is.
                              1 = the engine's camera: the world highlight
                              lands where you point, and UI is off by the same
                              ratio the other way. They agree only when the
                              panel is as wide as the engine frustum. */
    int   pointer_mode;    /* 1 = the cursor goes where the right
                              controller points. 0 = the old relative
                              nudge from the right stick. */
    int   hud_world_lock;  /* 1 = the in-game 2D screen stands fixed in
                              space, so the cursor and the highlight box
                              it drives stay together at any head angle.
                              0 = it follows the head, which separates
                              them.  Menus always follow the head. */
    float pointer_yield_ms; /* how long the physical mouse keeps the cursor
                               after it stops moving.  0 = the controller
                               always has it. */
    float pointer_smooth;  /* 0..1; how much of the previous position
                              to keep.  A raw aim pose is jittery at
                              the distance of a cursor. */
    float engine_fov_max;  /* ceiling on how wide the engine camera may be
                              driven.  Wider covers more head turn and costs
                              frames; this is that trade in one number. */
    float draw_distance;   /* multiplier on the engine's own far plane.
                              1 = untouched.  The engine draws to 2200 game
                              units and stops, which reads as a bubble of
                              world that travels with you. */
    float mirror_fps;      /* how often to present the desktop window.  A
                              windowed present is absorbed at the desktop's
                              refresh whatever swap interval was asked for,
                              so presenting every VR frame keeps the queue
                              permanently full and pins the game to the
                              monitor.  Must be well UNDER the desktop rate:
                              a cap AT it leaves no slack and re-creates the
                              stall. */
    int   force_vsync;     /* 1 = hold the swap interval at 0 so the
                              compositor paces frames.  A control: the
                              only thing that changed the game frame
                              rate from 60 to 90. */
    int   screen_mode;     /* 1 = world-locked stereo window (default) */
    float screen_distance; /* metres to the window */
    float screen_width;    /* metres across */
    float converge;        /* world units at which the eyes agree */
    float hud_size;        /* angular WIDTH of the 2D panel in game, deg */
    int   menu_draws;      /* 2D draws per frame at or above which the
                              frame counts as a menu rather than the HUD.
                              An absolute figure, because a learned one
                              has nothing to learn from at the main menu,
                              where there is no in-game HUD at all. */
    float menu_size;       /* and when a menu is up.  They need separate
                              values: a size that suits an in-game HUD
                              leaves the menus tiny, because a menu is a
                              small element inside the same full-screen
                              canvas. */
    float menu_height;     /* metres the menu screen is raised, 0 = eye height */
    float menu_tilt;       /* degrees the menu screen leans back (negative: toward you), 0 = upright */
    float hud_distance;    /* metres; where screen-space 2D should converge */
    float fov_scale;       /* 0 = auto: fit the eye frustum inside the FOV the
                              engine itself renders, so its culling can never
                              remove anything we would have shown.  1.0 = the
                              headset's whole FOV (more immersive, but the
                              engine drops geometry at the edges). */
    int   match_canvas_to_monitor; /* 1 = correct the GAME's own
                              XScreenRes/YScreenRes to this monitor before it
                              reads them. The game lays its 2D out for those
                              numbers whether or not its window is that size,
                              and a mismatch misshapes every menu. The
                              original file is backed up once. */
    int   borderless;      /* 1 = restyle the game's window to borderless
                              full screen on the monitor it opened on.  NOT
                              exclusive full screen, which changes the desktop
                              display mode and rearranges every other window
                              the player had open. */
    int   offscreen;       /* 1 = draw the scene into our own target sized
                              from the runtime, instead of into halves of the
                              game's back buffer.  The window then stops
                              deciding the VR resolution. */
    float render_scale;    /* fraction of the runtime's recommended eye size.
                              1.0 = exactly what it asks for. */
    int   mirror_mode;     /* what the DESKTOP window shows.  The eyes are
                              rendered side by side into the game's own back
                              buffer, so presenting it raw shows a squashed
                              stereo pair.  0 = left eye at its own shape,
                              1 = left eye cropped to fill the window,
                              2 = the raw back buffer (both eyes). */
} VrConfig;
extern VrConfig g_vrcfg;

#endif
