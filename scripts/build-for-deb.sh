#!/bin/sh
# Compila Mutter (+ shell se presente) con --prefix=/opt/folletto per il .deb.
# I path in libmutter (es. mutter-x11-frames) devono coincidere con l'installazione.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

if [ -z "${FOLLETTO_IN_CHROOT:-}" ] && [ -x /var/tmp/folletto/debian/usr/bin/meson ]; then
  exec "$SCRIPT_DIR/with-debian.sh" "$SCRIPT_DIR/build-for-deb.sh" "$@"
fi

ROOT=/var/tmp/folletto
OPT_PREFIX=/opt/folletto
STAGE="$ROOT/prefix-deb"
BUILD="$ROOT/build/mutter-deb"
SRC="$ROOT/src/mutter"
JOBS=${NINJA_JOBS:-$(nproc)}

MESON=meson
if ! command -v meson >/dev/null; then
  if [ -x /var/tmp/folletto/tools/meson-1.7.0/meson.py ]; then
    MESON="python3 /var/tmp/folletto/tools/meson-1.7.0/meson.py"
  else
    echo "meson non trovato." >&2
    exit 1
  fi
fi

[ -f "$SRC/meson.build" ] || {
  echo "Sorgenti mancanti in $SRC (sync-upstream + overlay)." >&2
  exit 1
}

rm -rf "$STAGE"
mkdir -p "$STAGE" "$BUILD"

if [ ! -f "$BUILD/build.ninja" ]; then
  $MESON setup "$BUILD" "$SRC" \
    --prefix="$OPT_PREFIX" \
    --libdir=lib \
    -Dintrospection=true \
    -Dtests=disabled \
    -Ddocs=false \
    -Dprofiler=false \
    -Dudev_dir="$OPT_PREFIX/lib/udev"
else
  $MESON setup --reconfigure "$BUILD" "$SRC" \
    --prefix="$OPT_PREFIX" \
    --libdir=lib \
    -Dintrospection=true \
    -Dtests=disabled \
    -Ddocs=false \
    -Dprofiler=false \
    -Dudev_dir="$OPT_PREFIX/lib/udev"
fi

$MESON compile -C "$BUILD" -j "$JOBS"
DESTDIR="$STAGE" $MESON install -C "$BUILD"

# Compila anche gnome-shell nel prefix /opt/folletto (stesso flusso di build.sh se presente).
if [ -f "$ROOT/src/gnome-shell/meson.build" ]; then
  SHELL_BUILD="$ROOT/build/gnome-shell-deb"
  SHELL_SRC="$ROOT/src/gnome-shell"
  if [ ! -f "$SHELL_BUILD/build.ninja" ]; then
    $MESON setup "$SHELL_BUILD" "$SHELL_SRC" \
      --prefix="$OPT_PREFIX" \
      --libdir=lib \
      -Dtests=false
  else
    $MESON setup --reconfigure "$SHELL_BUILD" "$SHELL_SRC" \
      --prefix="$OPT_PREFIX" \
      --libdir=lib \
      -Dtests=false
  fi
  $MESON compile -C "$SHELL_BUILD" -j "$JOBS"
  DESTDIR="$STAGE" $MESON install -C "$SHELL_BUILD"
fi

if command -v glib-compile-schemas >/dev/null 2>&1; then
  glib-compile-schemas "$STAGE$OPT_PREFIX/share/glib-2.0/schemas"
fi

PREFIX="$STAGE$OPT_PREFIX"
export FOLLETTO_PREFIX="$PREFIX"
echo "Prefix pacchetto pronto: $PREFIX"
echo "Esegui: FOLLETTO_PREFIX=$PREFIX $REPO_ROOT/scripts/build-deb.sh"