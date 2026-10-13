# Build and run the host test suites with MSVC (Visual Studio 2022 / Build
# Tools) through CMake — no GCC needed. Uses cmake from PATH, else the one
# PlatformIO ships (~/.platformio/packages/tool-cmake).
#
#   powershell -File test-host/run-host-tests.ps1 [-Filter test_civil] [-Clean] [-BuildDir C:/esprack-tb]
#
# -BuildDir: MSBuild's file tracker fails past MAX_PATH (260); from a deep
# checkout pass a short build directory.
param(
  [string]$Filter = "",
  [switch]$Clean,
  [string]$BuildDir = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = if ($BuildDir) { $BuildDir } else { Join-Path $here "build" }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $env:USERPROFILE ".platformio\packages\tool-cmake\bin\cmake.exe" }
if (-not (Test-Path $cmake)) { throw "cmake not found (PATH or PlatformIO tool-cmake)" }
$ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
& $cmake -S $here -B $build -G "Visual Studio 17 2022" -A x64 | Out-Null
if ($LASTEXITCODE) { throw "configure failed" }
& $cmake --build $build --config Release -- /nologo /verbosity:minimal
if ($LASTEXITCODE) { throw "build failed" }
$args = @("--test-dir", $build, "-C", "Release", "--output-on-failure")
if ($Filter) { $args += @("-R", $Filter) }
& $ctest @args
exit $LASTEXITCODE
