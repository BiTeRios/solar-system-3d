# Все автотесты проекта. Код выхода ≠ 0 при провале.
#   .\tools\run_tests.ps1            — обычный набор (около 5 минут); оконные тесты рендера открывают окна Godot
#   .\tools\run_tests.ps1 -Full      — плюс долгие: сохранение за 100 лет, таблица «Симуляции» за 10 лет (около 25 минут)
#   .\tools\run_tests.ps1 -NoWindow  — без оконных тестов (машина без экрана)
#   .\tools\run_tests.ps1 -Exported  — собрать игру (tools\export.ps1) и прогнать оконные тесты внутри SolarSystem.exe
# Перед запуском: .\tools\build_data.ps1 (данные игры) и .\tools\build_native.ps1 (C++).
param([switch]$Full, [switch]$NoWindow, [switch]$Exported)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"

if ($Exported) {
    & (Join-Path $PSScriptRoot "export.ps1")
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    Write-Host "== Оконные тесты внутри собранной игры (build\release\SolarSystem.exe)"
    $env:SPACE_EXE = Join-Path $root "build\release\SolarSystem.exe"
    Push-Location $root
    try {
        & $py -I -X utf8 -m pytest pipeline\tests -q -m "window and not slow"
        exit $LASTEXITCODE
    } finally { Pop-Location; Remove-Item Env:SPACE_EXE }
}

Write-Host "== C++ (doctest)"
& (Join-Path $root "native\bin\spacecore_tests.exe")
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "== Python и связка с C++/Godot (pytest)"
Push-Location $root
try {
    $sel = if ($Full) { "slow or not slow" } else { "not slow" }
    if ($NoWindow) { $sel = "($sel) and not window" }
    & $py -I -X utf8 -m pytest pipeline\tests -q -m $sel
    exit $LASTEXITCODE
} finally { Pop-Location }
