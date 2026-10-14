# Сборка C++: GDExtension (game/bin) и тесты (native/bin). Компилятор — WinLibs GCC 16.2 в C:\Tools\winlibs.
param([string]$Target = "template_debug", [switch]$NoTests)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = "C:\Tools\winlibs\mingw64\bin;" + $env:PATH
$scons = Join-Path $root ".venv\Scripts\scons.exe"
$testsArg = if ($NoTests) { "tests=no" } else { "tests=yes" }
Push-Location (Join-Path $root "native")
try {
    & $scons platform=windows target=$Target use_mingw=yes $testsArg -j12
    exit $LASTEXITCODE
} finally { Pop-Location }
