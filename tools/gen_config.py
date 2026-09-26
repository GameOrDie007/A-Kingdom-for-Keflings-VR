"""Regenerate the config file's template and its defaults from one table.

There were three places a default could be written -- the string in read_all,
the value printed into the shipped file, and the compiled-in initialiser -- and
nothing kept them together.  Sixteen live settings had no comment at all and
landed bare at the bottom of the file, and several of the comments that did
exist described the port as it was BEFORE the culling fix: the file told a
player that the shipped defaults lose the world when you turn your head.

A config key is a claim.  This makes one table the source of every claim, emits
the template from it, and rewrites read_all so the two cannot drift.

Run it whenever a setting is added or a default changes.
"""
import io
import re

BS = chr(92)

# key, type, default, comment.  Order here is the order in the file.
SECTIONS = [
 ("", [
  ("enabled", "i", "1", []),
 ]),

 ("how the world is presented", [
  ("stereo", "i", "1", [
    "1 = stereo diorama: the kingdom stands in front of you in 3D.",
    "0 = the flat game on a big virtual screen."]),
  ("world_scale", "f", "500", [
    "Game units per real metre. THE size knob: larger makes the",
    "kingdom read as a smaller model nearer your face.",
    "Try 250 / 500 / 1000 and pick."]),
  ("converge", "f", "800", [
    "Distance, in game units, at which the two eyes agree.",
    "Lower brings the kingdom further out towards you."]),
  ("fov_scale", "f", "1.0", [
    "How much of your headset's field of view to fill.",
    "1 = all of it. Lower fits the view inside the narrower",
    "frustum the engine renders, which costs immersion and is",
    "no longer needed: head_cull makes the engine draw what you",
    "are looking at, whichever way you turn."]),
  ("fill_amount", "f", "0", [
    "Extra uniform magnification, 0..1, on top of fov_scale.",
    "0 = geometrically exact. 1 = as magnified as it can go",
    "without distorting, which looks like binoculars. The stereo",
    "separation grows with it so depth still agrees with size."]),
  ("srgb_swapchain", "i", "1", [
    "1 = declare the compositor texture sRGB. Correct for this",
    "game; 0 is a test setting and looks washed out."]),
 ]),

 ("resolution", [
  ("match_canvas_to_monitor", "i", "1", [
    "1 = correct the GAME's own XScreenRes/YScreenRes, in its",
    "fsall/settings.ini, to this monitor's resolution before the",
    "game reads them.",
    "The game lays its entire 2D out for those two numbers whether",
    "or not the window it gets is that size. When they disagree,",
    "menus are measured against a rectangle that is not on screen:",
    "panels come out the wrong shape and some of the UI is drawn",
    "outside the window entirely. A flat player never sees that,",
    "because it is cropped; in VR the whole canvas is visible.",
    "Your original file is backed up once, beside it, as",
    "settings.ini.before-vr. Set this to 0 to manage it yourself."]),
  ("borderless", "i", "1", [
    "1 = make the game's window borderless and fill the monitor.",
    "This is NOT exclusive full screen: the desktop display mode",
    "is left alone, so your other windows are not rearranged every",
    "time the game starts. It also stops the taskbar covering the",
    "window and stops the title bar eating rows off the canvas.",
    "The window is not kept on top, so alt-tab behaves normally."]),
  ("offscreen", "i", "1", [
    "1 = render into our own target, sized from what your headset",
    "runtime asks for. 0 = render into halves of the game's own",
    "window, which lets your MONITOR decide the VR resolution --",
    "on a 1440p desktop that was 1287x1431 an eye against the",
    "3072x3264 the runtime wanted. Leave this on."]),
  ("render_scale", "f", "1.0", [
    "Fraction of the runtime's recommended eye size. 1.0 is what",
    "it asks for.",
    "LEAVE THIS AT 1.0 unless you are testing. Measured 20 Sep",
    "2026 on VirtualDesktopXR: 0.8 took the eye copy and submit",
    "from 0.45 ms to 143.9 ms and the game to 5.6 fps. Our own",
    "copy stays 1:1 either way, so this is the runtime taking a",
    "slow path for a swapchain that is not the size it asked for.",
    "It may behave on other runtimes; it has only been measured",
    "on this one, and the port says so in its log when the value",
    "is not 1.0."]),
 ]),

 ("how far the engine draws, and where it looks", [
  ("head_cull", "i", "1", [
    "1 = turn the engine's own view by your head, so the ground",
    "and props it decides to draw are the ones you are looking",
    "at. The eye transform cancels the rotation exactly, so the",
    "picture does not move. Without this the world ends at a hard",
    "edge below and to the right, where the flat game's camera",
    "never pointed."]),
  ("draw_distance", "f", "3", [
    "How much further than default the engine should draw. It",
    "stops at 2200 game units, which in a headset reads as a",
    "bubble of world travelling with you. 1 = untouched,",
    "3 = three times as far, which is 6600 units.",
    "This is the far plane only. It is NOT what makes ground tiles",
    "vanish at the bottom edge of your view when you tilt down --",
    "that is engine_fov, and it should be left at 0 so the port",
    "derives it from your headset. Raise this if distant parts of",
    "the map pop in, lower it if you need frames; on the test PC the",
    "whole range from 1 to 4 moved the frame by less than a",
    "millisecond, so it is not where the time goes."]),
  ("engine_fov", "f", "0", [
    "The engine's camera is hardcoded to 45 degrees vertically and",
    "culls to it. This writes a wider value over that camera in",
    "memory so it DRAWS what you can see.",
    "0 = derive it from your headset: exactly wide enough and not",
    "a degree more. A number forces that value instead."]),
  ("engine_fov_max", "f", "150", [
    "Ceiling on the above. Wider covers more head turn and costs",
    "frames. With head_cull on, the derived value sits well under",
    "this and the ceiling never comes into play."]),
  ("fov_both_ends", "i", "1", [
    "1 = write the widened camera again at the end of the frame.",
    "The engine resets the value every frame, and one of the two",
    "moments is not always enough."]),
 ]),

 ("frame pacing and the desktop window", [
  ("force_vsync", "i", "1", [
    "1 = hold vsync off and let the headset compositor pace the",
    "frames. 0 leaves the game vsynced to your desktop refresh,",
    "which caps VR at that rate."]),
  ("mirror_fps", "f", "30", [
    "How many times a second to update the desktop window.",
    "Presenting it every VR frame keeps the swap queue full and",
    "pins the whole game to the monitor's refresh. Keep this well",
    "under your desktop rate; 0 turns the window off entirely."]),
  ("mirror_mode", "i", "0", [
    "What that window shows. The two eyes are drawn side by side",
    "into one buffer, so showing it untouched is a squashed pair.",
    "0 = the left eye, cropped to the window's shape and filling",
    "it -- what other headset mirrors do.",
    "1 = the whole left eye, with black bars.",
    "2 = untouched."]),
 ]),

 ("menus and the in-game screen", [
  ("hud_size", "f", "75", [
    "Angular width of the 2D panel, in degrees. The game's own",
    "shape is kept inside it, so raising this makes menus bigger,",
    "not wider.",
    "It is ALSO how far away the panel's CORNERS are, and some of",
    "this game's UI is anchored to them -- the What's Next list",
    "sits in the canvas's bottom-left. At 100 that corner is 54",
    "degrees off centre and out at the rim of a Quest 3's view; at",
    "85 it is 46; at 75, 41; at 60, 34. Bigger text and reachable",
    "corners are one dial pulling two ways, so set it for whichever",
    "UI you read most."]),
  ("menu_size", "f", "75", [
    "The same, for frames the port judges to be a menu. Keep it",
    "EQUAL to hud_size unless you want the panel to change size",
    "when a menu opens: the port switches between the two on a",
    "draw-count threshold that flips as you click around, so any",
    "difference here shows up as the whole panel jumping."]),
  ("menu_draws", "i", "150", [
    "2D draws per frame at or above which a frame counts as a",
    "menu. On this game the in-game HUD draws about 101 and the",
    "main menu about 99, so NO threshold separates them -- which",
    "is why this is set above both, and hud_size is what you",
    "actually want to adjust. The log prints the live figure."]),
  ("hud_distance", "f", "1.5", [
    "How far away the panel sits, in metres. Flat images identical",
    "in both eyes read as infinitely far without this, which",
    "fights a world an arm's length in front of you."]),
  ("menu_height", "f", "0.5", [
    "Metres the menu screen is raised above your eyes (negative:",
    "lowered). Moves it straight up, so menu_tilt stays as it is.",
    "0.5 was chosen in the headset (25 Sep). -2 to 2."]),
  ("menu_tilt", "f", "-40", [
    "Degrees the menu screen (menus, blueprints, tech tree, HUD) leans",
    "back at the top; NEGATIVE leans the top toward you. The game looks",
    "down on the kingdom at about 44 degrees, so the world in front of",
    "you is tipped up like a ramp and an upright screen seems to lean",
    "back against it. -40, chosen in the headset (25 Sep), stands it up",
    "in the world; 0 = upright. (30 laid it almost flat.)",
    "-60 to 60."]),
  ("menu_dim", "f", "1.0", [
    "Carries the game's menu dimming out to the edge of your view.",
    "The game darkens the world behind a menu by drawing into its",
    "own 2D canvas, which here is a panel of a set angular size, so",
    "without this the darkening stops at the panel edge and the",
    "world stays bright around it.",
    "1.0 is exact. The surround is painted in the colour AND alpha",
    "of the game's own backdrop quad, which on this game is a dark",
    "navy at alpha 0.49, not black. It once shipped at 0.92 to hide",
    "a rectangle edge: that edge was the missing blue, and 0.92",
    "traded it for a red and green mismatch.",
    "tools/dim_shape.ps1 re-runs the measurement, per channel.",
    "This is a MULTIPLIER on whatever the game is doing, not a",
    "fixed amount: 1 matches it exactly and joins without a seam,",
    "0.5 is half as dark, 0 turns it off. Screens the game does not",
    "dim are left alone whatever this says."]),
  ("menu_dim_wide_search", "i", "0", [
    "Look for a menu's dimming backdrop in two extra places.",
    "Normally the port only inspects immediate-mode draws and",
    "display lists, and only recognises a backdrop by its dark",
    "colour. With this on it also inspects vertex-array draws, and",
    "accepts a translucent shape that covers essentially the whole",
    "canvas whatever colour it carries.",
    "Aimed at the blueprint DETAILS page, whose backdrop the normal",
    "search cannot see. UNVERIFIED: it has not been shown to fix",
    "that page, and inspecting more draws is how a screen that",
    "should stay bright ends up dark. Off unless someone is",
    "actively testing it."]),
  ("menu_dim_alpha_only", "i", "0", [
    "A second way of spotting the game's menu dimming, for the",
    "screens that do it with a texture instead of a colour.",
    "The normal detector looks for a near-black translucent quad.",
    "Some screens -- the blueprint DETAILS page is one -- draw a",
    "dark texture modulated by a WHITE vertex colour instead, so",
    "the colour the port can read is white and the darkness is in",
    "the texture, where it cannot be read at all.",
    "With this on, a menu whose darkest translucent draw has a",
    "usable alpha is dimmed by that alpha even when its colour is",
    "not dark. Screens that draw nothing translucent, or draw it",
    "at alpha 0, are still left alone -- the blueprint list itself",
    "reads 0.00 and stays bright.",
    "OFF by default: darkening a screen the game meant to leave",
    "bright is worse than leaving one undimmed, and this rule has",
    "been tried against only a handful of screens so far."]),
  ("hud_world_lock", "i", "1", [
    "1 = the 2D panel stands still in the world, where you were",
    "looking when it appeared, so you can turn your head to read",
    "the corners of it -- this game puts the What's Next list in",
    "the bottom-left corner, and a panel welded to your face moves",
    "that corner away exactly as fast as you turn towards it.",
    "It re-anchors if you turn far enough that it leaves your view,",
    "so it cannot be lost behind you.",
    "0 = the panel follows your head."]),
  ("panel_flat_test", "i", "0", [
    "DIAGNOSTIC. Capture the 2D pass and blit it back over the",
    "window at 1:1, with no headset and no VR session. A correct",
    "capture is pixel-identical to panel_once = 0, so this is the",
    "A/B that proves the blend rather than asserting it. Leave 0."]),

  ("skip_noop", "i", "1", [
    "Skip scenery draws that cannot change a single pixel.",
    "The game draws an invisible highlight overlay over most of the",
    "world every frame -- fully transparent until something is",
    "highlighted -- and in VR pays for it twice. When a draw is",
    "provably invisible it is skipped; the moment the game makes it",
    "visible, it is drawn again.",
    "It checks itself with the graphics card before it skips anything,",
    "and turns itself off if a check ever fails.",
    "0 off, 1 on (the default since 0.10.0), 2 alternates on and off to",
    "measure the difference. 3 follows merge_group's cycle."]),
  ("desk_stereo", "i", "0", [
    "A desk measurement aid. With no headset connected, 1 makes the",
    "port draw everything twice -- as it does for your two eyes -- into",
    "the left and right halves of the window. It looks wrong on purpose;",
    "it exists so the cost of the second eye can be measured without a",
    "headset. Leave it at 0."]),
  ("merge_group", "i", "9", [
    "Draw scenery that looks alike in one go instead of one piece at a",
    "time -- the biggest part of 0.10.0's speed-up. 9 is the setting to",
    "play with (the default). 0 draws everything the game's own way,",
    "slower: use it, with skip_noop = 0, if anything ever looks wrong,",
    "and say what and where. The other numbers are test modes:",
    "0 leaves everything as it is.",
    "1 is an audit: it picks the biggest group of scenery that shares",
    "one look, checks that every piece of it really does, and draws it",
    "two ways off screen to compare them pixel for pixel. Nothing you",
    "see changes.",
    "2 merges every group that is safe to draw in any order, from",
    "buffers on the graphics card.",
    "3 does the same for 10 seconds, then draws normally for 10,",
    "repeating, and logs the frame rate of each half separately --",
    "so one session shows the speed difference and whether anything",
    "looks different.",
    "4 is a desk check: it draws normally but compares every merged",
    "group pixel for pixel against the game's own drawing.",
    "5 draws every piece from its own buffer on the graphics card, in",
    "its original place and order, alternating 10 s on and 10 s off.",
    "6 combines both: merges what is safe to merge, draws everything",
    "else from its own buffer in its original place, alternating.",
    "7 cycles three states, 5 s each in VR: the game as it is, merging",
    "only what is provably order-safe, and merging everything including",
    "cut-out and soft-edged scenery -- with the frame rate of each.",
    "9 is the third of those, all the time: every group merged,",
    "including cut-out and soft-edged scenery. Confirmed in the",
    "headset 24 Sep 2026 -- nothing looked different, 74 -> 84 fps",
    "with skip_noop = 1."]),
  ("merge_offset", "f", "16", [
    "Push merged scenery back by this many depth steps. A merged draw",
    "computes positions on the CPU, so its depth can differ from the",
    "engine's in the last bits, and a layer the game draws exactly on",
    "top -- the snow on trees, rocks and crystals -- lost to it and",
    "flickered. 16 steps is a hundredth of a game unit at 1000 units:",
    "measured on a snowy save, merged and unmerged frames then differ",
    "less than two unmerged frames do. 0 = off, which falls back to",
    "keeping layered objects out of the merge (slower in the snow)."]),
  ("mv_shadow", "i", "0", [
    "Track the world's placement in software rather than asking the",
    "driver for it, which would stall the pipeline thousands of times a",
    "frame. It checks itself against the driver once and says how far",
    "apart they were.",
    "It also measures how much of the scene is standing still:",
    "buildings do not move, keflings do, and that ratio decides how",
    "much work a merging renderer would have to redo each frame.",
    "Diagnostic only. Changes nothing you can see."]),
  ("mesh_capture", "i", "0", [
    "Keep our own copy of the geometry in each display list, read as",
    "the engine compiles it. This draws nothing and changes nothing --",
    "every call still goes through to the driver exactly as before.",
    "It exists to answer two questions the merging design needs: how",
    "many vertices a frame actually draws, and how often the engine",
    "rebuilds its lists.",
    "Diagnostic only. Leave it at 0 unless a measurement is being",
    "taken."]),
  ("group_census", "i", "0", [
    "Count how many DISTINCT state groups a frame's world pass contains.",
    "Consecutive draws never share state in this engine -- but that is",
    "the order it happens to emit them in, and a batching renderer sorts",
    "first. What matters is how many groups there are in the whole",
    "frame, and this counts them, along with how many distinct display",
    "lists are called and how often each repeats.",
    "Diagnostic only. It changes nothing you can see and costs a little",
    "time per draw, so leave it at 0 unless a measurement is being",
    "taken."]),
  ("engine_aspect", "f", "0", [
    "Narrow how WIDE the engine culls, without changing what you see.",
    "It builds its camera from one vertical angle times its own 16:9",
    "shape, so making it tall enough for a headset also makes it 141",
    "degrees wide -- and an eye needs about 116. Everything between is",
    "drawn and never seen.",
    "1.0 is about right for a Quest 3; 1.7778 is the engine's own.",
    "0 leaves it alone. Your view cannot shrink: each eye is rendered",
    "from the headset's frustum, not this one.",
    "Measured worth little -- tripling the engine's frustum only",
    "changes its draw count by a third -- so treat any gain as a",
    "bonus."]),
  ("engine_tanx_ab", "i", "0", [
    "DIAGNOSTIC. 1 = switch engine_tanx on and off every 300 frames",
    "and report what each arm submitted. Two separate launches cannot",
    "be compared here -- the scene differs by hundreds of draws."]),
  ("engine_tanx", "f", "0", [
    "Narrow how wide the engine CULLS, without changing what you see.",
    "The engine builds its camera from a single vertical angle and its",
    "own 16:9 shape, so making it tall enough for a headset also makes",
    "it 141 degrees WIDE -- and a Quest 3 eye only needs about 116.",
    "Everything in between is drawn and never seen.",
    "This is the horizontal tangent to cull to: 1.6 is about 116",
    "degrees, 2.88 is what the engine picks on its own. 0 leaves it",
    "alone. It cannot shrink your view: what you see is rendered from",
    "the headset's own frustum, not the engine's."]),
  ("state_filter", "i", "0", [
    "1 = skip the graphics calls that set something to the value it",
    "already has. This engine sets everything from scratch for every",
    "object it draws: about 14 calls between every two draws, some",
    "38,000 a frame, and most of them change nothing.",
    "Safe here because the engine's display lists were measured and",
    "contain only geometry, so calling one cannot change the state",
    "being tracked. The cache is dropped at the start of every frame,",
    "on glPushAttrib and glPopAttrib, on a context switch and on a",
    "texture-unit change.",
    "The frame report says how many calls it skipped. F5 toggles it",
    "while the game runs."]),
  ("diagnostics", "i", "0", [
    "DIAGNOSTIC. 1 = run the development scaffolding: the module",
    "list, the camera and heap dumps, the GL call census, and a full",
    "frame trace every time a menu opens.",
    "Every one of those walks memory or writes files on the render",
    "thread. Measured on a second test PC: the batch at frame 300",
    "cost a 784 ms stall, and the trace armed by opening a menu is",
    "about a hundred flushed log lines each time. Leave this off",
    "unless you are asked for it."]),
  ("stall_ms", "f", "25", [
    "Frames that take longer than this many milliseconds are",
    "recorded, and the worst few are printed in the next frame",
    "report with where their time went. A stall that is mostly",
    "'waiting on the compositor' is the headset link; one that is",
    "mostly unaccounted is the game or the graphics driver."]),
  ("cull_follow_head", "i", "1", [
    "1 = let the engine decide what to draw from where your EYE is,",
    "not from where its own camera sits.",
    "head_cull already turns the engine's view to follow your head.",
    "This does the same for your head's POSITION, which world_scale",
    "multiplies by 500 -- so leaning 20 cm puts your eye 100 game",
    "units away from the camera that chose what to draw, and the",
    "nearest scenery at the edge of your view gets dropped. That is",
    "the squares vanishing at the bottom when you tilt down, and it",
    "comes and goes as you lean, which is how it was identified.",
    "The picture is unchanged: the offset is added to the engine's",
    "matrix and subtracted from the eye's, and the port checks that",
    "on the first frame and prints the result.",
    "If the world moves twice as far as your head, or swims when you",
    "lean, press F6 or set this to 0 and tell me -- that would mean",
    "one of those two halves is wrong."]),
  ("skip_glfinish", "i", "1", [
    "1 = ignore the engine's glFinish, which it calls once every",
    "frame. That call stops the processor dead until the graphics",
    "card has finished everything sent so far, so the two never",
    "work at the same time and your frame costs processor time",
    "PLUS card time instead of whichever is larger. Measured on a",
    "Test PC: engine 10.2 ms, card 6.9 ms, frame 13.6 ms -- much",
    "nearer the sum than the larger. In 2010 on a 60 Hz monitor",
    "that call was harmless; in VR it wastes half the machine, and",
    "the headset compositor already paces the frames.",
    "If anything looks stale or flickers, set this to 0. F7",
    "toggles it while the game runs and says so in the log."]),
  ("block_arrays", "i", "0", [
    "DIAGNOSTIC, measured neutral. 1 = collect each glBegin/glEnd",
    "block on the CPU and draw it as one array draw per eye instead",
    "of forwarding the engine's ~150,000 vertex calls a frame and",
    "compiling a display list per block. A/B/A on a paused frame,",
    "20 Sep 2026, test PC: 13.0 / 12.8 / 13.0 ms -- no difference,",
    "with 92-byte and with 36-byte vertices alike. The driver's",
    "immediate-mode path is already cheap; the engine's time is its",
    "own work per vertex. The picture is identical either way."]),
  ("dup_alternate", "i", "1", [
    "1 = draw the world for both eyes with ONE state switch per",
    "draw instead of three: draw N goes left eye then right, draw",
    "N+1 right then left, and the engine's own projection and",
    "viewport are put back only when it is about to look at them.",
    "Each eye still gets every draw in the engine's order, so the",
    "picture is the same. 0 = switch to each eye and back for every",
    "draw, the way it was before 20 Sep 2026; measured at 1.5 ms",
    "of an 11-13 ms frame on the test PC."]),
  ("stereo_pass_list", "i", "0", [
    "DIAGNOSTIC, and measured SLOWER. 1 = record each scene pass",
    "into one display list while the first eye draws and replay it",
    "for the second. On the test PC the driver spends about",
    "10 ms a frame compiling that list: 45 fps against 90. Kept so",
    "the measurement can be repeated, not to be used."]),
  ("dup_profile", "i", "0", [
    "DIAGNOSTIC. 1 = time each part of every duplicated draw. About",
    "eight clock reads a draw, half a millisecond a frame, so the",
    "frame report's duplication split is only filled in with this",
    "on -- and the numbers include the instrument's own cost."]),
  ("block_census", "i", "0", [
    "DIAGNOSTIC. 1 = log which GL calls the engine makes between",
    "glBegin and glEnd, how many vertices a block carries and which",
    "primitive modes it uses. For deciding whether the blocks can be",
    "drawn as arrays."]),
  ("mirror_eye", "i", "0", [
    "DIAGNOSTIC. 1 = the desktop window mirrors the RIGHT eye. The",
    "left eye is the one the engine draws itself; the right is the",
    "duplicated one, so this is how a duplication change is",
    "photographed."]),
  ("panel_once", "i", "1", [
    "1 = draw the 2D pass ONCE into a canvas-sized target and",
    "blit it to each eye, rather than re-issuing every 2D draw",
    "per eye at eye resolution. The panel is a flat image and is",
    "identical in both eyes; only the projection differs.",
    "Measured in VR on the tech tree, 20 Sep: 61.9 -> 82.4 fps,",
    "duplication 5.56 -> 1.41 ms, GPU 8.39 -> 4.82 ms, and 2D",
    "duplications 425400 -> 0. In ordinary play 87.7 -> 88.9, so",
    "no regression there. The picture was checked by toggling it",
    "live on a paused frame: the difference is within head",
    "jitter. Set to 0 to go back to drawing the 2D per eye."]),

  ("menu_dim_hold_force", "i", "0", [
    "DIAGNOSTIC. 1 = paint the held dim's canvas fill on EVERY",
    "full-screen menu, not only when the game has stopped drawing",
    "its own backdrop. The held state is otherwise reachable only",
    "through blueprints -> details -> back, which makes the one",
    "thing that was broken hard to look at. With menu_dim_probe",
    "the canvas fill is green and the surround blue, so a single",
    "capture says which drew. Leave at 0."]),

  ("scissor_mode", "i", "2", [
    "1 = honour the engine's clip rectangle, mapped through the",
    "panel transform. Scrolling lists -- the tips and blueprints",
    "menus -- stay inside their box.",
    "0 = ignore it. Nothing can be hidden, but those lists then",
    "draw over the whole panel instead of scrolling within it.",
    "2 = clip PLANES rather than a rectangle, and the default. A"
    "scissor is axis aligned in SCREEN pixels, and since the panel",
    "stands in the world the region to clip is a rotated, skewed",
    "quad -- so at 1 a scrolling list leaks more of itself as you",
    "roll your head. Planes are given in the engine's own",
    "coordinates and are right at any head angle.",
    "(It once blacked the whole screen, because the planes were",
    "left enabled during the WORLD pass, where canvas coordinates",
    "clip everything. They are now confined to the 2D pass.)",
    "A clip rectangle can only HIDE things, so if UI goes missing",
    "in a menu, try 1, then 0."]),
 ]),

 ("controls", [
  ("emulate_input", "i", "1", [
    "1 = drive the game with the Quest controllers. The PC build",
    "has no gamepad code left, so they are translated into the",
    "keyboard and mouse it does read. Left stick moves, right",
    "stick is the pointer, right trigger clicks, A is Enter,",
    "B is Escape, menu is Escape."]),
  ("controllers", "i", "1", [
    "1 = this port reads the controllers itself. 0 = leave them to",
    "Virtual Desktop's gamepad emulation."]),
  ("input_mode", "i", "1", [
    "0 = whichever device moved last owns the cursor.",
    "1 = controller: the ray owns it and the mouse is ignored.",
    "2 = mouse and keyboard: the controller never touches it.",
    "Hold both grips for a second to switch between 1 and 2",
    "without leaving the game."]),
  ("pointer_space", "i", "0", [
    "What an AIMED cursor (pointer_mode 1) is measured against.",
    "0 = the 2D panel. Pointing at icons and menus is exact, but",
    "the ground highlight runs AHEAD of where you point, by however",
    "much wider the engine camera is than the panel -- at 75 and",
    "141 degrees that is about 1.9x.",
    "1 = the engine's camera, which is what the game unprojects the",
    "cursor through to pick a tile. The highlight then lands where",
    "you point, and icons are off by the same ratio the other way.",
    "These agree only when hud_size equals the engine's frustum,",
    "which puts the panel's corners out of reach. Pick for whether",
    "you click tiles or icons more."]),
  ("pointer_mode", "i", "3", [
    "3 = THE GRIP MOUSE (default): controller play as on the Xbox --",
    "no cursor, A acts on what you look at. Hold the right grip and",
    "the cursor follows where your hand points, and the trigger",
    "clicks. In menus it always follows your hand.",
    "0 = the right stick nudges the cursor.",
    "1 = the cursor goes where the right controller points.",
    "2 = in menus the cursor goes where the controller points; in",
    "the world the right stick nudges it."]),
  ("leftorium", "i", "0", [
    "THE LEFTORIUM -- left-handed play. 1 = the cursor stick, the",
    "click trigger, A/B and pointing move to the left controller;",
    "walking, X/Y and the other trigger to the right. The menu",
    "button stays on the left: it is the only one there is."]),
  ("stick_deadzone", "f", "0.25", []),
  ("mouse_speed", "f", "14", [
    "Cursor speed for pointer_mode 0."]),
  ("pointer_yield_ms", "f", "400", [
    "How long the physical mouse keeps the cursor after it stops",
    "moving. 0 = the controller always has it."]),
  ("pointer_smooth", "f", "0.55", [
    "0..1, for pointer_mode 1: how much of the previous position",
    "to keep. A raw aim pose is jittery at a cursor's distance."]),
  ("move_arrows", "i", "1", [
    "1 = the left stick sends the arrow keys as well as WASD."]),
  ("key_post", "i", "0", [
    "1 = post key messages straight to the game window instead of",
    "injecting them into the system input stream."]),
  ("cursor_sync", "i", "0", [
    "1 = once, shortly after a level loads, drive the cursor into",
    "the top-left corner. The game does not read where the cursor",
    "is -- it accumulates how far the cursor MOVED into a position",
    "of its own -- so the two drift apart and the highlight square",
    "ends up on a different tile from your pointer. The gap cannot",
    "be measured, but cornering both squeezes it out: ours stops at",
    "the screen edge, the game's at its canvas edge, and they are",
    "then in the same place.",
    "You will see the cursor flick to the corner once. That is it.",
    "OFF by default: it was built to fix the cursor and the ground",
    "highlight disagreeing, and it did not -- that turned out to be",
    "a SCALE mismatch, not an offset, so cornering cannot help.",
    "Kept because the reasoning is sound for a genuine offset."]),
  ("cursor_lock", "i", "0", [
    "1 = hold the cursor at one point and ignore the mouse, the",
    "way the Xbox version has no cursor and highlights whatever is",
    "in front of you. Experimental."]),
  ("cursor_lock_x", "f", "0.5", [
    "Where to hold it, as a fraction of the window."]),
  ("cursor_lock_y", "f", "0.5", []),
 ]),

 ("the flat-screen mode (stereo = 0)", [
  ("screen_mode", "i", "0", [
    "1 = a world-locked stereo window you look around.",
    "0 = project into the headset's own field of view."]),
  ("screen_distance", "f", "2.2", []),
  ("screen_width", "f", "3.2", []),
  ("quad_distance", "f", "2.5", []),
  ("quad_width", "f", "3.0", []),
 ]),

 ("diagnostics", [
  ("menu_dim_hold", "i", "1", [
    "Keep dimming a full-screen menu the game has stopped dimming.",
    "Opening the building flowchart dims the world; stepping into a",
    "building's details keeps it dimmed; stepping back out (Back or",
    "Escape) leaves it bright, because the game stops drawing its",
    "own backdrop. Measured in the unmodified flat game on 20 Sep",
    "2026: the world behind that flowchart is at full brightness,",
    "so this is the game's own bug, and it is far more obvious in a",
    "headset. This holds the dim that was already found, in the",
    "game's own colour, while a full-screen menu is still up. It",
    "can only EXTEND a dim the game itself drew, never invent one,",
    "so the worst it can do is leave a menu dimmed a little longer",
    "than the game meant to. Verified in the eye: the held frame",
    "and the real one match to the pixel.",
    "Set to 0 to follow the game exactly, bug and all."]),
  ("menu_dim_measure", "i", "0", [
    "Work out a menu's dimming by measuring it rather than by",
    "recognising the draw that caused it.",
    "This game draws every 2D element with a white vertex colour",
    "and much of it through display lists, so there is no dark",
    "quad to find and no extent to measure. Instead the port reads",
    "the view at three points as the 2D pass begins and again when",
    "it ends: whatever the game drew in between, the difference is",
    "the dimming, and that is the figure the surround matches.",
    "Costs one small readback every eighth frame while a menu is",
    "open, and nothing at all otherwise."]),
  ("dim_trace", "i", "0", [
    "DIAGNOSTIC. Lists every translucent draw in a menu's 2D pass",
    "-- colour, alpha, how much of the canvas it covers, and which",
    "draw path it came by. For finding a menu backdrop the normal",
    "search cannot see, without guessing at what it might look",
    "like. Verbose; leave off unless hunting."]),
  ("menu_dim_probe", "i", "0", [
    "DIAGNOSTIC. 1 = paint the surround dim bright blue and log",
    "the rectangle it covers, to see whether it drew and where.",
    "Not a setting to play with."]),
  ("key_sweep", "i", "0", [
    "DIAGNOSTIC. 1 = the Y button sends the next key from a list",
    "of candidates instead of its normal binding, and logs which.",
    "For finding an action whose key is not documented.",
    "Note for the next person: PUNCH has no key. Testing the retail",
    "game key by key found that the only way to punch and demolish",
    "a building is to hover the cursor over the fist icon in the",
    "HUD and click it. Do not go looking for a binding."]),
  ("hud_ab", "i", "0", [
    "DIAGNOSTIC. 1 = alternate the 2D layer between both eyes",
    "and the left eye only every 300 frames, and log the frame",
    "rate for each. The 2D is missing from one eye half the",
    "time, so this is for measuring, not for playing."]),
  ("dump_strings", "i", "0", [
    "1 = dump the engine's string table to a file. Stalls the game",
    "for a few seconds."]),
 ]),
]

ALL = [(k, t, d, c) for _, ks in SECTIONS for (k, t, d, c) in ks]

# ---- check the table covers exactly the struct ----------------------------
h = io.open('src/vr.h', encoding='utf-8', newline='').read()
st = h.index('typedef struct {', h.index('/* settings, read from'))
fields = []
for line in h[st:h.index('} VrConfig', st)].split('\n'):
    m = re.match(r'\s*(int|float)\s+([a-z_]+);', line)
    if m:
        fields.append((m.group(2), m.group(1)))

names = set(k for k, _, _, _ in ALL)
missing = [n for n, _ in fields if n not in names]
extra = [k for k in names if k not in dict(fields)]
if missing or extra:
    raise SystemExit('table does not match VrConfig.\n  missing: %s\n  extra: %s'
                     % (missing, extra))
for k, t, d, _ in ALL:
    if dict(fields)[k] != ('int' if t == 'i' else 'float'):
        raise SystemExit('%s is declared %s but the table says %s'
                         % (k, dict(fields)[k], t))

# ---- the template ---------------------------------------------------------
lines = ['; A Kingdom for Keflings VR - settings',
         '; Delete opengl32.dll from this folder to remove the VR port.',
         '',
         '[vr]']
for title, keys in SECTIONS:
    if title:
        lines += ['', '; --- %s ---' % title]
    for k, t, d, comment in keys:
        if comment:
            if lines[-1] != '' and not lines[-1].startswith('; ---'):
                lines.append('')
            lines += ['; ' + c for c in comment]
        lines.append('%s = %s' % (k, d))
lines += ['',
          '; Do not edit: says which build wrote this file. A newer build',
          '; replaces the file rather than let a stale default win.']

c_lines = []
for line in lines:
    c_lines.append('                "%s%sn"' % (line.replace('"', BS + '"'), BS))
body = ('static void write_default_config(const char *path) {\n'
        '    FILE *f = fopen(path, "w");\n'
        '    if (f) {\n'
        '        fprintf(f,\n'
        + '\n'.join(c_lines) + '\n'
        '                "config_version = %d' + BS + 'n", KV_CONFIG_VERSION);\n')

s = io.open('src/vr.c', encoding='utf-8', newline='').read()
nl = '\r\n' if s.count('\r\n') > s.count('\n') / 2 else '\n'
i = s.index('static void write_default_config')
j = s.index('config_version = %d', i)
j = s.index(nl, s.index(';', j)) + len(nl)
s = s[:i] + body.replace('\n', nl) + s[j:]

# ---- read_all, from the same table ---------------------------------------
a = s.index('    GETI("enabled"')
b = s.index('#undef GETF', a)
gets = []
for k, t, d, _ in ALL:
    gets.append('    GET%s("%s", "%s", %s);' % ('I' if t == 'i' else 'F', k, d, k))
s = s[:a] + nl.join(gets).replace('\n', nl) + nl + s[b:]

# ---- write_all, same order ------------------------------------------------
a = s.index('    put_i(path, "enabled"')
b = s.index(nl + '}', a)
puts = ['    put_%s(path, "%s", g_vrcfg.%s);' % ('i' if t == 'i' else 'f', k, k)
        for k, t, d, _ in ALL]
s = s[:a] + nl.join(puts).replace('\n', nl) + s[b:]

# ---- the compiled-in defaults, same table, struct order -------------------
init = []
for name, ty in fields:
    d = dict((k, v) for k, _, v, _ in ALL)[name]
    init.append(d if ty == 'int' else (d + 'f' if '.' in d else d + '.0f'))
a = s.index('VrConfig g_vrcfg = {')
b = s.index('};', a)
s = s[:a] + 'VrConfig g_vrcfg = { ' + ', '.join(init) + ' };' + s[b + 2:]

io.open('src/vr.c', 'w', encoding='utf-8', newline='').write(s)
print('generated: %d settings, all three sources from one table' % len(ALL))
