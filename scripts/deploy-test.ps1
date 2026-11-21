# Copies the latest mod build into the Windows test folder.
#
#   powershell -ExecutionPolicy Bypass -File ".\scripts\deploy-test.ps1"
#
# Copies ddraw.dll and Armymen.exe.manifest. The ini is only copied with -Ini (so your own settings
# are kept unless you ask for the new defaults):
#
#   powershell -ExecutionPolicy Bypass -File "...\deploy-test.ps1" -Ini
param([switch]$Ini)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)    # ...\REA
$src  = Join-Path $root "armymen-1-modern\src"
$dst  = Join-Path $root "Army Men 1280 test"

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like "$dst\*" }
if ($running) {
    Write-Host "The game is running from the test folder - close it first, then run this again." -ForegroundColor Red
    exit 1
}

$files = @("ddraw.dll", "Armymen.exe.manifest")
if ($Ini) { $files += "armymen-mod.ini" }
foreach ($f in $files) {
    Copy-Item (Join-Path $src $f) $dst -Force -ErrorAction Stop
    Write-Host ("copied {0,-22} {1}" -f $f, (Get-Item (Join-Path $dst $f)).LastWriteTime)
}
# Widescreen menu art (hdart.c): REA\wide_1080p\*.png -> <game>\widescreen\
$wide = Join-Path $root "wide_1080p"
if (Test-Path $wide) {
    $wdst = Join-Path $dst "widescreen"
    New-Item -ItemType Directory -Force $wdst | Out-Null
    Copy-Item (Join-Path $wide "*.png") $wdst -Force -ErrorAction Stop
    Write-Host ("copied {0} widescreen pictures to widescreen\" -f (Get-ChildItem (Join-Path $wdst "*.png")).Count)
}
# Upscaled pictures (hdsprites.c): REA\Army Men 1280 test upscale\** -> <game>\hd\** (only newer files)
$up = Join-Path $root "Army Men 1280 test upscale"
if (Test-Path $up) {
    $hdst = Join-Path $dst "hd"
    $n = 0
    Get-ChildItem $up -Recurse -File -Include *.bmp, *.png | Where-Object { $_.FullName -notlike "*\widescreen\*" } | ForEach-Object {
        $rel = $_.FullName.Substring($up.Length + 1)
        $target = Join-Path $hdst $rel
        if (-not (Test-Path $target) -or (Get-Item $target).LastWriteTime -lt $_.LastWriteTime) {
            New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
            Copy-Item $_.FullName $target -Force -ErrorAction Stop
            $n++
        }
    }
    Write-Host ("copied {0} upscaled pictures to hd\" -f $n)
}
# Upscaled HUD (hdsprites.c, made by tooling\scripts\hud_hd.py): REA\hd_pack\hud\* -> <game>\hd\hud\ (when newer)
$hud = Join-Path $root "hd_pack\hud"
if (Test-Path $hud) {
    $hdst = Join-Path $dst "hd\hud"
    New-Item -ItemType Directory -Force $hdst | Out-Null
    $n = 0
    Get-ChildItem $hud -File | ForEach-Object {
        $t = Join-Path $hdst $_.Name
        if (-not (Test-Path $t) -or (Get-Item $t).LastWriteTime -lt $_.LastWriteTime) { Copy-Item $_.FullName $t -Force -ErrorAction Stop; $n++ }
    }
    if ($n) { Write-Host ("copied {0} HUD pictures to hd\hud\" -f $n) }
}
# Upscaled battlefield (hdworld.c): REA\hd_pack\world.pak -> <game>\hd\world.pak (when newer)
$pak = Join-Path $root "hd_pack\world.pak"
if (Test-Path $pak) {
    $pdst = Join-Path $dst "hd\world.pak"
    New-Item -ItemType Directory -Force (Split-Path $pdst) | Out-Null
    if (-not (Test-Path $pdst) -or (Get-Item $pdst).LastWriteTime -lt (Get-Item $pak).LastWriteTime) {
        Copy-Item $pak $pdst -Force -ErrorAction Stop
        Write-Host "copied world.pak to hd\"
    }
}
# Upscaled videos (hdvideo.c): REA\hd_pack\AVI\*.mp4 -> <game>\hd\AVI\ (when newer)
$avi = Join-Path $root "hd_pack\AVI"
if (Test-Path $avi) {
    $vdst = Join-Path $dst "hd\AVI"
    New-Item -ItemType Directory -Force $vdst | Out-Null
    $n = 0
    Get-ChildItem $avi -Filter *.mp4 | ForEach-Object {
        $t = Join-Path $vdst $_.Name
        if (-not (Test-Path $t) -or (Get-Item $t).LastWriteTime -lt $_.LastWriteTime) { Copy-Item $_.FullName $t -Force -ErrorAction Stop; $n++ }
    }
    Write-Host ("copied {0} upscaled videos to hd\AVI\" -f $n)
}
# Army Men II sound pack (audio.c, [audio] am2_sounds; made by scripts\am2_sounds.py): REA\am2_sounds\* ->
# <game>\am2_sounds\ (when newer)
$snd = Join-Path $root "am2_sounds"
if (Test-Path $snd) {
    $sdst = Join-Path $dst "am2_sounds"
    New-Item -ItemType Directory -Force $sdst | Out-Null
    $n = 0
    Get-ChildItem $snd -File -Include *.wav, *.txt -Recurse | ForEach-Object {
        $t = Join-Path $sdst $_.Name
        if (-not (Test-Path $t) -or (Get-Item $t).LastWriteTime -lt $_.LastWriteTime) { Copy-Item $_.FullName $t -Force -ErrorAction Stop; $n++ }
    }
    Write-Host ("copied {0} Army Men II sound files to am2_sounds\" -f $n)
}
Write-Host "Done. The first line of armymen-mod.log shows the build time once the game has started."
