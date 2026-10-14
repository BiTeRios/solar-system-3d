#!/usr/bin/env bash
# Загрузка разрешённых ядер NAIF в data/raw/naif/ (с докачкой). Список согласован 11.10.2026
# (DE442 и nep098_part-1 — для Урана и Нептуна «как в Horizons», комментарии *.cmt — модель динамики).
set -u
BASE="https://naif.jpl.nasa.gov/pub/naif/generic_kernels"
OUT="$(dirname "$0")/../data/raw/naif"
mkdir -p "$OUT"
FILES="
lsk/naif0012.tls
pck/pck00011.tpc
pck/gm_de440.tpc
spk/planets/aa_checksums.txt
spk/satellites/aa_checksums.txt
spk/planets/de440.bsp
spk/satellites/nep097.bsp
spk/satellites/plu060.bsp
spk/satellites/ura184_part-3.bsp
spk/satellites/sat441.bsp
spk/satellites/jup365.bsp
spk/satellites/mar099.bsp
spk/planets/de442.bsp
spk/satellites/nep098_part-1.bsp
spk/satellites/jup365.cmt
spk/satellites/sat441.cmt
spk/satellites/mar099.cmt
spk/satellites/ura184_part-3.cmt
spk/satellites/nep097.cmt
spk/satellites/plu060.cmt
spk/satellites/nep098_part-1.cmt
"
for f in $FILES; do
  name="$(echo "$f" | tr '/' '_')"
  case "$f" in *aa_checksums.txt) ;; *) name="$(basename "$f")";; esac
  echo "[$(date +%H:%M:%S)] $f -> $name"
  curl -fsS -C - --retry 5 --retry-delay 10 -o "$OUT/$name" "$BASE/$f" || echo "ОШИБКА: $f"
done
echo "[$(date +%H:%M:%S)] готово"
ls -la "$OUT"
