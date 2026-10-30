# Builds the loader for one chip with the ESP-IDF 5.4.2 that PlatformIO
# installed (framework-espidf, its Python env and toolchains): the same
# CMake build `pio run` drives, without PlatformIO's SCons layer. Use it
# where `pio run -e <chip>` fails (PlatformIO Core 6.2 wants tool-scons
# 4.11.1, the platform 4.8.1: each run reinstalls it under the running
# build, "No module named 'SCons.Tool.FortranCommon'").
#
#   powershell -File loader/build.ps1 -Target esp32c3 [-BuildDir <dir>] [-Clean]
#       [-BootPin <gpio> [-BootLevel low|high] [-BootHoldS <s>]]
#       [-FlashMode qio|dio] [-FlashFreq 40m|80m]
#
# -BootPin: the boot button (CMakeLists.txt, LOADER_BOOT_PIN): this
# project's bootloader then boots the loader while the pin is held at
# reset; tools/flash_loader.ps1 writes that bootloader instead of the app's.
# -BootHoldS 1..30: the bootloader's watchdog is set to it + 4 s (the IDF's
# 9 s would reset the chip during a hold of 9 s or more).
# -FlashMode / -FlashFreq: only for a board whose app bootloader differs
# from sdkconfig.defaults.<target> (flash_loader.ps1 compares them).
#
# Output: <BuildDir>/esprack-loader.bin, checked against the 4 MB loader
# table's factory partition (the smallest), and <BuildDir>/bootloader/
# bootloader.bin. sdkconfig is made anew from the defaults on every run.
param(
  [ValidateSet("esp32", "esp32c3", "esp32c6", "esp32s3")][string]$Target = "esp32c3",
  [string]$BuildDir = "",
  [switch]$Clean,
  [int]$BootPin = -1,
  [ValidateSet("low", "high")][string]$BootLevel = "low",
  [ValidateRange(1, 30)][int]$BootHoldS = 5,
  [ValidateSet("", "qio", "dio")][string]$FlashMode = "",
  [ValidateSet("", "40m", "80m")][string]$FlashFreq = ""
)
$ErrorActionPreference = "Stop"
$pio = Join-Path $env:USERPROFILE ".platformio"
$proj = $PSScriptRoot
$table = Join-Path (Split-Path -Parent $proj) "partitions\loader_4MB.csv"
$b = if ($BuildDir) { $BuildDir } else { Join-Path $env:TEMP "esprack-loader\$Target" }

# The boot pin: a GPIO of the chip, not one of its flash pins. A strapping
# pin (the boards' BOOT button) works when pressed just after the reset,
# not during it (held during the reset it starts the ROM's download mode).
$gpio = @{
  esp32   = @{ Max = 39; Flash = 6..11;  Strap = 0, 2, 5, 12, 15 }
  esp32c3 = @{ Max = 21; Flash = 12..17; Strap = 2, 8, 9 }
  esp32c6 = @{ Max = 30; Flash = 24..30; Strap = 8, 9, 15 }
  esp32s3 = @{ Max = 48; Flash = 26..32; Strap = 0, 3, 45, 46 }
}[$Target]
if ($BootPin -ge 0) {
  $missing = @{ esp32 = 20, 24, 28, 29, 30, 31; esp32s3 = 22, 23, 24, 25 }[$Target]
  if ($BootPin -gt $gpio.Max -or $gpio.Flash -contains $BootPin -or $missing -contains $BootPin) {
    throw "GPIO $BootPin is no usable input on $Target (flash pins: $($gpio.Flash -join ','))"
  }
  if ($gpio.Strap -contains $BootPin) {
    Write-Warning "GPIO $BootPin is a strapping pin on ${Target}: press it after the reset, not during it"
  }
  if ($Target -eq "esp32" -and $BootPin -ge 34) {
    Write-Warning "GPIO $BootPin has no internal pull-up: the button needs an external one"
  }
}

$idf = Get-ChildItem (Join-Path $pio "packages") -Directory -Filter "framework-espidf*" |
  Where-Object { (Get-Content (Join-Path $_.FullName "package.json") -Raw) -match '"version":\s*"3\.50402' } |
  Select-Object -First 1
if (-not $idf) { throw "framework-espidf 5.4.2 not installed (pio run once, or pio pkg install)" }
$env:IDF_PATH = $idf.FullName
$env:IDF_PYTHON_ENV_PATH = Join-Path $pio "penv\.espidf-5.4.2"
$env:ESP_ROM_ELF_DIR = Join-Path $pio "packages\tool-esp-rom-elfs\"
$py = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
$tools = "toolchain-riscv32-esp\bin", "toolchain-xtensa-esp-elf\bin", "tool-cmake\bin", "tool-ninja"
$env:PATH = (($tools | ForEach-Object { Join-Path $pio "packages\$_" }) -join ";") + ";$env:PATH"

if ($Clean -and (Test-Path $b)) { Remove-Item -Recurse -Force $b }
New-Item -ItemType Directory -Force $b | Out-Null
# The defaults are the whole configuration: a kept sdkconfig would keep an
# earlier run's choices.
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $b "sdkconfig")
# The partition table and flash size the build checks the image against.
$extra = Join-Path $b "partitions.defaults"
"CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=`"$($table -replace '\\', '/')`"`nCONFIG_ESPTOOLPY_FLASHSIZE_4MB=y" |
  Set-Content -Encoding ascii $extra
cmake -S $proj -B $b -G Ninja "-DIDF_TARGET=$Target" "-DSDKCONFIG=$b\sdkconfig" `
  "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults;$extra" "-DPYTHON=$py" -DPYTHON_DEPS_CHECKED=1 -DCCACHE_ENABLE=0 `
  "-DLOADER_BOOT_PIN=$BootPin" "-DLOADER_BOOT_LEVEL=$BootLevel" "-DLOADER_BOOT_HOLD_S=$BootHoldS" `
  "-DLOADER_FLASH_MODE=$FlashMode" "-DLOADER_FLASH_FREQ=$FlashFreq"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
ninja -C $b esprack-loader.elf
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# elf2image as the IDF build has it, with PlatformIO's esptool (its Python
# env has esptool's modules; IDF's env from PlatformIO doesn't).
function Elf2Image([string]$ninjaFile, [string]$elf) {
  $pattern = "--chip \S+ elf2image [^&]*$([regex]::Escape($elf))"
  $line = Select-String -Path $ninjaFile -Pattern $pattern | Select-Object -First 1
  if (-not $line) { throw "no elf2image step for $elf in $ninjaFile" }
  $esptoolArgs = ([regex]::Match($line.Line, $pattern).Value -split " ") | Where-Object { $_ }
  & (Join-Path $pio "penv\Scripts\python.exe") (Join-Path $pio "packages\tool-esptoolpy\esptool.py") @esptoolArgs
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
Elf2Image (Join-Path $b "build.ninja") "esprack-loader.elf"

# The bootloader: its sub-build stops at its own elf2image (IDF's Python
# env), so the image is made here from bootloader.elf as above.
ninja -C $b bootloader | Out-Null
$bootDir = Join-Path $b "bootloader"
if (-not (Test-Path (Join-Path $bootDir "bootloader.elf"))) {
  ninja -C $b bootloader
  throw "the bootloader did not build"
}
Elf2Image (Join-Path $bootDir "build.ninja") "bootloader.elf"

$size = (Get-Item (Join-Path $b "esprack-loader.bin")).Length
$factory = 0xC0000
"esprack-loader $Target`: $size B ($([math]::Round(100 * $size / $factory, 1)) % of the 768 KB factory partition)"
if ($size -gt $factory) { Write-Error "the loader does not fit the factory partition"; exit 1 }
$boot = (Get-Item (Join-Path $bootDir "bootloader.bin")).Length
$bootRoom = if ($Target -eq "esp32") { 0x7000 } else { 0x8000 }  # up to the partition table at 0x8000
$conf = Get-Content (Join-Path $b "sdkconfig")
$mode = if ($conf -contains "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y") { "qio" } else { "dio" }
$freq = ($conf | Select-String '^CONFIG_ESPTOOLPY_FLASHFREQ="(.*)"').Matches[0].Groups[1].Value
$pin = if ($conf -contains "CONFIG_BOOTLOADER_FACTORY_RESET=y") {
  $wdt = ($conf | Select-String '^CONFIG_BOOTLOADER_WDT_TIME_MS=(\d+)').Matches[0].Groups[1].Value
  if ([int]$wdt -lt ($BootHoldS + 4) * 1000) { Write-Error "bootloader watchdog $wdt ms: too short for a $BootHoldS s hold"; exit 1 }
  "boot button GPIO $BootPin held $BootLevel for $BootHoldS s (watchdog $wdt ms)"
} else { "no boot button" }
"bootloader: $boot B (room $bootRoom B), flash $mode $freq, $pin"
if ($boot -gt $bootRoom) { Write-Error "the bootloader does not fit before the partition table"; exit 1 }
