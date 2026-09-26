"""Build a drop-in test package for other machines.

What a second machine needs is small -- two DLLs beside the exe -- but what it
needs to be TOLD is not, and a tester on someone else's PC cannot be debugged
over the shoulder.  So the package carries instructions written for someone who
has never seen this port, and a one-click log collector, because a run whose log
does not come back is a run that did not happen.

What is deliberately NOT in it:

 - `akfk_license_key.txt`. It is in the game folder, it is the player's
   registration, and it must never travel. The build refuses if it finds one
   anywhere near the output.
 - saves, the ini, or anything else generated. The ini is written by the DLL on
   first run; shipping one would impose this machine's tuning, including its
   resolution, on every other machine.
 - winmm.dll and dinput8.dll. They are watchers, they were for answering
   questions already answered, and every DLL beside an exe is a thing that can
   go wrong on hardware nobody has tried.
"""
import io
import os
import shutil

VERSION = '1.0'
OUT = os.path.join('dist', 'A-Kingdom-for-Keflings-VR-' + VERSION)
FORBIDDEN = ('license', 'licence', 'key', '.kef', 'prefs.set')

if os.path.isdir(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)

FILES = [
    (os.path.join('build', 'opengl32.dll'), 'opengl32.dll'),
    (os.path.join('extern', 'openxr', 'bin', 'openxr_loader.dll'),
     'openxr_loader.dll'),
]
for src, name in FILES:
    if not os.path.exists(src):
        raise SystemExit('missing: ' + src)
    shutil.copy2(src, os.path.join(OUT, name))
    print('  +', name, '%d bytes' % os.path.getsize(src))

# openxr_loader.dll is somebody else's binary: the Khronos OpenXR Loader,
# Apache 2.0, whose section 4 says recipients get a copy of the licence.
# Shipping it without one is the whole reason this block exists.
LIC = os.path.join(OUT, 'LICENSES')
os.makedirs(LIC)
APACHE = os.path.join('extern', 'licenses', 'Apache-2.0.txt')
if not os.path.exists(APACHE):
    raise SystemExit('missing the Apache 2.0 text: ' + APACHE)
shutil.copy2(APACHE, os.path.join(LIC, 'Apache-2.0.txt'))
# The port's own licence, GPL-3.0 (chosen 25 Sep 2026).
# LICENSE at the top in the public repo, tools/public/LICENSE in the private one.
GPL = 'LICENSE' if os.path.exists('LICENSE') else os.path.join('tools', 'public', 'LICENSE')
shutil.copy2(GPL, os.path.join(LIC, 'GPL-3.0.txt'))
io.open(os.path.join(LIC, 'README.txt'), 'w',
        encoding='utf-8', newline='\r\n').write('''What is in this folder, and why
===============================

openxr_loader.dll
    The OpenXR Loader, version 1.1.63, from The Khronos Group.
    Copyright (C) 2017-2026 The Khronos Group Inc. and others.
    Licensed under the Apache License, Version 2.0 -- the full text is in
    Apache-2.0.txt beside this file. It is redistributed unmodified.
    Source: https://github.com/KhronosGroup/OpenXR-SDK

opengl32.dll
    The VR port itself. Original work, written for this project. It is a
    proxy for the system OpenGL library: it forwards every call the game
    makes and adds the VR rendering around them. It contains no code from
    the game and none from any other project.
    Licensed under the GNU General Public License, version 3 -- the full
    text is in GPL-3.0.txt beside this file. The source is at
    https://github.com/GameOrDie007/A-Kingdom-for-Keflings-VR

A Kingdom for Keflings
    The game is by NinjaBee and is not included here, in any part. This
    package is two DLLs. You supply your own copy of the game.
''')
print('  + LICENSES/ (GPL-3.0 and Apache 2.0 texts, and what covers what)')

# Which build this folder is, without opening a log: the version, the commit
# and the date. A folder on another PC is otherwise identified by a timestamp.
import datetime, subprocess
try:
    rev = subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD']).decode().strip()
except Exception:
    rev = 'unknown'
io.open(os.path.join(OUT, 'version.txt'), 'w', encoding='ascii', newline='\r\n').write(
    'A Kingdom for Keflings VR ' + VERSION + '\ncommit ' + rev + '\nbuilt ' +
    datetime.date.today().isoformat() + '\n')
print('  + version.txt (' + VERSION + ', ' + rev + ')')

# The installer. A cmd wrapper thin enough to read, and the work in
# PowerShell. Both are kept in tools/dist so they are version-controlled
# next to the code rather than hand-edited inside a release folder.
os.makedirs(os.path.join(OUT, 'tools'))
for src, dst in ((os.path.join('tools', 'dist', 'SETUP.bat'), 'SETUP.bat'),
                 (os.path.join('tools', 'dist', 'UNINSTALL.bat'), 'UNINSTALL.bat'),
                 (os.path.join('tools', 'dist', 'setup.ps1'),
                  os.path.join('tools', 'setup.ps1'))):
    if not os.path.exists(src):
        raise SystemExit('missing: ' + src)
    shutil.copy2(src, os.path.join(OUT, dst))
    print('  +', dst)

io.open(os.path.join(OUT, 'collect-log.bat'), 'w',
        encoding='ascii', newline='\r\n').write('''@echo off
REM Gather the port's logs so they can be sent back. Writes one folder,
REM keflings-logs, next to this file; nothing is uploaded and nothing leaves
REM the machine. Finds the game the same way SETUP.bat does -- drag the game's
REM folder onto this file if it cannot.
setlocal
REM pushd, not cd: cmd cannot stand in a network folder, and pushd maps one.
pushd "%~dp0"
set "DROP=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\\setup.ps1" -Collect -Path "%DROP%"
echo.
pause
''')

io.open(os.path.join(OUT, 'READ ME FIRST.txt'), 'w',
        encoding='utf-8', newline='\r\n').write("""A Kingdom for Keflings VR -- version """ + VERSION + """
================================================

Plays NinjaBee's A Kingdom for Keflings in a VR headset: the kingdom stands
in front of you as a model you walk around, and the menus sit on a panel in
the world. It is two files you drop next to the game, and one file you
delete to undo it.

Version 1.0: finished, and confirmed in the headset.


INSTALL
-------

Run SETUP.bat. It finds the game, copies two files into it, and checks they
arrived intact. It also puts a copy of itself -- SETUP, UNINSTALL and the log
collector -- in a folder called VR inside the game folder, so you can delete
this zip afterwards and run them from there any time. At the end it tells you
whether your headset software is running, and offers to start the game.

  - The game not installed yet? Put NinjaBee's installer
    (AKingdomForKeflings_Setup.exe) in this folder, in Downloads or on the
    Desktop -- or drag it onto SETUP.bat -- and SETUP offers to run it,
    then carries on when it finishes.
  - Installed somewhere unusual? Drag the game's folder onto SETUP.bat.

Start your headset software (Virtual Desktop, SteamVR or the Meta Quest Link
app) before the game. Without it the game simply plays on the monitor.
There is nothing to configure.

If you would rather do it by hand, it is genuinely just two files: copy
opengl32.dll and openxr_loader.dll into the folder that has
"A Kingdom for Keflings.exe" in it, usually
C:\\Program Files (x86)\\NinjaBee\\AKingdomForKeflings.


REMOVE IT
---------

Run UNINSTALL.bat from the VR folder inside the game folder (or from this
folder). It takes both files back out, puts your original screen
resolution back, and removes the port's own settings and logs, leaving the
game exactly as it was -- the VR folder removes itself once the window
closes. Your saves are never touched.

By hand: delete opengl32.dll from the game folder. That alone is enough to
turn the port off.

One thing it changes outside itself: the game's own resolution, in
fsall\\settings.ini, is set to match your monitor, because the game lays its
menus out for those numbers and gets them wrong otherwise. Your original is
kept beside it as settings.ini.before-vr -- copy that back if you want it.


WHAT YOU GET
------------

 * The whole game in VR, in stereo, with the menus, blueprints and tech tree
   on a screen standing in the world, and every season of the year.
 * The grip mouse. Play with the controller as on the Xbox -- look at
   something and press A -- or hold the right grip and the game's own arrow
   appears on the right controller: point and pull the trigger to click,
   anywhere in the world, on the HUD or in a menu. Let go, and you are back
   to the controller. A quick tap of the right grip still opens the tech tree.
 * Photo mode: hold the menu button to hide the HUD and menus for a clean
   screenshot.
 * The Leftorium: left-handed play (leftorium = 1 in keflings_vr.ini).
 * Squeeze both grips to bring the menus, blueprints and tech tree back in
   front of you. The headset's own recenter does the same.
 * Y removes a kefling's hat, so you can give it a new job, as on the Xbox.
   The menu button is What's Next; B cancels, or pauses.
 * A steady 90 fps in a busy kingdom on a Quest 3 over Virtual Desktop.
   Two things do it:

     - Scenery that looks alike -- trees, rocks, crystals, hillsides -- is
       drawn in one go instead of one piece at a time, for both eyes.
     - The game draws an invisible highlight layer over most of the world
       every frame. Those draws are skipped while they are invisible, and
       drawn again the moment they are not. The graphics card is asked to
       confirm they really draw nothing before any is skipped, and again
       every few seconds.
   The picture is meant to be exactly the game's own. Both were checked
   against the game's own drawing on ten saves and through seven game-years
   of seasons -- snow, falling leaves, objects fading in and out -- frame by
   frame.


WHAT YOU NEED
-------------

 * The PC version of A Kingdom for Keflings, installed and already working.
   Run it once without the port first.
 * A headset with a 32-BIT OpenXR runtime. The game is a 32-bit program, and
   some runtimes only install a 64-bit one. Virtual Desktop (its VDXR
   runtime) and SteamVR both provide 32-bit. If yours does not, the port
   says so in its log and the game simply runs on the monitor.


CONTROLS
--------

Quest controllers, mapped to the keys the game itself lists:

    left stick        move
    look              in the world, what you look at is what A acts on --
                      the game's highlight shows it
    right grip, held  the mouse: the game's arrow follows the right
                      controller -- over the world, the HUD's icons and
                      the menus -- and the trigger clicks where it is.
                      Let go to go back.
    right grip, tap   the tech tree
    right trigger     click. In the world without the grip, it clicks
                      what you look at, like A.
    A                 Enter -- select, pick up, put down
    B                 Escape -- cancel, or the pause menu when there is
                      nothing to cancel
    X                 kick
    Y                 remove the hat of the kefling you are holding (C),
                      to give it a new job -- as on the Xbox
    left grip         blueprints
    both grips        recenter: the menus, blueprints and tech tree move
                      to straight in front of you. (Held for two seconds,
                      it hands the cursor to a real mouse, and back.)
    menu              What's Next -- the tutorial help (F1)
    menu, held        photo mode: the HUD and menus disappear so you can
                      take a clean screenshot with the headset's own
                      capture. Hold it again, or press B, to bring them
                      back.

The headset's own recenter (hold the Quest's menu button) re-centers the
menus too.

Left-handed? Set leftorium = 1 in keflings_vr.ini (the Leftorium): the
grip mouse, the click trigger, A/B and pointing move to the left
controller, and walking to the right. The menu button stays on the left,
because it is the only one there is.

To punch and demolish a building, hold the right grip, point at the fist
icon and pull the trigger. That is how the flat game does it too, with the
mouse; there is no key for it.

A keyboard and mouse work as they always did.


SETTINGS
--------

keflings_vr.ini appears in the game folder on first run. Every setting is
commented. The few worth knowing:

    world_scale     how big the kingdom is. Larger makes it a smaller model
                    nearer your face.
    menu_size       how big the menu panel is, in degrees.
    hud_distance    how far away that panel sits, in metres.
    menu_height     how far the menu panel is raised above your eyes, in
                    metres (0.5). It moves straight up; menu_tilt is kept.
    menu_tilt       how far the menu panel leans back, in degrees; negative
                    leans its top toward you. The game looks down on the
                    kingdom, so an upright panel can seem to lean back;
                    the default, -40, stands it up in the world. 0 is
                    upright.
    draw_distance   how far the game draws. Raise it if distant parts of the
                    map pop in.
    enabled         0 runs the game flat with the port still installed.
    merge_group     9 draws alike scenery in one go (0.10.0's speed-up).
    skip_noop       1 skips the invisible highlight layer.
                    If anything ever looks different from the flat game --
                    flickers, a missing object, a wrong shade -- set both to
                    0 to draw everything the game's own way, and send the log
                    with a word about what and where.


KNOWN ISSUES
------------

 * With the grip mouse, pointing past the edge of the HUD's screen still
   picks in the world -- the game's highlight shows where -- but the arrow
   itself is only drawn on that screen.
 * The port sets the game's resolution to your screen's every time it
   starts, so there is no need to change it. If you do change it in the
   game's own options, restart the game once: until you do, parts of the
   world at the edges of your view can be missing.
 * Occasional stalls -- one short one as a menu closes, while the scenery
   is gathered up again. The port records any frame over 25 ms in its log
   along with where the time went, so if you hit them the log will say
   whether it was the game or the headset link.


NOT INCLUDED, AND WHY
---------------------

Things our other VR ports have that this one does not:

 * Switching games from the headset: there is one game.
 * Walking around the room, crouching, a comfort vignette: the kingdom is a
   model on a table in front of you, not a world you stand in.
 * A VR options menu: the game's menus are its own and cannot be extended.
   Every setting is in keflings_vr.ini, commented.
 * Controller vibration: nothing in a city builder calls for it.


IF IT DOES NOT START IN VR
--------------------------

The game will still run, on the monitor. The reason is in the log, at the
top: it names your OpenXR runtime and says what failed. The most common
cause is that only a 64-bit runtime is installed.


SENDING A LOG BACK
------------------

Run collect-log.bat, from the VR folder inside the game folder. It gathers
everything needed into one folder, keflings-logs, next to it -- zip that
folder and send it.

The port writes its log into the game folder, next to the exe:

    keflings_vr_log.txt        the run you just did
    keflings_vr_log.prev.txt   the one before it

That file names the build, your GPU, your screen size, your OpenXR runtime
and the frame timings. Nothing collected contains save games or
registration details.


CREDITS AND LICENCES
--------------------

A Kingdom for Keflings is by NinjaBee. This is an unofficial VR port, not
made by, affiliated with or endorsed by them, and it includes no part of the
game -- you supply your own copy.

The OpenXR Loader is by The Khronos Group, redistributed unmodified under
the Apache License 2.0. See the LICENSES folder, which carries the full text
and says what covers what.

The VR port is by Ryan Moore (Game Or Die). It is original work written for
this project: a proxy for the system OpenGL library that forwards the game's
calls and adds the VR rendering around them. It is licensed under the GNU
General Public License, version 3, and its source is at
https://github.com/GameOrDie007/A-Kingdom-for-Keflings-VR
""")

for root, _dirs, files in os.walk(OUT):
    for f in files:
        low = f.lower()
        if any(b in low for b in FORBIDDEN):
            raise SystemExit('REFUSING: %s looks like it should not ship' % f)

print()
print('package: %s' % OUT)
for f in sorted(os.listdir(OUT)):
    print('   %-22s %d bytes' % (f, os.path.getsize(os.path.join(OUT, f))))
print()
print('checked: nothing matching %s is in it' % ', '.join(FORBIDDEN))

# The zip, every time, from the folder just built. It used to be made by hand
# and was a day stale on 25 Sep -- a two-PC test would have installed the
# build from before every fix of that day.
import zipfile
ZIP = OUT + '.zip'
with zipfile.ZipFile(ZIP, 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _dirs, files in os.walk(OUT):
        for f in sorted(files):
            full = os.path.join(root, f)
            z.write(full, os.path.relpath(full, os.path.dirname(OUT)).replace(os.sep, '/'))
print('zip: %s (%d bytes)' % (ZIP, os.path.getsize(ZIP)))
