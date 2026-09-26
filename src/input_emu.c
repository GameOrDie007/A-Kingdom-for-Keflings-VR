/* A Kingdom for Keflings VR -- Quest controllers as keyboard and mouse.
 *
 * Why this rather than a gamepad.  The game's own content still carries the
 * whole Xbox control scheme -- "Move Giant" on the left stick, "Rotate Camera"
 * on the right, "Break/Kick" on X -- but the PC build has no code left to read
 * a pad with.  Measured, not assumed: it creates a DirectInput object and never
 * a device, loads no XInput DLL, never calls the legacy joystick API, and
 * registers nothing for Raw Input.  NinjaBee kept the controller UI and dropped
 * the controller input.
 *
 * So the controllers are translated into what the game DOES read.  At this
 * level there is no difference: the game cannot tell a synthesised key from a
 * real one.
 *
 * Two things this must not do:
 *  - type into whatever else is on the desktop.  SendInput goes to the
 *    FOREGROUND window, so everything here is gated on the game actually being
 *    it.  The user is often connected remotely with other windows open.
 *  - hold a key down forever.  Every press is edge-tracked and released, and
 *    everything is released when the game loses focus.
 */
#include "proxy.h"
#include "vr.h"

#define NKEYS 21

typedef struct {
    int vk;             /* virtual key, or 0 for a mouse button */
    DWORD mouse_down;   /* MOUSEEVENTF_* if this is a mouse button */
    DWORD mouse_up;
    int held;
} Binding;

static Binding g_bind[NKEYS];
static int g_bound;
static HWND g_game_wnd;
static int g_had_focus;
static int g_in_menu;
static int g_photo;                 /* photo mode: the HUD and menus hidden */
static int g_mouse;                 /* right grip held: the grip mouse is on */
int kv_mouse_mode(void) { return g_mouse; }
/* diagnostics = 4 forces it on, for a VR session with no hand on the button.
   NOT provable at the desk: with VR off the 2D pass is never captured into
   the panel (vr_panel_begin has no target), so it goes straight to the
   window and there is nothing for photo mode to leave out -- tried 24 Sep. */
int kv_photo_mode(void) { return g_photo || g_vrcfg.diagnostics == 4; }

/* index into g_bind */
enum { K_FWD, K_BACK, K_LEFT, K_RIGHT, K_INTERACT, K_SECOND, K_MENU,
       K_ACTION, K_KICK, K_HAT, K_BLUEPRINT, K_BUMPERL,
       K_NAV_UP, K_NAV_DOWN, K_NAV_LEFT, K_NAV_RIGHT, K_NAV_OK,
       K_FWD2, K_BACK2, K_LEFT2, K_RIGHT2 };

static void set_key(int i, int vk) {
    g_bind[i].vk = vk;
    g_bind[i].mouse_down = g_bind[i].mouse_up = 0;
}

static void set_mouse(int i, DWORD down, DWORD up) {
    g_bind[i].vk = 0;
    g_bind[i].mouse_down = down;
    g_bind[i].mouse_up = up;
}

static void bind_defaults(void) {
    if (g_bound) return;
    g_bound = 1;
    set_key(K_FWD,   'W');
    set_key(K_BACK,  'S');
    set_key(K_LEFT,  'A');
    set_key(K_RIGHT, 'D');
    set_mouse(K_INTERACT, MOUSEEVENTF_LEFTDOWN,  MOUSEEVENTF_LEFTUP);
    set_mouse(K_SECOND,   MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    /* The menu button is What's Next (F1); B alone is Esc. Both used to be
       Esc, so both opened the pause menu (the game has one key for cancel
       and pause), and there was no button left for Remove Hat. Chosen
       24 Sep 2026: the Xbox layout -- B cancels, Y removes the hat. */
    set_key(K_MENU,   VK_F1);        /* What's Next / tutorial help */
    set_key(K_ACTION, VK_ESCAPE);    /* B: cancel, or pause when nothing to cancel */
    /* From the game's own key list, in Readme.rtf -- whose document title
       is "Swat-PC Game Keys" -- PLUS one it leaves out. The list has no
       Remove Hat, and Y was moved off 'C' on the strength of that; the
       retail game, tested on 24 Sep 2026, shows C removes the hat of the
       kefling you are holding. A list of keys is a claim, not a proof.

       What the list truly lacks: the Xbox build has ten contextual actions
       including PUNCH, and the PC build exposes only Kick. Punch is the
       fist icon in the HUD, clicked with the pointer. */
    set_key(K_KICK, VK_LSHIFT);      /* Kick */
    set_key(K_HAT,  'C');            /* Remove Hat, as Y is on Xbox */
    /* The two shoulder buttons of the Xbox scheme: left bumper is B,
       right bumper is T.  The grips are the nearest thing a Quest
       controller has to a shoulder. */
    set_key(K_BLUEPRINT, 'T');
    set_key(K_BUMPERL,   'B');
    /* Menu navigation, used only while a menu is up. */
    set_key(K_NAV_UP,    VK_UP);
    set_key(K_NAV_DOWN,  VK_DOWN);
    set_key(K_NAV_LEFT,  VK_LEFT);
    set_key(K_NAV_RIGHT, VK_RIGHT);
    set_key(K_NAV_OK,    VK_RETURN);
    /* The arrow keys, as a SECOND set of movement keys.  Everything
       measurable says the chain works -- the stick reaches full
       deflection, the key injects, and Windows reports it held -- and
       the giant still does not move, which leaves only one thing: WASD
       is not what moves him.  Arrows are the other convention, and
       sending both costs nothing if WASD turns out to be right. */
    set_key(K_FWD2,   VK_UP);
    set_key(K_BACK2,  VK_DOWN);
    set_key(K_LEFT2,  VK_LEFT);
    set_key(K_RIGHT2, VK_RIGHT);
    kv_log("INPUT: left stick WASD, right stick pointer, right trigger "
           "clicks, A = Enter (select/pickup/drop), B = Esc (cancel/pause), "
           "X = Left Shift (kick), Y = C (remove hat), left grip = B "
           "(blueprint), right grip = T (tech tree), menu = F1 (What's Next), "
           "both grips = recenter the menus (held 2 s: controller/mouse). "
           "All from the game's own list in Readme.rtf, plus C, which the "
           "list leaves out.");
}

/* Two ways to give this game a key, and the measurements say which.
 *
 * SendInput puts the key into the SYSTEM input stream and Windows routes it to
 * whatever has keyboard focus.  That is the polite way, and it is what was
 * used until now -- but the key spy, which watches every message arriving at
 * the game window, recorded NOT ONE key arriving while the stick was being
 * pushed.  So they were going somewhere else: the window that holds keyboard
 * focus is not the window the game renders into and we subclassed.
 *
 * PostMessage skips the routing entirely and puts the message on the chosen
 * window's own queue.  That is normally a poor substitute, because a game that
 * POLLS the keyboard never looks at its message queue and would not notice.
 * This one cannot have that problem: it was measured not to poll at all --
 * zero calls to GetAsyncKeyState and GetKeyState across a whole session -- so
 * messages are the only thing it reads, and posting them is exact.
 *
 * lParam has to be built properly or the message is not a key press.  It
 * carries the repeat count, the scan code, the extended flag, and -- the two
 * that are easy to forget -- the previous key state and the transition state,
 * which are what tell the game a key went DOWN rather than came up.
 */
static HWND g_key_target;

static LPARAM key_lparam(int vk, int down) {
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    LPARAM l = 1;                       /* repeat count */
    int extended = 0;
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
    case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
        extended = 1;
        break;
    default: break;
    }
    l |= ((LPARAM)(scan & 0xFF)) << 16;
    if (extended) l |= (LPARAM)1 << 24;
    if (!down) l |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    return l;
}

/* Injection is the default again.  Posting a message was addressing a fault
   that did not exist -- the window with keyboard focus turned out to BE the
   window the game renders into -- and it is the weaker of the two: a posted
   message never updates the key-state table, so anything reading state rather
   than messages sees nothing at all.  SendInput does both, which is what makes
   it indistinguishable from a real key.  key_post = 1 in the ini switches back
   if this is ever wrong. */
/* The game's own name for each key, so the log says what the press meant and
   not just a number.  "I pressed every button and nothing happened" cannot
   separate a key that never arrived from one that arrived and did nothing. */
static const char *key_name(int vk) {
    switch (vk) {
    case VK_RETURN:  return "Enter (select/pickup/drop)";
    case VK_ESCAPE:  return "Esc (cancel/pause)";
    case VK_LSHIFT:  return "Left Shift (KICK)";
    case VK_F1:      return "F1 (What's Next / tutorial help)";
    case 'C':        return "C (remove hat)";
    case 'B':        return "B (show blueprint)";
    case 'T':        return "T (tech tree)";
    case 'P':        return "P (player list)";
    case 'W': case 'A': case 'S': case 'D': return "WASD (move)";
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        return "arrow (move/menu)";
    default:         return "unlisted key";
    }
}

static void send_key(int vk, int down) {
    /* Only the presses, not the releases, and not the movement keys:
       a held stick would otherwise fill the log. */
    if (down && vk != 'W' && vk != 'A' && vk != 'S' && vk != 'D' &&
        vk != VK_UP && vk != VK_DOWN && vk != VK_LEFT && vk != VK_RIGHT)
        kv_log("INPUT: sent %s", key_name(vk));
    HWND h = g_key_target ? g_key_target : g_game_wnd;
    UINT sent;
    INPUT in;

    if (g_vrcfg.key_post && h) {
        PostMessageA(h, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk,
                     key_lparam(vk, down));
        return;
    }

    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (key_lparam(vk, down) & ((LPARAM)1 << 24))
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    sent = SendInput(1, &in, sizeof(in));

    /* The measurement that splits this in one run.  Right after injecting a
       key, ask Windows whether it considers that key held.  If Windows says
       yes and the giant still does not move, the injection is sound and the
       key is simply not what moves him -- which is a completely different
       problem from the one I have been chasing, and no amount of work on
       delivery would ever have fixed it. */
    if (down) {
        static unsigned reported;
        int slot = 0;
        switch (vk) {
        case 'W': slot = 1; break;  case 'A': slot = 2; break;
        case 'S': slot = 3; break;  case 'D': slot = 4; break;
        default: slot = 5; break;
        }
        if (!(reported & (1u << slot))) {
            reported |= (1u << slot);
            kv_log("INPUT: injected key 0x%02X scan 0x%02X -- SendInput "
                   "accepted %u of 1, and Windows now reports it %s",
                   vk, (unsigned)in.ki.wScan, sent,
                   (GetAsyncKeyState(vk) & 0x8000) ? "HELD (injection works; "
                   "if nothing happens, this key is not the one that acts)"
                   : "NOT held (the injection itself is being rejected)");
        }
    }
}

/* Which window actually has keyboard focus.  Not an assumption: ask Windows,
   because the answer is the whole reason the keys were going nowhere. */
static void find_key_target(void) {
    GUITHREADINFO gti;
    HWND chosen;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(0, &gti) && gti.hwndFocus) chosen = gti.hwndFocus;
    else if (gti.hwndActive) chosen = gti.hwndActive;
    else chosen = g_game_wnd;

    if (chosen == g_key_target) return;
    g_key_target = chosen;
    kv_log("INPUT: keys go to %p (the window with keyboard focus); the game "
           "renders into %p%s", (void *)g_key_target, (void *)g_game_wnd,
           g_key_target == g_game_wnd ? " -- the same window"
                                      : " -- A DIFFERENT WINDOW, which is why "
                                        "injected keys were going nowhere");
}

static void send_mouse_button(DWORD flag) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(in));
}

/* One wheel notch.  Positive is away from you, which the game reads as
   zooming in. */
static void send_wheel(int notches) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(notches * WHEEL_DELTA);
    SendInput(1, &in, sizeof(in));
}

static void send_mouse_move(int dx, int dy) {
    INPUT in;
    if (!dx && !dy) return;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

/* Press or release only on a CHANGE, so a held stick is one key-down rather
   than ninety a second. */
static void hold(int i, int want) {
    Binding *b = &g_bind[i];
    if (!!b->held == !!want) return;
    b->held = want;
    if (b->vk) send_key(b->vk, want);
    else if (want) send_mouse_button(b->mouse_down);
    else send_mouse_button(b->mouse_up);
}

static void release_all(void) {
    int i;
    for (i = 0; i < NKEYS; i++) hold(i, 0);
}

/* A menu step: one press when the stick goes over, then a slow repeat while
   it is held.  Held outright, an arrow key at 90 fps crosses the whole menu
   before you let go. */
static void nav_step(int i, int want, int *state) {
    /* One step per push, and the stick must return to centre before the next.
       An auto-repeat fires several times into a menu that may be part way
       through a transition, which is a good way to crash one. */
    if (!want) { *state = 0; return; }
    if (*state) return;
    *state = 1;
    hold(i, 1);
    hold(i, 0);
}

void input_emu_frame(HDC hdc) {
    float dz, mx, my;
    int focused;

    if (!g_vrcfg.emulate_input || !g_vrin.valid) return;
    bind_defaults();

    if (!g_game_wnd) g_game_wnd = WindowFromDC(hdc);

    /* Everything is gated on the game being the foreground window.  Without
       this, a stick nudge types into whatever else is on the desktop. */
    focused = (g_game_wnd && GetForegroundWindow() == g_game_wnd);
    if (!focused) {
        if (g_had_focus) {
            release_all();
            kv_log("INPUT: game lost focus; all synthetic keys released");
        }
        g_had_focus = 0;
        return;
    }
    if (!g_had_focus) kv_log("INPUT: game has focus; controller input is live");
    g_had_focus = 1;
    find_key_target();

    dz = g_vrcfg.stick_deadzone;

    /* The game mapping runs ALWAYS, menu or not.
       It used to be suppressed while a menu was up, which sounded harmless and
       was not: "is a menu up" is guessed from the number of draws in the 2D
       pass, and the in-game HUD draws 44-46 against a threshold of 50.  Any
       extra element -- a tooltip, an icon, a kefling carrying something --
       crosses it, and movement silently died while every other control kept
       working.  That is exactly the symptom that cost this evening.

       Nothing needs the suppression.  Menu navigation by stick is off, so the
       only thing it bought was not walking behind an open menu, which is not
       worth a control that stops working for reasons nobody can see. */
    if (kv_menu_up() != g_in_menu) {
        g_in_menu = kv_menu_up();
        kv_log("INPUT: %s (the mapping is unchanged either way)",
               g_in_menu ? "a menu is up" : "the menu closed");
    }

    /* Left stick -> movement keys. */
    hold(K_FWD,   g_vrin.move_y >  dz);
    hold(K_BACK,  g_vrin.move_y < -dz);
    hold(K_LEFT,  g_vrin.move_x < -dz);
    hold(K_RIGHT, g_vrin.move_x >  dz);
    if (g_vrcfg.move_arrows) {
        hold(K_FWD2,   g_vrin.move_y >  dz);
        hold(K_BACK2,  g_vrin.move_y < -dz);
        hold(K_LEFT2,  g_vrin.move_x < -dz);
        hold(K_RIGHT2, g_vrin.move_x >  dz);
    }

    /* Say, once, that the stick was actually pushed far enough to do
       anything.  "The stick did nothing" and "the stick was never past the
       dead zone" look identical from here and need completely different
       fixes. */
    {
        static int said;
        if (!said && (g_vrin.move_x > dz || g_vrin.move_x < -dz ||
                      g_vrin.move_y > dz || g_vrin.move_y < -dz)) {
            said = 1;
            kv_log("INPUT: left stick pushed past the dead zone (%.2f %.2f, "
                   "dead zone %.2f); movement keys are being sent",
                   g_vrin.move_x, g_vrin.move_y, dz);
        }
    }

    /* PHOTO MODE, 24 Sep 2026 (game-or-die-features 3). Hold the menu button
       and the HUD, the menus and their dim disappear, so the player can take
       a clean screenshot with the headset's own capture; hold it again and
       they are back. A TAP is still What's Next -- sent on the release now,
       since a press cannot know yet whether it will become a hold. While the
       HUD is hidden every other button is swallowed (a press would act on a
       menu nobody can see) except that B also brings everything back, so
       nobody is left stranded; the left stick still walks. */
#define PHOTO_HOLD 54                 /* frames, about 0.6 s at 90 fps */
    {
        static int menu_frames, f1_up, b_latch;
        if (f1_up) { hold(K_MENU, 0); f1_up = 0; }
        if (g_vrin.menu) {
            if (++menu_frames == PHOTO_HOLD) {
                g_photo = !g_photo;
                kv_log("INPUT: photo mode %s (menu button held)",
                       g_photo ? "ON -- HUD and menus hidden" : "OFF");
                if (g_photo) release_all();
            }
        } else {
            if (menu_frames > 0 && menu_frames < PHOTO_HOLD && !g_photo) {
                hold(K_MENU, 1);          /* a tap: What's Next */
                f1_up = 1;
            }
            menu_frames = 0;
        }
        if (g_photo) {
            if (g_vrin.btn_b) {
                g_photo = 0;
                b_latch = 1;              /* this press must not also be Esc */
                kv_log("INPUT: photo mode OFF (B)");
            }
            if (g_photo) return;          /* swallow everything else */
        }
        if (b_latch) {
            if (g_vrin.btn_b) return;     /* until B is let go */
            b_latch = 0;
        }
    }

    /* The cursor goes where the right controller points.  The right stick is
       only the pointer when that is unavailable -- controller set down, out of
       tracking, or switched off -- so the stick stays useful rather than
       fighting the ray for the same cursor. */
    /* pointer_frame returns 1 when it owns the cursor -- including when the
       cursor is pinned -- and only then does the right stick step in as a
       relative mouse.  Otherwise the stick would be dragging the cursor off
       the point the pin is holding it at, ninety times a second. */
    if (!pointer_frame(g_game_wnd)) {
        mx = g_vrin.look_x;
        my = g_vrin.look_y;
        if (mx > -dz && mx < dz) mx = 0.0f;
        if (my > -dz && my < dz) my = 0.0f;
        send_mouse_move((int)(mx * g_vrcfg.mouse_speed),
                        (int)(-my * g_vrcfg.mouse_speed));
    }

    /* A is Enter, everywhere.  The trigger is the mouse click.
       A used to be a second mouse button, which meant it clicked wherever the
       cursor happened to be and ignored whatever the menu had highlighted.
       Making it Enter only inside menus did not work either, because "is a
       menu up" is guessed from the number of draws in the 2D pass and that
       guess is not reliable enough to hang a button on.  Unconditional needs
       no guess and cannot be wrong.

       Held rather than tapped: one key-down while the button is down, one
       key-up when it is released.  That is what a real keyboard sends, and it
       cannot auto-repeat into a menu mid-transition the way a tap loop can. */
    hold(K_NAV_OK,   g_vrin.btn_a);
    /* The grip mouse: in a menu without the grip the cursor is hidden and
       left wherever it was, so a click there would press something nobody
       can see. Decided on the press and kept until the release. */
    {
        static int clicking = -1;             /* -1 up, 0 swallowed, 1 a click */
        if (!g_vrin.select) clicking = -1;
        else if (clicking < 0)
            clicking = g_vrcfg.pointer_mode != 3 || kv_mouse_mode() ||
                       (!kv_menu_up() && !kv_front_end());
        hold(K_INTERACT, clicking == 1);
    }
    hold(K_SECOND,   g_vrin.trig_l > 0.5f);
    /* K_MENU (What's Next) is sent by the photo-mode block above, on release */
    hold(K_ACTION,   g_vrin.btn_b);
    hold(K_KICK,     g_vrin.btn_x);
    /* The sweep found nothing because there is nothing to find: testing the
       retail game key by key showed that PUNCH has no keyboard binding at all.
       It is a pointer-only action -- hover the cursor over the fist icon in the
       HUD and click it.  Kept, switched off, because the next undocumented
       action will want it. */
    if (g_vrcfg.key_sweep) {
        /* Y walks a candidate list instead of its normal binding.  Driven by
           a button rather than a timer so the tester sets the pace and can
           report "the seventh press" -- a count, which the log can resolve
           exactly, rather than a moment, which it cannot. */
        /* The nineteen already eliminated are at the END, so a second pass
           does not spend nineteen presses reaching new ground.  vk 0 means a
           mouse button: those were dismissed with right-click when LEFT click
           turned out to walk the character, which says nothing about the
           other three. */
        static const struct { int vk; int mouse_dn, mouse_up; const char *name; } cand[] = {
            { 0, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, "MIDDLE mouse" },
            { 0, MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP,      "mouse side button" },
            { 0, MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP,  "RIGHT mouse" },
            { VK_RSHIFT, 0, 0, "Right Shift" },
            { VK_RCONTROL, 0, 0, "Right Ctrl" },
            { VK_BACK, 0, 0, "Backspace" }, { VK_DELETE, 0, 0, "Delete" },
            { VK_INSERT, 0, 0, "Insert" },  { VK_HOME, 0, 0, "Home" },
            { VK_END, 0, 0, "End" },        { VK_PRIOR, 0, 0, "PageUp" },
            { VK_NEXT, 0, 0, "PageDown" },  { VK_CAPITAL, 0, 0, "CapsLock" },
            { VK_F2, 0, 0, "F2" }, { VK_F3, 0, 0, "F3" },
            { VK_F4, 0, 0, "F4" }, { VK_F5, 0, 0, "F5" },
            { VK_OEM_3, 0, 0, "Backtick" }, { '4', 0, 0, "4" },
            { '5', 0, 0, "5" }, { '6', 0, 0, "6" }, { 'N', 0, 0, "N" },
            { 'J', 0, 0, "J" }, { 'L', 0, 0, "L" }, { 'U', 0, 0, "U" },
            { 'I', 0, 0, "I" }, { 'O', 0, 0, "O" }, { 'Y', 0, 0, "Y" },
            /* the ones already ruled out, kept so the list is a record */
            { VK_SPACE, 0, 0, "Space (tried)" }, { VK_CONTROL, 0, 0, "Ctrl (tried)" },
            { VK_MENU, 0, 0, "Alt (tried)" }, { VK_TAB, 0, 0, "Tab (tried)" },
            { 'E', 0, 0, "E (tried)" }, { 'F', 0, 0, "F (tried)" },
            { 'Q', 0, 0, "Q (tried)" }, { 'R', 0, 0, "R (tried)" },
        };
        static int idx, was;
        int n = (int)(sizeof(cand) / sizeof(cand[0]));
        if (g_vrin.btn_y && !was) {
            kv_log("SWEEP press %d of %d: sending %s",
                   idx + 1, n, cand[idx].name);
            if (cand[idx].vk) {
                send_key(cand[idx].vk, 1);
                send_key(cand[idx].vk, 0);
            } else {
                send_mouse_button(cand[idx].mouse_dn);
                send_mouse_button(cand[idx].mouse_up);
            }
            idx = (idx + 1) % n;
            if (idx == 0)
                kv_log("SWEEP: that was the last candidate; the next press "
                       "starts again at 1. If nothing hit the building, the "
                       "key is not in this list.");
        }
        was = g_vrin.btn_y;
    } else {
        hold(K_HAT,      g_vrin.btn_y);
    }
    /* Both grips together switches between controller and mouse.  It was on
       the two thumbstick clicks, which is Virtual Desktop's own menu chord --
       a reminder that this port does not have the controller to itself and
       cannot claim a combination without checking what already owns it.

       The grips are the two bumper keys individually, so the chord has to
       suppress those on the frame it fires or switching would also press B
       and T. */
    /* Both grips, 24 Sep 2026: a squeeze RECENTERS the menus in front of you,
       as in the SWE1R port; held for two seconds it still switches between
       controller and mouse. The switch was on a 0.75 s hold, which a recenter
       squeeze would trip -- and there is no way to SEE which mode you are in
       from inside the headset, so an accidental switch is expensive.

       Each grip alone is still a key (blueprints, tech tree), so a grip that
       goes down alone waits CHORD_WAIT frames for its partner before it
       sends anything; otherwise the first grip of every squeeze would open a
       menu. After a chord, neither grip sends its key until both are up. */
#define CHORD_WAIT 6               /* frames, about 1/15 s at 90 fps */
    /* THE GRIP MOUSE, 25 Sep 2026: play is controller-first,
       as on the Xbox -- no cursor, A acts on what you look at. HOLD the right
       grip and the mouse is there: the cursor follows where the hand points
       and the trigger clicks. A TAP of the right grip is still the tech tree
       (sent on release, since a press cannot yet know it is a tap). Both
       grips pressed TOGETHER -- within CHORD_WAIT frames of each other --
       recenter; a left grip pressed while the right is already holding the
       mouse is just the blueprints. */
#define MOUSE_HOLD 20              /* frames, about 0.2 s: longer is a hold */
    {
        static int both_held, chord, wait_l, wait_r, t_up;
        int gl = g_vrin.grip_l > 0.5f, gr = g_vrin.grip_r > 0.5f;
        if (t_up) { hold(K_BLUEPRINT, 0); t_up = 0; }
        if (gl && gr && !chord && !g_mouse &&
            (wait_l <= CHORD_WAIT || wait_r <= CHORD_WAIT) &&
            (wait_l - wait_r <= CHORD_WAIT && wait_r - wait_l <= CHORD_WAIT)) {
            chord = 1;
            vr_recenter("both grips", 1);
        }
        if (gl && gr && chord) {
            if (++both_held == 180) {
                g_vrcfg.input_mode = (g_vrcfg.input_mode == 2) ? 1 : 2;
                kv_log("INPUT: switched to %s",
                       g_vrcfg.input_mode == 2
                           ? "MOUSE AND KEYBOARD -- the controller will not "
                             "move the cursor"
                           : "CONTROLLER -- the ray owns the cursor and the "
                             "mouse is ignored");
            }
        } else {
            both_held = 0;
        }
        /* the right grip: a tap is the tech tree, a hold is the mouse */
        if (gr) {
            wait_r++;
            if (!chord && wait_r > MOUSE_HOLD && !g_mouse) {
                g_mouse = 1;
                kv_log("INPUT: mouse ON (right grip held) -- the cursor follows "
                       "your hand and the trigger clicks");
            }
        } else {
            if (wait_r > 0 && wait_r <= MOUSE_HOLD && !chord && !g_mouse) {
                hold(K_BLUEPRINT, 1);       /* a tap: the tech tree */
                t_up = 1;
            }
            if (g_mouse) kv_log("INPUT: mouse OFF (right grip let go)");
            g_mouse = 0;
            wait_r = 0;
        }
        /* the left grip: blueprints while held, after waiting for a chord */
        wait_l = gl ? wait_l + 1 : 0;
        hold(K_BUMPERL, gl && !chord && wait_l > CHORD_WAIT);
        /* LAST: a chord's release must not read as a tap of either grip */
        if (!gl && !gr) chord = 0;
    }

    /* Zoom, on the stick clicks.  Throttled: a notch per frame at 90 fps
       would cross the whole zoom range before you let go. */
    {
        static int tick;
        int in_ = g_vrin.stick_click_r, out = g_vrin.stick_click_l;

        if (in_ || out) {
            if (--tick <= 0) {
                send_wheel(in_ ? 1 : -1);
                tick = 6;
            }
        } else {
            tick = 0;
        }
    }
}

void input_emu_shutdown(void) {
    if (g_bound) release_all();
}
