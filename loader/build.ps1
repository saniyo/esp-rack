# Builds the loader for one chip with the ESP-IDF 5.4.2 that PlatformIO
# installed (framework-espidf, its Python env and toolchains): the same
# CMake build `pio run` drives, without PlatformIO's SCons layer. Use it
# where `pio run -e <chip>` fails (PlatformIO Core 6.2 wants tool-scons
# 4.11.1, the platform 4.8.1: each run reinstalls it under the running
# build, "No module named 'SCons.Tool.FortranCommon'").
#
#   powershell -File loader/build.ps1 -Target esp32c3 [-BuildDir <dir>] [-Clean]
#
# Output: <BuildDir>/esprack-loader.bin, checked against the 4 MB loader
# table's factory partition (the smallest).
param(
  [ValidateSet("esp32", "esp32c3", "esp32c6", "esp32s3")][string]$Target = "esp32c3",
  [string]$BuildDir = "",
  [switch]$Clean
)
$ErrorActionPreference = "Stop"
$pio = Join-Path $env:USERPROFILE ".platformio"
$proj = $PSScriptRoot
$table = Join-Path (Split-Path -Parent $proj) "partitions\loader_4MB.csv"
$b = if ($BuildDir) { $BuildDir } else { Join-Path $env:TEMP "esprack-loader\$Target" }

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
# The partition table and flash size the build checks the image against.
$extra = Join-Path $b "partitions.defaults"
"CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=`"$($table -replace '\\', '/')`"`nCONFIG_ESPTOOLPY_FLASHSIZE_4MB=y" |
  Set-Content -Encoding ascii $extra
cmake -S $proj -B $b -G Ninja "-DIDF_TARGET=$Target" "-DSDKCONFIG=$b\sdkconfig" `
  "-DSDKCONFIG_DEFAULTS=$proj\sdkconfig.defaults;$extra" "-DPYTHON=$py" -DPYTHON_DEPS_CHECKED=1 -DCCACHE_ENABLE=0
if ($LASTEXITCODE) { exit $LASTEXITCODE }
ninja -C $b esprack-loader.elf
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# elf2image as the IDF build has it, with PlatformIO's esptool (its Python
# env has esptool's modules; IDF's env from PlatformIO doesn't).
$line = Select-String -Path (Join-Path $b "build.ninja") -Pattern "--chip \S+ elf2image [^&]*esprack-loader\.elf" |
  Select-Object -First 1
$esptoolArgs = ([regex]::Match($line.Line, "--chip \S+ elf2image [^&]*esprack-loader\.elf").Value -split " ") | Where-Object { $_ }
& (Join-Path $pio "penv\Scripts\python.exe") (Join-Path $pio "packages\tool-esptoolpy\esptool.py") @esptoolArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$size = (Get-Item (Join-Path $b "esprack-loader.bin")).Length
$factory = 0xC0000
"esprack-loader $Target`: $size B ($([math]::Round(100 * $size / $factory, 1)) % of the 768 KB factory partition)"
if ($size -gt $factory) { Write-Error "the loader does not fit the factory partition"; exit 1 }
