# Пересборка данных игры из data/raw/ (ядра, страницы, карты и рельеф уже скачаны, см. docs/sources.md).
# Карты Solar System Scope берутся из справочного проекта D:\work\Vanya\space\godot\assets\textures.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"
Push-Location $root
try {
    & $py -I -X utf8 pipeline\export_eph.py --sat-range 1900-01-01 2100-01-01; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $py -I -X utf8 pipeline\timeconv.py; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $py -I -X utf8 pipeline\export_constants.py; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $py -I -X utf8 pipeline\build_bodies.py; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $py -I -X utf8 pipeline\build_textures.py; if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $py -I -X utf8 pipeline\build_stars.py; if ($LASTEXITCODE) { exit $LASTEXITCODE }
} finally { Pop-Location }
