/* A Kingdom for Keflings VR -- put the cursor where the right controller points.
 *
 * This is a mouse game, and three of its functions -- What's Next, Save, and
 * the zoom buttons -- exist only as icons you click, with no key behind them.
 * A controller that cannot place a cursor cannot reach them at all.  The
 * cursor was also riding the head: it is drawn in the 2D pass, that pass is a
 * panel anchored to where you are facing, so it swung with the head while a
 * relative stick nudge moved it within the panel.  Nothing about that is
 * aimable.
 *
 * The geometry.  vr_hud_projection draws the 2D canvas as a rectangle one unit
 * in front of the eye, half_w wide and half_h tall in tangent units, centred
 * on the forward axis.  So in head space the panel is the plane z = -1 with
 * x in [-half_w, half_w] and y in [-half_h, half_h].  Bring the controller's
 * aim ray into head space, cross it with that plane, and the crossing point in
 * panel units IS the position on the game's canvas.
 *
 * Then the game does the rest by itself: it unprojects from canvas
 * coordinates, so placing the OS cursor there makes it pick in the world
 * exactly as a mouse would.  None of its picking had to be understood.
 *
 * The panel size is read back from vr_hud_projection rather than recomputed,
 * because the menu and the HUD use different sizes -- a second copy of that
 * calculation would be right for one of them and wrong for the other.
 */
#include "proxy.h"
#include "vr.h"
#include <math.h>

/* Rotate a vector by the inverse of a quaternion: world into head space. */
static void quat_inv_rotate(const float *q, const float *v, float *out) {
    /* inverse of a unit quaternion is its conjugate */
    float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    float tx = 2.0f * (y * v[2] - z * v[1]);
    float ty = 2.0f * (z * v[0] - x * v[2]);
    float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* Move the cursor to a point in the window's client area.  SendInput's
   absolute coordinates are fractions of the WHOLE virtual desktop, which is
   not the same thing as the primary monitor when there is more than one. */
/* The last place WE put the cursor, in screen coordinates.  Anything that
   moves it away from here was not us. */
static POINT g_we_set;
static int   g_report_x, g_report_y, g_report_have;
static int   g_we_set_valid;

static void cursor_to_client(HWND wnd, int cx, int cy) {
    POINT pt;
    INPUT in;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;

    pt.x = cx; pt.y = cy;
    if (!ClientToScreen(wnd, &pt)) return;

    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK;
    /* The -1 is the standard correction: the range maps onto vw-1 pixels, and
       without it the cursor never reaches the right and bottom edges. */
    in.mi.dx = (LONG)(((double)(pt.x - vx) * 65535.0) / (double)(vw - 1));
    in.mi.dy = (LONG)(((double)(pt.y - vy) * 65535.0) / (double)(vh - 1));
    SendInput(1, &in, sizeof(in));

    /* Remember where we asked for it, so a later reading can tell our own
       movement from the physical mouse being picked up. */
    g_we_set = pt;
    g_we_set_valid = 1;
}

/* Who owns the cursor?  Both devices can move it and neither knows about the
   other, so without arbitration the controller yanks it back ninety times a
   second the moment the mouse is touched.

   The first attempt gave it to whichever moved LAST, and that was too eager.
   This machine is driven remotely, so the session receives a trickle of cursor
   updates even when nobody is really moving the mouse -- every one of those
   handed ownership over, and the controller then had to be swung several
   degrees to win it back.  The mouse had to be held perfectly still to point
   at anything.

   So ownership needs two corrections.  It takes a real movement to claim, not
   a single pixel of drift; and it LAPSES on its own once the mouse stops, so
   the controller recovers by default instead of having to fight for it. */
static int mouse_has_it(void) {
    static int  frames_left;        /* how long the mouse keeps it */
    static int  announced;
    POINT now;
    int hold_frames;

    if (!GetCursorPos(&now)) return 0;

    hold_frames = (int)(g_vrcfg.pointer_yield_ms * 0.09f);   /* at 90 fps */
    if (hold_frames < 1) hold_frames = 1;

    if (g_we_set_valid) {
        long dx = now.x - g_we_set.x, dy = now.y - g_we_set.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        /* Several pixels, not one.  A pixel or two is the rounding in our own
           absolute positioning coming back at us, plus whatever the remote
           session contributes, and reading that as "the user grabbed the
           mouse" is what caused the fight. */
        if (dx + dy > 12) {
            if (!announced) {
                announced = 1;
                kv_log("POINTER: the mouse is being used; it holds the cursor "
                       "until it stops for %.0f ms", g_vrcfg.pointer_yield_ms);
            }
            frames_left = hold_frames;
        }
    }

    if (frames_left > 0) {
        /* A deliberate aim, or a trigger, takes it back at once rather than
           waiting out the timer. */
        static float lx, ly, lz;
        float dx = g_vrin.aim_dir[0] - lx;
        float dy = g_vrin.aim_dir[1] - ly;
        float dz = g_vrin.aim_dir[2] - lz;
        float moved = dx * dx + dy * dy + dz * dz;
        lx = g_vrin.aim_dir[0]; ly = g_vrin.aim_dir[1]; lz = g_vrin.aim_dir[2];
        if (moved > 0.0025f || g_vrin.select || g_vrin.btn_a) frames_left = 0;
        else frames_left--;
    }
    return frames_left > 0;
}

/* Panel units to client pixels, in one place so that the self-check below
   tests the SAME arithmetic the pointer uses rather than a copy of it.
   MARGIN keeps the cursor off the outermost pixels: the window and the game
   canvas are different sizes, so the very edge is where any rounding in the
   engine own scaling lands outside its canvas. */
#define MARGIN 2

static int client_x(float u, int w) {
    int x = (int)((u * 0.5f + 0.5f) * (float)w);
    if (x < MARGIN) x = MARGIN;
    else if (x > w - 1 - MARGIN) x = w - 1 - MARGIN;
    return x;
}

/* Y flips: the panel counts up, a window counts down. */
static int client_y(float v, int h) {
    int y = (int)((0.5f - v * 0.5f) * (float)h);
    if (y < MARGIN) y = MARGIN;
    else if (y > h - 1 - MARGIN) y = h - 1 - MARGIN;
    return y;
}

/* Check the mapping with no headset and no hands: three known points whose
   answers are not a matter of opinion.  This is the whole reason the mapping
   is a function rather than two lines inline. */
void pointer_selfcheck(int w, int h) {
    int cxm = client_x(0.0f, w), cym = client_y(0.0f, h);
    int ok = 1;
    if (cxm < w / 2 - 1 || cxm > w / 2 + 1) ok = 0;
    if (cym < h / 2 - 1 || cym > h / 2 + 1) ok = 0;
    if (client_x(-1.0f, w) > MARGIN + 1) ok = 0;
    if (client_x(1.0f, w) < w - 2 - MARGIN) ok = 0;
    if (client_y(1.0f, h) > MARGIN + 1) ok = 0;      /* v=+1 is the TOP */
    if (client_y(-1.0f, h) < h - 2 - MARGIN) ok = 0;
    kv_log("POINTER: mapping self-check on a %dx%d window: centre->(%d,%d), "
           "left->%d right->%d top->%d bottom->%d -- %s",
           w, h, cxm, cym, client_x(-1.0f, w), client_x(1.0f, w),
           client_y(1.0f, h), client_y(-1.0f, h), ok ? "correct" : "WRONG");
}

/* Say each reason for falling back ONCE.  A pointer that silently does
   nothing costs a whole test run to diagnose; one that says why costs a line
   in the log. */
static void said_once(int which, const char *why) {
    static unsigned g_said;
    if (g_said & (1u << which)) return;
    g_said |= (1u << which);
    kv_log("POINTER: falling back to the right stick -- %s", why);
}

/* Both cursors into the corner, so they start from the same place.
   Relative movement, deliberately: an absolute jump tells Windows where to put
   OUR cursor and the game only ever sees the difference, so a jump that starts
   from an unknown place ends in an unknown place.  A large relative shove ends
   against the edge wherever it started. */
static int cursor_sync_running(void) {
    static int frames = -1;
    INPUT in;
    if (!g_vrcfg.cursor_sync) return 0;
    if (frames > 24) return 0;               /* done, for the session */
    if (frames < 0) {
        if (!kv_in_level()) return 0;        /* not yet: wait for the game */
        frames = 0;
        kv_log("CURSOR: homing both cursors into the top-left corner so the "
               "game's own cursor and ours start from the same place. The "
               "highlight square and the pointer disagree by whatever gap "
               "there is between them, and cornering is the only way to "
               "remove it -- the game never says where it thinks its cursor "
               "is.");
    }
    frames++;
    if (frames > 24) {
        kv_log("CURSOR: homed. From here the two move together, because the "
               "canvas and the window are the same size and therefore clamp "
               "at the same edges.");
        return 0;
    }
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;        /* relative */
    in.mi.dx = -3000;
    in.mi.dy = -3000;
    SendInput(1, &in, sizeof(in));
    return 1;                                /* hold off normal pointing */
}

/* What the renderer should do with the game's cursor sprite, and where our
   cursor sits in canvas pixels (y up). Set by the grip mouse below. */
static int   g_cur_draw;                 /* 0 as the game draws it, 1 hide, 2 shift */
static float g_cur_dx, g_cur_dy;
static float g_cur_cx = -1.0f, g_cur_cy = -1.0f;
/* diagnostics = 5, desk only (no controllers there, so the pointer never
   runs): feed the sprite machinery the REAL Windows cursor and cycle 3 s
   normal, 3 s hidden, 3 s shifted by (-400, -300) -- proxy.c dumps the
   screen once in each, to prove learning, hiding and moving at the desk. */
int g_cur_desk_mode = -1;               /* set frame by frame by proxy.c */
int kv_cursor_desk_phase(void) {
    return g_vrcfg.diagnostics == 5 ? (g_cur_desk_mode < 0 ? 0 : g_cur_desk_mode) : -1;
}
/* Where the arrow's tip should be, in canvas pixels (y up): proxy.c places
   the game's sprite there from where the game REALLY drew it this frame,
   so a frame's lag between our cursor move and the game's cannot show. */
static float g_cur_tx, g_cur_ty;
static int   g_cur_have_t;
int kv_cursor_target(float *tx, float *ty) {
    int ph = kv_cursor_desk_phase();
    if (ph == 2) {                       /* the desk check: 400 left, 300 down */
        float x, y;
        if (!kv_cursor_canvas(&x, &y)) return 0;
        *tx = x - 400.0f; *ty = y - 300.0f;
        return 1;
    }
    if (ph >= 0 || !g_cur_have_t) return 0;
    *tx = g_cur_tx; *ty = g_cur_ty;
    return 1;
}
int kv_cursor_draw(float *dx, float *dy) {
    int ph = kv_cursor_desk_phase();
    if (ph >= 0) { *dx = -400.0f; *dy = -300.0f; return ph; }
    *dx = g_cur_dx; *dy = g_cur_dy;
    return g_cur_draw;
}
int kv_cursor_canvas(float *x, float *y) {
    if (kv_cursor_desk_phase() >= 0) {
        HWND w = GetForegroundWindow();
        POINT p; RECT r; float cw, ch;
        if (!w || !GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &r) ||
            r.right <= 0 || r.bottom <= 0 || !kv_canvas_size(&cw, &ch)) return 0;
        *x = (float)p.x / (float)r.right * cw;
        *y = (1.0f - (float)p.y / (float)r.bottom) * ch;
        return 1;
    }
    if (g_cur_cx < 0.0f) return 0;
    *x = g_cur_cx; *y = g_cur_cy;
    return 1;
}
static void note_canvas(int cx, int cy, const RECT *rc) {
    float cw, ch;
    if (!kv_canvas_size(&cw, &ch) || rc->right <= 0 || rc->bottom <= 0) return;
    g_cur_cx = (float)cx / (float)rc->right * cw;
    g_cur_cy = (1.0f - (float)cy / (float)rc->bottom) * ch;
}

/* THE GRIP MOUSE (pointer_mode 3), 25 Sep 2026.
 *
 *  - No grip: the cursor is HIDDEN. In the world it is held at the centre of
 *    the canvas. The engine's camera is turned with the head (head_cull), so
 *    the centre of its view is where you are looking: A acts on what you look
 *    at, which is the Xbox game's "whatever is in front of you" in a headset.
 *    In a menu it is left where it is -- moving it would hover a row.
 *  - Right grip held: the game's arrow is shown where the controller's ray
 *    crosses the panel's plane, and the cursor goes to what is BEHIND that
 *    arrow as seen from the eye. In a menu, or over a HUD icon on the panel
 *    (kv_hud_hit, so the fist that punches and demolishes can be clicked),
 *    that is the panel's own mapping. In the world it is the eye's line
 *    through the arrow, taken through the ENGINE's camera -- which covers
 *    about 3.8 times the panel's angle, so a cursor placed by the panel would
 *    pick a tile far beyond the arrow. The game's sprite is redrawn at the
 *    arrow; past the panel's edge it is hidden and the game's highlight shows
 *    the pick.
 *
 * First headset run (25 Sep): clicks sometimes landed off target.
 * Three causes, all fixed here:
 *   - The world pick followed the hand's DIRECTION from the eye, while the
 *     arrow was on the hand's ray. A hand 30 cm from the eye with the panel
 *     2 m away puts those degrees apart. Now the pick is the eye's line
 *     through the arrow, so the two cannot disagree.
 *   - The arrow followed the raw aim and the cursor a smoothed one, so the
 *     click went to where the arrow had been a moment before. Now one
 *     smoothed point drives both.
 *   - Pulling a trigger turns the hand. The click is taken from where the
 *     arrow was CLICK_REWIND frames before the trigger registered, and held
 *     there until it is let go. */
#define CLICK_REWIND 7                        /* frames, ~80 ms at 90 fps */
static int grip_pointer(HWND wnd) {
    static float sx, sy;                      /* the smoothed crossing, panel units */
    static float ring_x[CLICK_REWIND], ring_y[CLICK_REWIND];
    static int have_smooth, ring_n, ring_i, held, was_hud;
    static int menu_st = -1, menu_cnt;
    int on_hud = 0, rewound = 0, had_t;
    float head_pos[3], head_q[4], hw, hh, panel_dist, k, u, v, pz;
    float o[3], d[3], e[3], rel[3], ayaw = 0.0f, apos[3], tilt;
    int anchored = 0;
    /* The front end (title, its submenus) is all menu. The draw count behind
       kv_menu_up cannot see it -- the main menu draws as many 2D elements as
       the HUD -- so there the world mapping pulled the cursor toward the
       middle, below the row pointed at (25 Sep). kv_front_end tells the
       two apart by the HUD in the top corners. */
    int menu = kv_menu_up() || kv_front_end(), mouse = kv_mouse_mode(), cx, cy, shift = 0;
    RECT rc;

    /* Debounced: the menu test is a draw count, and a tooltip can carry it
       across the line for a frame. Each flip moved the cursor between two
       mappings a long way apart -- the flicker seen in the headset (25 Sep). */
    if (menu_st < 0) menu_st = menu;
    if (menu != menu_st) { if (++menu_cnt >= 4) { menu_st = menu; menu_cnt = 0; } }
    else menu_cnt = 0;
    menu = menu_st;
    had_t = g_cur_have_t;
    g_cur_have_t = 0;

    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
    if (g_vrcfg.input_mode == 2) { g_cur_draw = 0; return 1; }   /* the real mouse has it */

    if (!mouse) {
        POINT cur;
        g_cur_draw = 1;                       /* hidden */
        have_smooth = 0; ring_n = 0; held = 0;
        if (menu) return 1;                   /* left where it is */
        cx = rc.right / 2; cy = rc.bottom / 2;
        if (!(GetCursorPos(&cur) && ScreenToClient(wnd, &cur) &&
              cur.x >= cx - 1 && cur.x <= cx + 1 && cur.y >= cy - 1 && cur.y <= cy + 1))
            cursor_to_client(wnd, cx, cy);
        note_canvas(cx, cy, &rc);
        return 1;
    }
    if (!g_vrin.valid || !g_vrin.aim_valid) { g_cur_draw = 0; have_smooth = 0; return 1; }
    if (g_vrin.select && held) {              /* hold still through the click */
        g_cur_have_t = had_t;
        return 1;
    }
    if (!g_vrin.select) held = 0;
    if (!vr_head_pose(head_pos, head_q) || !vr_panel_geometry(&hw, &hh, &panel_dist) ||
        hw <= 0.0f || hh <= 0.0f) { g_cur_draw = 0; return 1; }

    /* The ray and the eye in the panel's frame: the anchor's (standing
       still where it was placed), or the head's for a head-locked panel. */
    o[0] = g_vrin.aim_pos[0]; o[1] = g_vrin.aim_pos[1]; o[2] = g_vrin.aim_pos[2];
    d[0] = g_vrin.aim_dir[0]; d[1] = g_vrin.aim_dir[1]; d[2] = g_vrin.aim_dir[2];
    pz = -1.0f;                               /* the head-locked panel's plane */
    tilt = panel_dist > 0.0f ? vr_panel_tilt() : 0.0f;
    if (panel_dist > 0.0f) {
        e[0] = head_pos[0]; e[1] = head_pos[1]; e[2] = head_pos[2];
        if (vr_panel_anchor(&ayaw, apos)) {
            float c = (float)cos(-ayaw), sn = (float)sin(-ayaw), rx, rz;
            anchored = 1;
            o[0] -= apos[0]; o[1] -= apos[1]; o[2] -= apos[2];
            e[0] -= apos[0]; e[1] -= apos[1]; e[2] -= apos[2];
            rx = o[0] * c + o[2] * sn; rz = -o[0] * sn + o[2] * c; o[0] = rx; o[2] = rz;
            rx = d[0] * c + d[2] * sn; rz = -d[0] * sn + d[2] * c; d[0] = rx; d[2] = rz;
            rx = e[0] * c + e[2] * sn; rz = -e[0] * sn + e[2] * c; e[0] = rx; e[2] = rz;
        }
        /* ...then into the panel's OWN frame: its centre at the origin,
           leaning back by menu_tilt, so its plane is z = 0 (vr.c builds it
           the same way). The eye stays in the anchor's frame. */
        {
            float c = (float)cos(tilt), sn = (float)sin(tilt), ry, rz;
            o[2] += panel_dist;
            o[1] -= vr_panel_height();        /* raised by menu_height */
            ry = o[1] * c - o[2] * sn; rz = o[1] * sn + o[2] * c; o[1] = ry; o[2] = rz;
            ry = d[1] * c - d[2] * sn; rz = d[1] * sn + d[2] * c; d[1] = ry; d[2] = rz;
        }
        pz = 0.0f;
    } else {
        rel[0] = o[0] - head_pos[0]; rel[1] = o[1] - head_pos[1]; rel[2] = o[2] - head_pos[2];
        quat_inv_rotate(head_q, rel, o);
        quat_inv_rotate(head_q, g_vrin.aim_dir, d);
        e[0] = e[1] = e[2] = 0.0f;
    }
    /* Where the ray crosses the panel's PLANE, in panel units (-1..1 is on
       the panel). Pointing away from it: nothing to show. */
    {
        float t, pu, pv;
        if (d[2] > -0.05f) { g_cur_draw = 1; have_smooth = 0; return 1; }
        t = (pz - o[2]) / d[2];
        if (t <= 0.0f) { g_cur_draw = 1; have_smooth = 0; return 1; }
        pu = (o[0] + t * d[0]) / hw;
        pv = (o[1] + t * d[1]) / hh;
        k = g_vrcfg.pointer_smooth;
        if (k < 0.0f) k = 0.0f; else if (k > 0.95f) k = 0.95f;
        if (!have_smooth) { sx = pu; sy = pv; have_smooth = 1; ring_n = 0; }
        else { sx = sx * k + pu * (1.0f - k); sy = sy * k + pv * (1.0f - k); }
    }
    if (g_vrin.select) {
        /* the trigger has just registered: go back to before the pull */
        if (ring_n > 0) {
            int j = ring_n < CLICK_REWIND ? 0 : ring_i;
            sx = ring_x[j]; sy = ring_y[j];
            rewound = 1;
        }
        held = 1;
    } else {
        ring_x[ring_i] = sx; ring_y[ring_i] = sy;
        ring_i = (ring_i + 1) % CLICK_REWIND;
        if (ring_n < CLICK_REWIND) ring_n++;
    }

    if (!menu && sx >= -1.0f && sx <= 1.0f && sy >= -1.0f && sy <= 1.0f) {
        float cw, ch;
        if (kv_canvas_size(&cw, &ch))
            on_hud = kv_hud_hit((sx + 1.0f) * 0.5f * cw, (sy + 1.0f) * 0.5f * ch, 1,
                                /* sticky: across the gaps between the icons */
                                was_hud ? 64.0f : 6.0f);
    }
    was_hud = on_hud;
    if (menu || on_hud) {
        if (sx < -1.0f || sx > 1.0f || sy < -1.0f || sy > 1.0f) {   /* off the menu */
            g_cur_draw = 1; return 1;
        }
        u = sx; v = sy;
        g_cur_draw = 0;
    } else {
        /* the world: the eye's line through the arrow, through the engine's
           own camera, in head space */
        float ex = vr_engine_tanx(), ey = vr_engine_tany(), dir[3], hd[3];
        dir[0] = sx * hw; dir[1] = sy * hh; dir[2] = pz;
        if (panel_dist > 0.0f) {
            /* the arrow, from the panel's frame back to the anchor's */
            float c = (float)cos(tilt), sn = (float)sin(tilt), ry, rz;
            ry = dir[1] * c + dir[2] * sn; rz = -dir[1] * sn + dir[2] * c;
            dir[1] = ry + vr_panel_height(); dir[2] = rz - panel_dist;
        }
        dir[0] -= e[0]; dir[1] -= e[1]; dir[2] -= e[2];
        if (anchored) {
            float c = (float)cos(ayaw), sn = (float)sin(ayaw), rx, rz;
            rx = dir[0] * c + dir[2] * sn; rz = -dir[0] * sn + dir[2] * c;
            dir[0] = rx; dir[2] = rz;
        }
        if (panel_dist > 0.0f) quat_inv_rotate(head_q, dir, hd);
        else { hd[0] = dir[0]; hd[1] = dir[1]; hd[2] = dir[2]; }
        if (ex <= 0.0f || ey <= 0.0f || hd[2] > -0.05f) { g_cur_draw = 1; return 1; }
        u = (hd[0] / -hd[2]) / ex;
        v = (hd[1] / -hd[2]) / ey;
        if (u < -1.0f || u > 1.0f || v < -1.0f || v > 1.0f) { g_cur_draw = 1; return 1; }
        shift = (sx >= -1.0f && sx <= 1.0f && sy >= -1.0f && sy <= 1.0f);
        g_cur_draw = shift ? 2 : 1;           /* beyond the panel: highlight only */
    }

    cx = client_x(u, rc.right);
    cy = client_y(v, rc.bottom);
    if (g_cur_draw != 1 && sx >= -1.0f && sx <= 1.0f && sy >= -1.0f && sy <= 1.0f) {
        float cw, ch;
        if (kv_canvas_size(&cw, &ch)) {
            g_cur_tx = (sx + 1.0f) * 0.5f * cw;
            g_cur_ty = (sy + 1.0f) * 0.5f * ch;
            g_cur_have_t = 1;
        }
    }
    if (shift) {
        float cw, ch;
        if (kv_canvas_size(&cw, &ch)) {
            /* from where the game will draw it (the cursor) to the arrow, in
               canvas pixels */
            g_cur_dx = (sx - u) * 0.5f * cw;
            g_cur_dy = (sy - v) * 0.5f * ch;
        } else g_cur_draw = 1;
    }
    {
        POINT cur;
        if (!(GetCursorPos(&cur) && ScreenToClient(wnd, &cur) &&
              cur.x - cx <= 1 && cx - cur.x <= 1 && cur.y - cy <= 1 && cy - cur.y <= 1))
            cursor_to_client(wnd, cx, cy);
    }
    note_canvas(cx, cy, &rc);
    {
        static unsigned tick;
        if (rewound || (tick++ % 600) == 0)
            kv_log("POINTER: grip mouse -- %s, u=%+.3f v=%+.3f -> (%d,%d), sprite %s%s",
                   menu ? "menu (panel)" : on_hud ? "HUD icon (panel)" : "world (engine camera)",
                   u, v, cx, cy,
                   g_cur_draw == 2 ? "shifted to the arrow" :
                   g_cur_draw == 1 ? "hidden" : "as drawn",
                   rewound ? " -- CLICK, taken from before the trigger pull" : "");
    }
    return 1;
}

int pointer_frame(HWND wnd) {
    if (cursor_sync_running()) return 1;
    if (g_vrcfg.pointer_mode == 3) return grip_pointer(wnd);
    g_cur_draw = 0;
    float head_pos[3], head_q[4], hw, hh, panel_dist;
    float rel[3], o[3], d[3];
    float t, px, py, u, v;
    static float su, sv;
    static int have_smooth;
    RECT rc;
    int cx, cy;
    float k;

    /* PINNED TO THE MIDDLE.
       The Xbox version has no cursor: it highlights whatever is in front of
       the giant and A interacts with it.  Holding the cursor at the centre of
       the canvas reproduces that -- the camera follows the giant, so the
       middle of the screen IS in front of him.

       It also dissolves the three-way conflict that free pointing created.
       The game unprojects the cursor through its own frustum, so a cursor
       anywhere else only lands on what it appears to point at when the panel
       is drawn at exactly that frustum -- which then has to be wide enough not
       to clip the world and narrow enough to keep its corners in view, and
       cannot be both.  At the centre, every one of those agrees regardless of
       width, because the centre of any frustum is the same direction.

       So the panel is free to be a readable size again, and the engine camera
       is free to be as wide as the world needs. */
    if (g_vrcfg.cursor_lock) {
        /* The game does not read where the cursor IS.  It accumulates how far
           the cursor MOVED, into a position of its own -- which is why every
           attempt to place the cursor has failed in the same way: the two
           positions run in parallel with a constant offset that nothing ever
           corrects, so clicks land that offset away from where we aimed.  And
           pinning the cursor made it worse rather than better, because a
           pinned cursor produces no movement at all and the game's own cursor
           then froze wherever it already was.

           The offset cannot be measured from outside, but it can be DELETED.
           Drive the cursor hard into the top-left corner: ours stops at the
           edge of the screen, the game's stops at the edge of its canvas, and
           whatever gap there was is squeezed out against the corner.  Both are
           now at a known place.  Move a known distance from there and both
           arrive together.

           That is why this homes for several frames rather than jumping once:
           each frame carries one movement, and the corner has to be reached
           and held long enough for the game to have processed it. */
        RECT r;
        int px, py;
        static int phase;
        static int settled;

        if (!wnd || !GetClientRect(wnd, &r)) return 1;
        if (r.right <= r.left || r.bottom <= r.top) return 1;
        px = (int)((r.right - r.left) * g_vrcfg.cursor_lock_x);
        py = (int)((r.bottom - r.top) * g_vrcfg.cursor_lock_y);

        /* If the cursor has been dragged away from the pin -- the physical
           mouse, or the game moving it -- the offset may have changed too, so
           start the whole sequence again rather than just putting it back. */
        if (settled) {
            POINT cur;
            if (GetCursorPos(&cur) && ScreenToClient(wnd, &cur)) {
                long dx = cur.x - px, dy = cur.y - py;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx <= 2 && dy <= 2) return 1;
            }
            settled = 0;
            phase = 0;
            kv_log("POINTER: the cursor moved off the pin; re-homing");
        }

        if (phase < 12) {
            /* Into the corner, repeatedly, until any accumulated offset has
               been squeezed out against it. */
            cursor_to_client(wnd, 0, 0);
            phase++;
            return 1;
        }

        /* And now a known distance from a known corner. */
        cursor_to_client(wnd, px, py);
        phase = 0;
        settled = 1;
        kv_log("POINTER: homed against the corner and moved to (%d,%d) of a "
               "%ldx%ld window. The game's cursor and ours are now the same "
               "place, and the highlight is whatever is in front of the giant.",
               px, py, (long)(r.right - r.left), (long)(r.bottom - r.top));
        return 1;
    }

    /* MENU POINTER (pointer_mode 2), 24 Sep 2026. Aimed only while a menu is
       up; in the world the right stick nudges the cursor as before -- which
       also sidesteps the world-picking mismatch pointer_space describes.

       No homing is needed. Measured at the desk (diagnostics = 3): the game's
       own cursor sprite is the last 2D element of every frame, and it sits
       exactly where Windows puts the cursor, a constant half-sprite off (its
       hotspot is the top-left corner) -- no offset accumulates on this setup.
       So an aimed cursor lands where the ray does, and the game's own cursor
       is the dot the player sees.

       While the trigger is held the cursor stays put: menus act on the
       release over the row that was pressed, and a hand drifts while it
       pulls a trigger. */
    if (g_vrcfg.pointer_mode == 2) {
        static int was_menu;
        if (!kv_menu_up()) {
            if (was_menu) have_smooth = 0;
            was_menu = 0;
            return 0;                     /* the stick has it */
        }
        if (!was_menu) {
            was_menu = 1;
            kv_log("POINTER: a menu is up -- the controller points at it");
        }
        if (g_vrin.select) return 1;      /* hold still through the click */
    } else if (!g_vrcfg.pointer_mode) return 0;

    /* MOUSE AND KEYBOARD: the controller does not touch the cursor at all.
       Return 1 rather than 0, so the right stick does not step in as a
       relative mouse -- in this mode the cursor belongs to the mouse and
       nothing else should be pushing it about. */
    if (g_vrcfg.input_mode == 2) return 1;
    if (!g_vrin.valid || !g_vrin.aim_valid) {
        have_smooth = 0;
        said_once(0, "the right controller aim pose is not tracked");
        return 0;
    }
    if (!vr_head_pose(head_pos, head_q)) {
        have_smooth = 0; said_once(1, "no head pose yet"); return 0;
    }
    if (!vr_panel_geometry(&hw, &hh, &panel_dist)) {
        have_smooth = 0;
        said_once(2, "the 2D panel has not been drawn yet, so there is "
                     "nothing to point AT");
        return 0;
    }
    if (hw <= 0.0f || hh <= 0.0f) { said_once(3, "the panel has no size"); return 0; }
    if (!wnd || !GetClientRect(wnd, &rc)) {
        said_once(4, "the game window could not be measured"); return 0;
    }
    /* Return 1, not 0: the cursor is being driven, just not by us, and
       falling through to the right stick would be a third thing fighting for
       it. */
    /* CONTROLLER: the ray owns the cursor outright.  No arbitration, which is
       the point -- automatic hand-over meant the mouse had to be held still,
       and a stray pixel of movement from a remote session could take the
       cursor away mid-click. */
    if (g_vrcfg.input_mode == 0 && mouse_has_it()) { have_smooth = 0; return 1; }
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        said_once(5, "the game window has no client area"); return 0;
    }

    if (panel_dist > 0.0f) {
        /* The panel stands still, but NOT in front of the tracking origin --
           it stands at the anchor it was given when it opened.  This branch
           used to say the ray needed no transform at all, which was true of
           the old placement and is the fourth thing that assumption broke.
           Bring the ray into the anchor's frame: subtract its position, turn
           by minus its yaw, and the plane maths below is right again. */
        float ayaw, apos[3];
        float rx, rz, c, sn;
        o[0] = g_vrin.aim_pos[0];
        o[1] = g_vrin.aim_pos[1];
        o[2] = g_vrin.aim_pos[2];
        d[0] = g_vrin.aim_dir[0];
        d[1] = g_vrin.aim_dir[1];
        d[2] = g_vrin.aim_dir[2];
        if (vr_panel_anchor(&ayaw, apos)) {
            o[0] -= apos[0]; o[1] -= apos[1]; o[2] -= apos[2];
            c = (float)cos(-ayaw); sn = (float)sin(-ayaw);
            rx = o[0] * c + o[2] * sn;
            rz = -o[0] * sn + o[2] * c;
            o[0] = rx; o[2] = rz;
            rx = d[0] * c + d[2] * sn;
            rz = -d[0] * sn + d[2] * c;
            d[0] = rx; d[2] = rz;
        }
    } else {
        /* Head-anchored panel (menus): bring the ray into head space. */
        rel[0] = g_vrin.aim_pos[0] - head_pos[0];
        rel[1] = g_vrin.aim_pos[1] - head_pos[1];
        rel[2] = g_vrin.aim_pos[2] - head_pos[2];
        quat_inv_rotate(head_q, rel, o);
        quat_inv_rotate(head_q, g_vrin.aim_dir, d);
    }

    /* Cross the plane the panel is drawn on.  Pointing away from it -- behind
       you, or parallel to it -- has no crossing, and forcing one would fling
       the cursor to a corner. */
    if (d[2] > -0.05f) {
        said_once(6, "you are pointing away from the screen");
        return 0;
    }
    /* The plane the panel lies on: its real distance when it is fixed in the
       world, or unit distance when it is a tangent-space panel on the head. */
    t = ((panel_dist > 0.0f ? -panel_dist : -1.0f) - o[2]) / d[2];
    if (t <= 0.0f) return 0;

    px = o[0] + t * d[0];
    py = o[1] + t * d[1];

    if (g_vrcfg.pointer_space) {
        /* Measure against the ENGINE's frustum, which is what the game
           unprojects the cursor through when it picks a ground tile.  The
           panel's own size is then irrelevant to where the highlight lands --
           which is the point: the highlight is what the player is aiming, not
           the cursor sprite. */
        float ex = vr_engine_tanx(), ey = vr_engine_tany();
        if (ex > 0.0f && ey > 0.0f) {
            /* px,py are tangents of the aim direction only when the panel is
               at unit distance; scale back out of the panel first. */
            float tx = px / (panel_dist > 0.0f ? panel_dist : 1.0f);
            float ty = py / (panel_dist > 0.0f ? panel_dist : 1.0f);
            u = tx / ex;
            v = ty / ey;
        } else {
            u = px / hw;
            v = py / hh;
        }
    } else {
        u = px / hw;          /* -1 at the left edge, +1 at the right */
        v = py / hh;          /* -1 at the bottom, +1 at the top */
    }

    /* Pointing off the panel LEAVES THE CURSOR WHERE IT IS.  Clamping a miss
       onto the edge drags the cursor into a corner every time the hand drops,
       and the first thing the first version ever did was slam it to the
       bottom-right pixel -- which is also the most likely input to have
       crashed the game, since the window is 3840x1431 while the canvas it maps
       onto is 3840x2160, and the very last pixel is where an off-by-one in
       that scaling would land out of bounds. */
    if (u < -1.0f || u > 1.0f || v < -1.0f || v > 1.0f) {
        said_once(7, "you are pointing off the edge of the screen; the cursor "
                     "stays where it was");
        return 1;
    }

    /* A held hand is not still, and at the far end of a ray a small wobble is
       a large cursor movement.  Smooth it, but seed from the first sample so
       the cursor does not slide in from wherever it was. */
    k = g_vrcfg.pointer_smooth;
    if (k < 0.0f) k = 0.0f; else if (k > 0.95f) k = 0.95f;
    if (!have_smooth) { su = u; sv = v; have_smooth = 1; }
    else { su = su * k + u * (1.0f - k); sv = sv * k + v * (1.0f - k); }

    cx = client_x(su, rc.right - rc.left);
    cy = client_y(sv, rc.bottom - rc.top);

    /* Do not re-send a position the cursor is ALREADY at -- every send is a
       mouse message the game has to process.

       Compared against where the cursor actually is, not against the last
       position we asked for.  Those are not the same thing: with a steady
       hand the requested position stops changing, and a check against our own
       last request then stops sending entirely, which hands the cursor to
       whatever else moves it.  Clicks land wherever that left it, and nothing
       ever pulls it back.  Comparing against reality makes this
       self-correcting: if anything moves the cursor away from where the ray
       points, the very next frame puts it back. */
    {
        POINT cur;
        if (GetCursorPos(&cur) && ScreenToClient(wnd, &cur)) {
            long dx = cur.x - cx, dy = cur.y - cy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= 1 && dy <= 1) return 1;
        }
    }

    /* Where the cursor was BEFORE this move, captured before sending it.
       The status line used to read the position straight after SendInput,
       which is asynchronous -- so it reported the pre-move position as though
       it were the result, and made a working move look like a 650 pixel
       error. */
    {
        POINT before;
        g_report_have = GetCursorPos(&before) && ScreenToClient(wnd, &before);
        g_report_x = g_report_have ? before.x : -1;
        g_report_y = g_report_have ? before.y : -1;
    }

    cursor_to_client(wnd, cx, cy);

    /* Reported REPEATEDLY, not once.  Every one-shot report in this port has
       fired during startup and captured values that do not apply in play --
       a panel size taken before the engine had drawn a scene, an action read
       as unbound before the runtime had bound it.  Each one sent me after the
       wrong thing.  A status line that keeps printing cannot do that. */
    {
        static unsigned tick;
        if ((tick++ % 600) == 0) {

            kv_log("POINTER: mode %d (%s) | panel half %.3f x %.3f m at %.2f m "
                   "| engine tan %.3f x %.3f | ray u=%+.3f v=%+.3f -> moving "
                   "cursor to (%d,%d) from (%ld,%ld)",
                   g_vrcfg.input_mode,
                   g_vrcfg.input_mode == 2 ? "MOUSE, controller will not move "
                                             "the cursor"
                                           : "CONTROLLER",
                   hw, hh, panel_dist, vr_engine_tanx(), vr_engine_tany(),
                   su, sv, cx, cy,
                   (long)g_report_x, (long)g_report_y);
        }
    }
    return 1;
}
