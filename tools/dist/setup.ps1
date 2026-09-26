# A Kingdom for Keflings VR - setup.
#
# Finds the game and copies two files into it. That is the whole install, so
# this script is mostly about FINDING the install and being honest when it
# cannot.
#
# Where it looks, in order:
#   1. a folder dragged onto SETUP.bat
#   2. the registry key the game's own installer writes
#   3. the usual Program Files locations
#   4. it asks
#
# The registry key is the reliable one: measured on 19 Sep 2026, the whole
# footprint the installer leaves outside the game folder is two keys, and
# Install_Dir in the first of them is the path.

param([string]$Path = "", [switch]$Uninstall, [switch]$Collect,
      # Testing only: behave as if the game were not installed until the
      # installer step has run, so that path can be walked on a PC that has it.
      [switch]$PretendNotInstalled,
      # Testing only: answer every question with this instead of asking.
      [string]$Answer = "")

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $PSScriptRoot      # the package folder, above tools\
$Exe  = "A Kingdom for Keflings.exe"
$Ours = @("opengl32.dll", "openxr_loader.dll")
# The kit SETUP leaves inside the game folder, so the zip can be deleted and
# SETUP / UNINSTALL / the log collector are always to hand (26 Sep). An
# explicit list: a package folder can hold other things -- one tester's held
# the game's installer and its licence key -- and those must never be copied.
$KitDir = "VR"
$KitFiles = @("SETUP.bat", "UNINSTALL.bat", "collect-log.bat", "READ ME FIRST.txt",
              "version.txt", "opengl32.dll", "openxr_loader.dll", "tools\setup.ps1",
              "LICENSES\README.txt", "LICENSES\Apache-2.0.txt", "LICENSES\GPL-3.0.txt")

function Same-Folder {
    param([string]$a, [string]$b)
    try {
        return ([IO.Path]::GetFullPath($a).TrimEnd('\') -eq [IO.Path]::GetFullPath($b).TrimEnd('\'))
    } catch { return $false }
}

# Is this folder nothing but our kit (and the logs collect-log.bat writes into
# it), with no junction or link inside? Only then may it be deleted whole.
function Is-OnlyOurKit {
    param([string]$d)
    $items = @(Get-ChildItem -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue)
    foreach ($i in $items) {
        if ($i.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $false }
        if ($i.PSIsContainer) { continue }
        $rel = $i.FullName.Substring($d.TrimEnd('\').Length + 1)
        if ($KitFiles -notcontains $rel -and -not $rel.StartsWith("keflings-logs\")) { return $false }
    }
    return $true
}

function Say { param([string]$m) Write-Host ("   " + $m) }

function Is-GameFolder {
    param([string]$d)
    if ([string]::IsNullOrWhiteSpace($d)) { return $false }
    return (Test-Path -LiteralPath (Join-Path $d $Exe))
}

# A dropped folder arrives with whatever quoting Explorer felt like, and a
# trailing backslash inside quotes escapes the quote -- see
# trailing-backslash-eats-arguments. Trim both.
function Clean-Path {
    param([string]$p)
    if ($null -eq $p) { return "" }
    $p = $p.Trim().Trim('"')
    while ($p.EndsWith("\") -and $p.Length -gt 3) { $p = $p.Substring(0, $p.Length - 1) }
    return $p
}

$script:answers = @($Answer -split ',' | Where-Object { $_ -ne "" })
function Ask-Yes {
    param([string]$q)
    if ($Answer -ne "") {
        # testing: one answer per question, in order; "n" once they run out
        $a = "n"
        if ($script:answers.Count -gt 0) { $a = $script:answers[0]; $script:answers = @($script:answers | Select-Object -Skip 1) }
        Say ($q + " [Y/n] " + $a)
        return ($a -notmatch '^[nN]')
    }
    $a = Read-Host ("   " + $q + " [Y/n]")
    return ($a -notmatch '^\s*[nN]')
}

# The game's own installer, wherever a player is likely to have it: dropped
# on SETUP.bat, beside this package (a tester put it in a folder inside it, 25 Sep),
# in Downloads or on the Desktop. NinjaBee's is AKingdomForKeflings_Setup.exe.
function Is-Installer {
    param([string]$f)
    $n = [IO.Path]::GetFileName($f)
    return ($n -like "*Keflings*Setup*.exe" -or $n -like "*Keflings*Install*.exe")
}
function Find-Installer {
    param([string]$Given)
    $g = Clean-Path $Given
    if ($g -ne "" -and (Test-Path -LiteralPath $g)) {
        if ((Test-Path -LiteralPath $g -PathType Leaf) -and (Is-Installer $g)) { return $g }
        if (Test-Path -LiteralPath $g -PathType Container) {
            $hit = Get-ChildItem -LiteralPath $g -Filter "*.exe" -File -Recurse -Depth 2 -ErrorAction SilentlyContinue |
                   Where-Object { Is-Installer $_.FullName } | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    foreach ($d in @($Here, (Split-Path -Parent $Here),
                     (Join-Path $env:USERPROFILE "Downloads"),
                     [Environment]::GetFolderPath("Desktop"))) {
        if ([string]::IsNullOrWhiteSpace($d) -or -not (Test-Path -LiteralPath $d)) { continue }
        $hit = Get-ChildItem -LiteralPath $d -Filter "*.exe" -File -Recurse -Depth 2 -ErrorAction SilentlyContinue |
               Where-Object { Is-Installer $_.FullName } |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return ""
}

function Find-Game {
    param([string]$Given, [switch]$Quiet)

    $g = Clean-Path $Given
    if ($g -ne "") {
        # They may have dropped the exe itself rather than the folder.
        if ((Test-Path -LiteralPath $g) -and -not (Test-Path -LiteralPath $g -PathType Container)) {
            if (Is-Installer $g) { $g = "" }      # the INSTALLER: handled below
            else { $g = Split-Path -Parent $g }
        }
        if ($g -ne "") {
            if (Is-GameFolder $g) { Say "using the folder you dropped."; return $g }
            if (-not $Quiet) { Say "that folder has no $Exe in it:"; Say "  $g" }
        }
    }
    # Run from the copy SETUP leaves in the game's own VR folder: the game is
    # the folder that VR folder sits in.
    $parent = Split-Path -Parent $Here
    if ((Split-Path -Leaf $Here) -eq $KitDir -and (Is-GameFolder $parent)) {
        Say "running from the game's own $KitDir folder."
        return $parent
    }
    if ($script:pretend) { return "" }

    foreach ($k in @("HKLM:\SOFTWARE\WOW6432Node\NinjaBee\AKingdomForKeflings",
                     "HKLM:\SOFTWARE\NinjaBee\AKingdomForKeflings")) {
        try {
            $v = (Get-ItemProperty -LiteralPath $k -ErrorAction Stop).Install_Dir
            $v = Clean-Path $v
            if (Is-GameFolder $v) { Say "found it from the game's own registry entry."; return $v }
        } catch { }
    }

    foreach ($d in @("${env:ProgramFiles(x86)}\NinjaBee\AKingdomForKeflings",
                     "$env:ProgramFiles\NinjaBee\AKingdomForKeflings",
                     "C:\Program Files (x86)\NinjaBee\AKingdomForKeflings",
                     "C:\Program Files\NinjaBee\AKingdomForKeflings")) {
        if (Is-GameFolder $d) { Say "found it in the usual place."; return $d }
    }

    return ""
}

Write-Host ""
if ($Uninstall) { Write-Host "   A Kingdom for Keflings VR - remove" }
elseif ($Collect) { Write-Host "   A Kingdom for Keflings VR - collect the logs" }
else { Write-Host "   A Kingdom for Keflings VR - setup" }
Write-Host "   ----------------------"
Write-Host ""

$script:pretend = [bool]$PretendNotInstalled
$game = Find-Game $Path

# Not installed yet: offer the game's own installer if it is anywhere near,
# and carry on when it finishes (25 Sep: on a PC without the game, SETUP
# stopped, and dragging the installer onto it did nothing).
if ($game -eq "" -and -not $Uninstall -and -not $Collect) {
    $inst = Find-Installer $Path
    if ($inst -ne "") {
        Write-Host ""
        Say "A Kingdom for Keflings is not installed on this PC yet."
        Say "Its installer is here:"
        Say "  $inst"
        Write-Host ""
        if (Ask-Yes "Run it now? Setup carries on when it finishes.") {
            Say "Running the game's installer. Finish it, then come back here."
            try {
                Start-Process -FilePath $inst -WorkingDirectory (Split-Path -Parent $inst) -Wait
            } catch {
                Say ("It would not start: " + $_.Exception.Message)
            }
            $script:pretend = $false
            Write-Host ""
            $game = Find-Game "" -Quiet
            if ($game -ne "") { Say "The game is installed now." }
        }
    }
}
if ($game -eq "") {
    Write-Host ""
    Say "I could not find A Kingdom for Keflings on this PC."
    Write-Host ""
    if (-not $Uninstall -and -not $Collect) {
        Say "Not installed yet?  Install the game first, then run SETUP.bat again."
        Say "                    (Put its installer in this folder or in Downloads"
        Say "                    and SETUP will offer to run it for you.)"
    }
    Say "Installed somewhere unusual?  Drag the folder that has"
    Say "                    '$Exe' in it onto SETUP.bat,"
    Say "                    or paste that folder here."
    Write-Host ""
    $typed = ""
    if ($Answer -eq "") { $typed = Clean-Path (Read-Host "   Game folder (or just press Enter to close)") }
    if (Is-GameFolder $typed) { $game = $typed }
}
if ($game -eq "") {
    Say "Nothing was changed."
    Write-Host ""
    exit 1
}
Say "Game folder:"
Say "  $game"
Write-Host ""

# Everything needed to diagnose a run from another PC, in one folder beside
# this package. The port's log lives in the GAME folder; the first collector
# looked in LOCALAPPDATA and gathered everything except it.
if ($Collect) {
    $out = Join-Path $Here "keflings-logs"
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
    New-Item -ItemType Directory -Path $out | Out-Null
    $got = @()
    foreach ($f in @("keflings_vr_log.txt", "keflings_vr_log.prev.txt", "keflings_vr.ini")) {
        $p = Join-Path $game $f
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $out; $got += $f }
    }
    $p = Join-Path $game "log.txt"                  # the game's own log
    if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination (Join-Path $out "game_log.txt"); $got += "game_log.txt" }
    $la = Join-Path $env:LOCALAPPDATA "KeflingsVR"
    if (Test-Path -LiteralPath $la) {
        Get-ChildItem -LiteralPath $la -Filter "probe-*.log" -File |
            Sort-Object LastWriteTime -Descending | Select-Object -First 3 |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $out; $got += $_.Name }
    }
    # The PC itself: which OpenXR runtime a 32-bit game is handed is the
    # first question when VR does not start, and it is in the registry.
    $s = @()
    $s += "A Kingdom for Keflings VR log collection, " + (Get-Date -Format "yyyy-MM-dd HH:mm")
    $s += "PC: " + $env:COMPUTERNAME
    try { $s += "Windows: " + (Get-CimInstance Win32_OperatingSystem).Caption + " " + (Get-CimInstance Win32_OperatingSystem).Version } catch { }
    try { $s += "GPU: " + ((Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + " (driver " + $_.DriverVersion + ")" }) -join "; ") } catch { }
    $s += "Game folder: " + $game
    foreach ($f in $Ours) {
        $p = Join-Path $game $f
        if (Test-Path -LiteralPath $p) { $s += ("  {0}: {1} bytes, SHA256 {2}" -f $f, (Get-Item -LiteralPath $p).Length, (Get-FileHash -LiteralPath $p).Hash) }
        else { $s += "  ${f}: NOT INSTALLED" }
    }
    $s += ""
    $s += "OpenXR (32-bit view, which is the one this game uses):"
    foreach ($k in @("HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1", "HKLM:\SOFTWARE\Khronos\OpenXR\1")) {
        $s += "  " + $k
        try { $s += "    ActiveRuntime = " + (Get-ItemProperty -LiteralPath $k -ErrorAction Stop).ActiveRuntime } catch { $s += "    (no ActiveRuntime)" }
        foreach ($sub in @("AvailableRuntimes", "ApiLayers\Implicit")) {
            try {
                $props = (Get-ItemProperty -LiteralPath (Join-Path $k $sub) -ErrorAction Stop).PSObject.Properties |
                         Where-Object { $_.Name -notlike "PS*" }
                foreach ($pp in $props) { $s += ("    {0}: {1} = {2}" -f $sub, $pp.Name, $pp.Value) }
            } catch { }
        }
    }
    [IO.File]::WriteAllLines((Join-Path $out "this-pc.txt"), [string[]]$s)
    $got += "this-pc.txt"
    Say "Collected into:"
    Say "  $out"
    foreach ($g in $got) { Say ("    " + $g) }
    Write-Host ""
    Say "Zip that folder and send it. It holds no save games and no"
    Say "registration details -- only the logs and settings the port wrote."
    Write-Host ""
    exit 0
}

if ($Uninstall) {
    $gone = @()
    foreach ($f in $Ours) {
        $p = Join-Path $game $f
        if (Test-Path -LiteralPath $p) {
            try { [IO.File]::Delete($p); $gone += $f }
            catch { Say "could not remove $f -- is the game still running?"; exit 1 }
        }
    }
    if ($gone.Count -eq 0) { Say "The port was not installed here." }
    else { Say ("Removed: " + ($gone -join ", ")) }

    $before = Join-Path $game "fsall\settings.ini.before-vr"
    if (Test-Path -LiteralPath $before) {
        Copy-Item -LiteralPath $before -Destination (Join-Path $game "fsall\settings.ini") -Force
        Remove-Item -LiteralPath $before -Force -ErrorAction SilentlyContinue
        Say "Put your original screen resolution back."
    }
    # The port's own settings and logs too, so a reinstall starts fresh: an
    # old keflings_vr.ini survived a test PC's uninstall-then-setup (25 Sep) and
    # kept a pre-release control scheme. Only the port's own files -- never
    # saves; and in AppData only its probe logs, not the folder.
    $cleared = @()
    foreach ($f in @("keflings_vr.ini", "keflings_vr_log.txt", "keflings_vr_log.prev.txt")) {
        $p = Join-Path $game $f
        if (Test-Path -LiteralPath $p) {
            try { Remove-Item -LiteralPath $p -Force; $cleared += $f } catch { }
        }
    }
    $la = Join-Path $env:LOCALAPPDATA "KeflingsVR"
    if (Test-Path -LiteralPath $la) {
        Get-ChildItem -LiteralPath $la -Filter "probe-*.log" -File -ErrorAction SilentlyContinue |
            ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Force } catch { } }
    }
    if ($cleared.Count -gt 0) { Say ("Removed the port's settings and logs: " + ($cleared -join ", ")) }

    # And the kit SETUP left in the game folder. Running from inside it, this
    # batch file is still open, so the folder goes a few seconds after it closes.
    $vr = Join-Path $game $KitDir
    if (Test-Path -LiteralPath $vr -PathType Container) {
        if (-not (Is-OnlyOurKit $vr)) {
            Say "Left the $KitDir folder in the game folder: it holds files that"
            Say "are not this port's. Delete it yourself if you do not need them:"
            Say "  $vr"
        } elseif (Same-Folder $Here $vr) {
            # UNINSTALL.bat's own window stands IN this folder (pushd), and
            # Windows will not delete a folder a process is standing in -- so
            # keep trying, quietly, until that window has closed (10 minutes
            # at most). The path travels in the environment: no quoting.
            $env:KV_REMOVE_KIT = $vr
            # ...and it must not stand in that folder itself: started from here
            # it would inherit the batch's current folder, the VR folder.
            Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -WorkingDirectory $env:TEMP -ArgumentList @(
                '-NoProfile', '-Command',
                'for ($i = 0; $i -lt 300; $i++) { Start-Sleep 2; if (-not (Test-Path -LiteralPath $env:KV_REMOVE_KIT)) { break }; try { Remove-Item -LiteralPath $env:KV_REMOVE_KIT -Recurse -Force -ErrorAction Stop } catch { } }')
            Say "The $KitDir folder in the game folder removes itself when this window closes."
        } else {
            try { Remove-Item -LiteralPath $vr -Recurse -Force; Say "Removed the $KitDir folder from the game folder." }
            catch { Say "Could not remove $vr -- delete it yourself." }
        }
    }
    Write-Host ""
    Say "The game is exactly as it was. Your saves are untouched."
    Write-Host ""
    exit 0
}

foreach ($f in $Ours) {
    if (-not (Test-Path -LiteralPath (Join-Path $Here $f))) {
        Say "$f is missing from this folder. Extract the whole zip and try again."
        Write-Host ""
        exit 1
    }
}

# A running game holds the DLL open, and Program Files may need elevation.
# Both produce the same useless "access denied" unless we say which it is.
for ($try = 0; @(Get-Process -Name "A Kingdom for Keflings" -ErrorAction SilentlyContinue).Count -gt 0; $try++) {
    # The game's own installer can start it at the end; wait rather than quit.
    if ($try -ge 3 -or $Answer -ne "") {
        Say "The game is still running. Close it, then run SETUP.bat again."
        Write-Host ""
        exit 1
    }
    Say "The game is running. Close it, then press Enter here."
    [void](Read-Host "   ")
}

foreach ($f in $Ours) {
    $dst = Join-Path $game $f
    try {
        Copy-Item -LiteralPath (Join-Path $Here $f) -Destination $dst -Force
    } catch [System.UnauthorizedAccessException] {
        Say "Windows would not let me write into that folder."
        Say "Right-click SETUP.bat and choose 'Run as administrator'."
        Write-Host ""
        exit 1
    } catch {
        Say ("Could not copy " + $f + ": " + $_.Exception.Message)
        Write-Host ""
        exit 1
    }
}

# Verify what landed, rather than trusting the copy.
$bad = @()
foreach ($f in $Ours) {
    $a = (Get-FileHash -LiteralPath (Join-Path $Here $f)).Hash
    $b = (Get-FileHash -LiteralPath (Join-Path $game $f)).Hash
    if ($a -ne $b) { $bad += $f }
}
if ($bad.Count -gt 0) {
    Say ("These did not copy correctly: " + ($bad -join ", "))
    Write-Host ""
    exit 1
}

Say "Installed, and checked byte for byte:"
foreach ($f in $Ours) { Say ("  " + $f) }
Write-Host ""

# The kit into the game folder, unless this IS that copy.
$vr = Join-Path $game $KitDir
if (-not (Same-Folder $Here $vr)) {
    $kitBad = @()
    if ((Test-Path -LiteralPath $vr) -and -not (Is-OnlyOurKit $vr)) {
        Say "The game folder already has a $KitDir folder with other files in it,"
        Say "so the kit was not copied there. The port itself is installed."
    } else {
        foreach ($f in $KitFiles) {
            $src = Join-Path $Here $f
            if (-not (Test-Path -LiteralPath $src)) { continue }
            $dst = Join-Path $vr $f
            try {
                [void][IO.Directory]::CreateDirectory((Split-Path -Parent $dst))
                Copy-Item -LiteralPath $src -Destination $dst -Force
                if ((Get-FileHash -LiteralPath $src).Hash -ne (Get-FileHash -LiteralPath $dst).Hash) { $kitBad += $f }
            } catch { $kitBad += $f }
        }
        if ($kitBad.Count -gt 0) {
            Say ("Could not copy the kit into the game folder: " + ($kitBad -join ", "))
        } else {
            Say "A copy of SETUP, UNINSTALL and the log collector is now in:"
            Say "  $vr"
            Say "Run them from there any time. You can delete the zip and this folder."
        }
    }
    Write-Host ""
}

# Is anything there to give it a headset? Without it the game just plays on
# the monitor, which is correct but looks like the port did nothing.
$vrApps = [ordered]@{ "VirtualDesktop.Streamer" = "Virtual Desktop"; "vrserver" = "SteamVR";
                      "OVRServer_x64" = "the Meta Quest Link app" }
$running = @()
foreach ($k in $vrApps.Keys) {
    if (Get-Process -Name $k -ErrorAction SilentlyContinue) { $running += $vrApps[$k] }
}
if ($running.Count -gt 0) {
    Say ("Your headset software is running: " + ($running -join ", ") + ".")
} else {
    Say "No headset software is running yet. Start Virtual Desktop (or"
    Say "SteamVR, or the Meta Quest Link app) and put the headset on first --"
    Say "without it the game simply plays on the monitor."
}
Say "To remove the port later, run UNINSTALL.bat in the game folder's $KitDir folder."
Write-Host ""
if (Ask-Yes "Start the game now?") {
    Start-Process -FilePath (Join-Path $game $Exe) -WorkingDirectory $game
    Say "Starting. Have fun!"
}
Write-Host ""
exit 0
