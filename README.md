# A Kingdom for Keflings VR

Play **A Kingdom for Keflings** (PC) by NinjaBee (Wahoo Studios) in a VR headset. The kingdom
stands in front of you as a living model you look around, the menus,
blueprints and tech tree sit on a screen in the world, and you can play with
the controller like the Xbox version or point and click with the grip mouse.

It is two files next to the game. Nothing in the game is changed, and one
click takes it back out.

## Install

1. **Download** `A-Kingdom-for-Keflings-VR-1.0.zip` from
   [Releases](https://github.com/GameOrDie007/A-Kingdom-for-Keflings-VR/releases)
   and extract it anywhere.
2. **Run `SETUP.bat`.** It finds the game, copies the two files in, and
   checks they arrived intact. It also leaves a copy of itself, with
   `UNINSTALL.bat` and the log collector, in a `VR` folder inside the game
   folder, so you can delete the zip afterwards.
   - Game not installed yet? Download NinjaBee's installer
     (`AKingdomForKeflings_Setup.exe`) from [their site](https://www.ninjabee.com/games/a_kingdom_for_keflings/).
     Put it in the extracted folder, in Downloads or on the Desktop, or drag
     it onto `SETUP.bat`, and setup offers to run it first. The installer is
     the free demo; the license key from
     [NinjaBee's itch.io page](https://ninjabee.itch.io/a-kingdom-for-keflings) unlocks the full game.
   - Installed somewhere unusual? Drag the game's folder onto `SETUP.bat`.
3. **Start your headset software** (Virtual Desktop, SteamVR or the Meta
   Quest Link app), then start the game. Setup offers to start it for you.

There is nothing to configure. The port sets the game to your screen's
resolution itself.

To remove it, run `UNINSTALL.bat` from the game folder's `VR` folder. It takes
the port out, puts your original screen resolution back, removes the port's
settings and logs, and then the `VR` folder itself. Your saves are never
touched.

## What you need

- The PC version of A Kingdom for Keflings, installed. Buy it on
  [NinjaBee's itch.io page](https://ninjabee.itch.io/a-kingdom-for-keflings) (a license key that
  unlocks NinjaBee's free demo installer into the full game). This project
  contains no part of the game.
- A headset with a **32-bit OpenXR runtime**, because the game is a 32-bit
  program. Virtual Desktop and SteamVR both provide one. Without it the game
  simply runs on the monitor, and the log says why.

## Controls

| Control | What it does |
|---|---|
| Left stick | Move |
| Look | In the world, what you look at is what A acts on; the game's highlight shows it |
| Right grip, held | The grip mouse: the game's arrow follows the right controller over the world, the HUD's icons and the menus, and the trigger clicks |
| Right grip, tap | Tech tree |
| Right trigger | Click |
| A | Select, pick up, put down |
| B | Cancel, or the pause menu when there is nothing to cancel |
| X | Kick |
| Y | Remove the hat of the kefling you are holding, to give it a new job |
| Left grip | Blueprints |
| Both grips | Recenter the menus in front of you |
| Menu | What's Next (the tutorial help) |
| Menu, held | Photo mode: hides the HUD and menus for a clean screenshot |

Punch and demolish a building by holding the right grip, pointing at the fist
icon and pulling the trigger, the same way the flat game does it with the
mouse.

**Left-handed?** Set `leftorium = 1` in `keflings_vr.ini` in the game folder.
The grip mouse, the trigger, A/B and pointing move to the left controller.

## Settings

`keflings_vr.ini` appears in the game folder on the first run, and every
setting in it is commented. The ones worth knowing: `world_scale` (how big the
kingdom is), `menu_size`, `hud_distance`, `menu_height` and `menu_tilt` (where
the menu screen sits), and `draw_distance`.

## Known issues

- With the grip mouse, pointing past the edge of the menu screen still picks
  in the world (the game's highlight shows where), but the arrow itself is
  only drawn on that screen.
- If you change the resolution in the game's own options, restart the game
  once; until you do, parts of the world at the edges of your view can be
  missing. There should be no need to change it.
- Occasionally a short stall as a menu closes.

If something goes wrong, run `collect-log.bat` from the game folder's `VR` folder and
attach the `keflings-logs` folder it makes to an
[issue](https://github.com/GameOrDie007/A-Kingdom-for-Keflings-VR/issues).
It contains no saves and no registration details.

## Building from source

Needs Visual Studio 2019 Build Tools (the 32-bit x86 compiler) and Python 3.

    build.bat                          builds build\opengl32.dll
    python tools\make_test_package.py  makes the release folder and zip in dist\

`tools\gen_config.py` generates the settings code in `src\vr.c` from one
table; run it after changing a setting.

## Credits and licence

- **A Kingdom for Keflings** is made by NinjaBee (Wahoo Studios), and all of
  it is theirs: [get it on itch.io](https://ninjabee.itch.io/a-kingdom-for-keflings). This is an unofficial,
  fan-made VR port, not made by, affiliated with or endorsed by them. It
  contains no part of the game; you need your own copy.
- **The OpenXR Loader** is by The Khronos Group, redistributed unmodified
  under the Apache License 2.0 (`extern/licenses/Apache-2.0.txt`).
- **The VR port** is by Ryan Moore (Game Or Die). It is original work: a proxy
  for the system OpenGL library that forwards the game's calls and adds the
  VR rendering around them. Licensed under the GNU General Public License,
  version 3; see `LICENSE`.

---

**Get an email when the next port ships:** follow [Game Or Die on Patreon](https://www.patreon.com/cw/GameOrDie) for free. Ports are never paywalled.
