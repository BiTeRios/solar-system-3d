# Сборка игры для Windows в build\release: GDExtension (release), импорт ресурсов, экспорт пресета «Windows Desktop».
#   .\tools\export.ps1          — SolarSystem.exe, SolarSystem.pck и библиотека spacecore (консольной обёртки нет:
#                                 в установке Godot нет её шаблонов; вывод сценариев перехватывает pytest)
# Перед сборкой: .\tools\build_data.ps1 (данные игры).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$godot = "C:\Tools\Godot\Godot_v4.7.2-stable_win64_console.exe"
$out = Join-Path $root "build\release"

& (Join-Path $PSScriptRoot "build_native.ps1") -Target template_release -NoTests
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# список расширений — заранее: иначе Godot 4.7.2 падает при выходе из первого импорта (docs/architecture.md)
New-Item -ItemType Directory -Force (Join-Path $root "game\.godot") | Out-Null
Set-Content -Encoding ascii (Join-Path $root "game\.godot\extension_list.cfg") "res://spacecore.gdextension"
& $godot --headless --path (Join-Path $root "game") --import
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
& $godot --headless --path (Join-Path $root "game") --export-release "Windows Desktop" (Join-Path $out "SolarSystem.exe")
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-ChildItem $out | ForEach-Object { "{0,-40} {1,10:N1} МБ" -f $_.Name, ($_.Length / 1MB) }
