<#
.SYNOPSIS
  First install of the "loader" OTA scheme over USB: bootloader, partition
  table, otadata, the OTA loader (factory) and the app (ota_0) in one go.

.DESCRIPTION
  The same on ESP32, ESP32-C3, ESP32-C6 and ESP32-S3. The app env must use a
  loader partition table (esp-rack/partitions/loader_*.csv) and be built
  first (pio run -e <env>); the loader for the same chip too
  (pio run -d esp-rack/loader -e <esp32|esp32c3|esp32c6|esp32s3>).
  Offsets come from the env's partitions.bin, never from this script.
  Checks before writing: the table is a loader table (factory + ota_0, no
  ota_1), both images are for -Chip, the loader is the loader (project
  "esprack-loader") and the app is not, each one fits its partition.

  NVS and the file system are not touched (no -EraseAll): a board switched
  from a dual table with the same data offsets keeps its settings and files
  (docs/plans/lib-ota-loader.md, "Switching a board").

  The bootloader: the app env's (Arduino's), unless the loader was built
  with a boot button (build.ps1 -BootPin, its sdkconfig has
  CONFIG_BOOTLOADER_FACTORY_RESET=y): then the loader build's own, which
  boots the loader while that pin is held at reset. Its flash mode and
  frequency (image header) must equal the app bootloader's; the size is
  set from the chip (--flash-size detect) for both. -LoaderOnly then
  writes that bootloader too.

.EXAMPLE
  .\flash_loader.ps1 -Chip c3 -Port COM7                 # env loader-c3 in the current project
  .\flash_loader.ps1 -Chip s3 -Env my-s3-loader -Project D:\proj\gateway -Port COM5
  .\flash_loader.ps1 -Chip c6 -Port COM9 -StartIn loader # boots into the loader first
  .\flash_loader.ps1 -Chip esp32 -Port COM3 -LoaderOnly  # replaces just the loader (and its boot-button bootloader)
  .\flash_loader.ps1 -Chip c3 -DryRun                    # checks and prints the command
#>
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('esp32', 'c3', 'c6', 's3', 'esp32c3', 'esp32c6', 'esp32s3')]
  [string]$Chip,
  [string]$Port = '',                # empty: esptool picks the port
  [Alias('Env')]
  [string]$PioEnv = '',              # the app's env; default: loader-<chip>
  [string]$Project = (Get-Location).Path,
  [string]$LoaderBin = '',           # default: searched under esp-rack/loader (see below)
  [ValidateSet('app', 'loader')]
  [string]$StartIn = 'app',          # what boots first: the app (otadata -> ota_0) or the loader
  [int]$Baud = 921600,
  [switch]$EraseAll,                 # erase the whole flash first (settings and files too)
  [switch]$LoaderOnly,               # write only the loader into factory
  [switch]$Force,                    # skip the project-name and bootloader flash-setting checks
  [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$chips = @{
  'esp32' = @{ Esptool = 'esp32';   Short = 'esp32'; Id = 0;  Boot = 0x1000 }
  'c3'    = @{ Esptool = 'esp32c3'; Short = 'c3';    Id = 5;  Boot = 0x0 }
  'c6'    = @{ Esptool = 'esp32c6'; Short = 'c6';    Id = 13; Boot = 0x0 }
  's3'    = @{ Esptool = 'esp32s3'; Short = 's3';    Id = 9;  Boot = 0x0 }
}
$c = $chips[($Chip -replace '^esp32(?=c|s)', '')]
if (-not $PioEnv) { $PioEnv = "loader-$($c.Short)" }

$espRack = Split-Path -Parent $PSScriptRoot
$pio = Join-Path $env:USERPROFILE '.platformio'
$python = Join-Path $pio 'penv\Scripts\python.exe'
$esptool = Join-Path $pio 'packages\tool-esptoolpy\esptool.py'
$bootApp0 = Join-Path $pio 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
$buildDir = Join-Path $Project ".pio\build\$PioEnv"

function Fail([string]$msg) { Write-Host "flash_loader: $msg" -ForegroundColor Red; exit 1 }
function Hex([long]$v) { '0x{0:x}' -f $v }
function Need([string]$path, [string]$what) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "$what not found: $path" }
  (Resolve-Path -LiteralPath $path).Path
}

# An ESP app image: chip id (u16 at 12) and esp_app_desc_t's project name
# (magic 0xABCD5432 at 32, project_name at 80).
function Read-Image([string]$path) {
  $b = [System.IO.File]::ReadAllBytes($path)
  if ($b.Length -lt 112 -or $b[0] -ne 0xE9) { return @{ Ok = $false; Size = $b.Length } }
  $project = ''
  if ([BitConverter]::ToUInt32($b, 32) -eq 0xABCD5432L) {  # L: a 32-bit hex literal is signed in PS 5.1
    $project = [System.Text.Encoding]::ASCII.GetString($b, 80, 32).Split([char]0)[0]
  }
  @{ Ok = $true; Size = $b.Length; ChipId = [BitConverter]::ToUInt16($b, 12); Project = $project }
}

# The loader build next to a loader image: its sdkconfig and bootloader.bin
# (build.ps1: <dir>\sdkconfig, <dir>\bootloader\bootloader.bin; idf.py:
# the project's loader\sdkconfig, build*\bootloader\bootloader.bin;
# PlatformIO: loader\sdkconfig.<env>, .pio\build\<env>\bootloader.bin).
# Only an sdkconfig for this chip counts (idf.py's is shared by its build
# dirs). Boot: the boot button's pin, level and hold time, or $null.
function Read-LoaderBuild([string]$loaderPath) {
  $dir = Split-Path -Parent $loaderPath
  $envName = Split-Path -Leaf $dir
  $confs = @((Join-Path $dir 'sdkconfig'), (Join-Path $espRack "loader\sdkconfig.$envName"))
  if ($envName -like 'build*') { $confs += Join-Path (Split-Path -Parent $dir) 'sdkconfig' }  # idf.py's default
  $target = "CONFIG_IDF_TARGET=`"$($c.Esptool)`""
  $conf = $confs | Where-Object { (Test-Path -LiteralPath $_ -PathType Leaf) -and
    @(Get-Content -LiteralPath $_) -contains $target } | Select-Object -First 1
  $boot = @((Join-Path $dir 'bootloader\bootloader.bin'), (Join-Path $dir 'bootloader.bin')) |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
  $info = @{ Conf = $conf; Bootloader = $boot; Boot = $null; Mode = '?' }
  if (-not $conf) { return $info }
  $lines = Get-Content -LiteralPath $conf
  $info.Mode = if ($lines -contains 'CONFIG_ESPTOOLPY_FLASHMODE_QIO=y') { 'qio' } else { 'dio' }
  if ($lines -contains 'CONFIG_BOOTLOADER_FACTORY_RESET=y') {
    $val = { param($k) (($lines | Select-String "^$k=(.*)$").Matches | Select-Object -First 1).Groups[1].Value }
    $info.Boot = @{
      Pin = & $val 'CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET'
      Level = if ($lines -contains 'CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH=y') { 'high' } else { 'low' }
      Hold = & $val 'CONFIG_BOOTLOADER_HOLD_TIME_GPIO'
    }
  }
  $info
}

# A bootloader's flash settings from its image header: mode (byte 2), and
# size / frequency (byte 3: high / low nibble).
function Read-FlashHeader([string]$path) {
  $b = [System.IO.File]::ReadAllBytes($path)
  if ($b.Length -lt 16 -or $b[0] -ne 0xE9) { return $null }
  $modes = @('qio', 'qout', 'dio', 'dout')
  @{ Mode = $b[2]; Freq = $b[3] -band 0x0F; Size = $b[3] -shr 4; ChipId = [BitConverter]::ToUInt16($b, 12)
    Text = ('{0}, frequency code {1:x}' -f $(if ($b[2] -lt 4) { $modes[$b[2]] } else { $b[2] }), ($b[3] -band 0x0F)) }
}

# partitions.bin: 32-byte entries (magic AA 50, type, subtype, offset, size, label).
function Read-Partitions([string]$path) {
  $b = [System.IO.File]::ReadAllBytes($path)
  $list = @()
  for ($o = 0; $o + 32 -le $b.Length; $o += 32) {
    if ($b[$o] -ne 0xAA -or $b[$o + 1] -ne 0x50) { break }
    $list += [pscustomobject]@{
      Type = $b[$o + 2]; Subtype = $b[$o + 3]
      Offset = [BitConverter]::ToUInt32($b, $o + 4); Size = [BitConverter]::ToUInt32($b, $o + 8)
      Label = [System.Text.Encoding]::ASCII.GetString($b, $o + 12, 16).Split([char]0)[0]
    }
  }
  $list
}

foreach ($p in @($python, $esptool)) { Need $p 'PlatformIO tool' | Out-Null }
if (-not (Test-Path -LiteralPath $buildDir)) { Fail "no build of env '$PioEnv' in $Project (pio run -e $PioEnv first)" }
$partBin = Need (Join-Path $buildDir 'partitions.bin') "the env's partition table"
$parts = Read-Partitions $partBin
$factory = $parts | Where-Object { $_.Type -eq 0 -and $_.Subtype -eq 0x00 } | Select-Object -First 1
$ota0 = $parts | Where-Object { $_.Type -eq 0 -and $_.Subtype -eq 0x10 } | Select-Object -First 1
$ota1 = $parts | Where-Object { $_.Type -eq 0 -and $_.Subtype -eq 0x11 } | Select-Object -First 1
$otadata = $parts | Where-Object { $_.Type -eq 1 -and $_.Subtype -eq 0x00 } | Select-Object -First 1
if (-not $factory -or -not $ota0 -or $ota1 -or -not $otadata) {
  Fail "env '$PioEnv' has no loader table (needs factory + ota_0 + otadata, no ota_1): use esp-rack/partitions/loader_*.csv"
}

# The loader for this chip: -LoaderBin, else the newest loader build under
# esp-rack/loader (idf.py: build*/esprack-loader.bin, PlatformIO:
# .pio/build/*/firmware.bin) whose image is for this chip.
$searched = @((Join-Path $espRack 'loader\build*\esprack-loader.bin'), (Join-Path $espRack 'loader\.pio\build\*\firmware.bin'),
  (Join-Path $env:TEMP 'esprack-loader\*\esprack-loader.bin'))  # build.ps1's default
if ($LoaderBin) {
  $loaderCandidates = @($LoaderBin)
} else {
  $loaderCandidates = @($searched | ForEach-Object { Get-ChildItem -Path $_ -File -ErrorAction SilentlyContinue } |
      Sort-Object LastWriteTime -Descending | ForEach-Object { $_.FullName })
}
$loader = $null
foreach ($cand in $loaderCandidates) {
  if (-not (Test-Path -LiteralPath $cand -PathType Leaf)) { Fail "$cand not found" }
  $img = Read-Image $cand
  if ($img.Ok -and $img.ChipId -eq $c.Id -and ($LoaderBin -or $img.Project -eq 'esprack-loader')) {
    $loader = (Resolve-Path -LiteralPath $cand).Path; $loaderImg = $img; break
  }
  if ($LoaderBin) { Fail "$cand is not an image for $($c.Esptool)" }
}
if (-not $loader) { Fail ("no loader built for $($c.Esptool) (build loader/ for it, or -LoaderBin); looked at:`n  " + ($searched -join "`n  ")) }
if (-not $Force -and $loaderImg.Project -ne 'esprack-loader') {
  Fail "$loader is not the OTA loader (project '$($loaderImg.Project)'); -Force writes it anyway"
}
if ($loaderImg.Size -gt $factory.Size) {
  Fail "the loader ($($loaderImg.Size) B) does not fit factory ($($factory.Size) B)"
}

# The bootloader: the app env's, or the loader build's when it has a boot
# button; that one's flash settings must be the app bootloader's.
$loaderBuild = Read-LoaderBuild $loader
if (-not $loaderBuild.Conf -and $loaderBuild.Bootloader) {
  Write-Warning ("no $($c.Esptool) sdkconfig found for $loader`: whether its bootloader has a boot button is " +
    "unknown, the app env's is written (build.ps1 keeps the sdkconfig next to the image)")
}
$appBoot = Join-Path $buildDir 'bootloader.bin'
$bootNote = "the app env's"
$bootloader = $null
if ($loaderBuild.Boot) {
  $lb = $loaderBuild.Boot
  if (-not $loaderBuild.Bootloader) { Fail "the loader has a boot button (GPIO $($lb.Pin)) but its build has no bootloader.bin: rebuild it (build.ps1)" }
  $bootloader = (Resolve-Path -LiteralPath $loaderBuild.Bootloader).Path
  $mine = Read-FlashHeader $bootloader
  if (-not $mine -or $mine.ChipId -ne $c.Id) { Fail "$bootloader is not a bootloader for $($c.Esptool)" }
  if (Test-Path -LiteralPath $appBoot -PathType Leaf) {
    $theirs = Read-FlashHeader $appBoot
    if (-not $Force -and (-not $theirs -or $theirs.Mode -ne $mine.Mode -or $theirs.Freq -ne $mine.Freq)) {
      Fail ("the loader's bootloader has flash $($mine.Text), the app's $($theirs.Text): build the loader with the " +
        "board's settings (build.ps1 -FlashMode / -FlashFreq), or -Force")
    }
  } elseif (-not $Force) {
    Fail "no app bootloader to compare the loader's with ($appBoot): pio run -e $PioEnv first, or -Force"
  }
  $bootNote = "the loader's: GPIO $($lb.Pin) held $($lb.Level) for $($lb.Hold) s at reset boots the loader; " +
    "flash $($mine.Text), built $($loaderBuild.Mode)"
}

$writes = @()
if ($LoaderOnly) {
  if ($bootloader) { $writes += , @((Hex $c.Boot), $bootloader) }
  $writes += , @((Hex $factory.Offset), $loader)
} else {
  if (-not $bootloader) { $bootloader = Need $appBoot "the env's bootloader" }
  $app = Need (Join-Path $buildDir 'firmware.bin') "the env's app"
  $appImg = Read-Image $app
  if (-not $appImg.Ok -or $appImg.ChipId -ne $c.Id) { Fail "$app is not an app for $($c.Esptool)" }
  if (-not $Force -and $appImg.Project -eq 'esprack-loader') { Fail "$app is the loader, not the app" }
  if ($appImg.Size -gt $ota0.Size) { Fail "the app ($($appImg.Size) B) does not fit ota_0 ($($ota0.Size) B)" }
  if ($StartIn -eq 'app') {
    $otaImage = Need $bootApp0 'boot_app0.bin (otadata selecting ota_0)'
  } else {
    # blank otadata: the bootloader starts factory, the loader
    $otaImage = Join-Path ([System.IO.Path]::GetTempPath()) 'esprack_otadata_blank.bin'
    $blank = New-Object byte[] $otadata.Size
    for ($i = 0; $i -lt $blank.Length; $i++) { $blank[$i] = 0xFF }
    [System.IO.File]::WriteAllBytes($otaImage, $blank)
  }
  $writes += , @((Hex $c.Boot), $bootloader)
  $writes += , @((Hex 0x8000), $partBin)
  $writes += , @((Hex $otadata.Offset), $otaImage)
  $writes += , @((Hex $factory.Offset), $loader)
  $writes += , @((Hex $ota0.Offset), $app)
}

$cmd = @($esptool, '--chip', $c.Esptool)
if ($Port) { $cmd += @('--port', $Port) }
$cmd += @('--baud', "$Baud", '--before', 'default-reset', '--after', 'hard-reset',
  'write-flash', '-z', '--flash-mode', 'keep', '--flash-freq', 'keep', '--flash-size', 'detect')
if ($EraseAll) { $cmd += '--erase-all' }
foreach ($w in $writes) { $cmd += @($w[0], $w[1]) }

Write-Host ("chip {0}, env {1}: loader {2} B in factory {3}" -f $c.Esptool, $PioEnv, $loaderImg.Size, (Hex $factory.Offset))
if ($bootloader) { Write-Host "bootloader: $bootNote" }
if (-not $LoaderOnly) {
  Write-Host ("app {0} B in ota_0 {1} ({2:N0} % of {3} B), first boot: {4}" -f $appImg.Size, (Hex $ota0.Offset),
    (100.0 * $appImg.Size / $ota0.Size), $ota0.Size, $StartIn)
}
foreach ($w in $writes) { Write-Host ("  {0,-9} {1}" -f $w[0], $w[1]) }
if ($DryRun) {
  Write-Host ("dry run: `"$python`" " + (($cmd | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '))
  exit 0
}
& $python @cmd
if ($LASTEXITCODE -ne 0) { Fail "esptool failed ($LASTEXITCODE)" }
Write-Host 'flash_loader: done' -ForegroundColor Green
