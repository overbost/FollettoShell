#!/bin/sh
# Compila Mutter (e opzionalmente GNOME Shell) in /var/tmp/folletto/prefix
set -eu
ROOT=/var/tmp/folletto
export GIO_USE_VFS=local
export GVFS_DISABLE_FUSE=1
SRC=$ROOT/src/mutter
BUILD=$ROOT/build/mutter
PREFIX=$ROOT/prefix
JOBS=$(nproc)

MESON=meson
if ! command -v meson >/dev/null; then
  if [ -x /var/tmp/folletto/tools/meson-1.7.0/meson.py ]; then
    MESON="python3 /var/tmp/folletto/tools/meson-1.7.0/meson.py"
  else
    echo "meson non trovato. Esegui prima: ./scripts/install-build-deps.sh" >&2
    exit 1
  fi
fi

if [ ! -f "$SRC/meson.build" ]; then
  echo "Sorgenti mancanti in $SRC" >&2
  exit 1
fi

mkdir -p "$BUILD" "$PREFIX"

if [ ! -f "$BUILD/build.ninja" ]; then
  $MESON setup "$BUILD" "$SRC" \
    --prefix="$PREFIX" \
    --libdir=lib \
    -Dintrospection=true \
    -Dtests=disabled \
    -Ddocs=false \
    -Dprofiler=false
fi

$MESON compile -C "$BUILD" -j "$JOBS"
$MESON install -C "$BUILD"
echo "Installato in $PREFIX"
echo "Prova: ./scripts/run-nested.sh"
