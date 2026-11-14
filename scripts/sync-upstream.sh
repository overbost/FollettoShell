#!/bin/sh
# Clona Mutter 48.7 in /var/tmp/folletto e ci applica l'overlay Folletto.
set -eu

ROOT=/var/tmp/folletto
SRC=$ROOT/src/mutter
REPO=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PATCH=$REPO/overlay/mutter-48.7.patch

mkdir -p "$ROOT/src"
if [ ! -f "$SRC/meson.build" ]; then
  git clone --depth 1 --branch 48.7 https://gitlab.gnome.org/GNOME/mutter.git "$SRC"
fi

if git -C "$SRC" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  echo "Patch Mutter già applicata."
else
  git -C "$SRC" apply "$PATCH"
fi

cp -a "$REPO/overlay/mutter/src/core/folletto-wm.c" "$SRC/src/core/folletto-wm.c"
cp -a "$REPO/overlay/mutter/src/core/folletto-wm.h" "$SRC/src/core/folletto-wm.h"
cp -a "$REPO/overlay/mutter/data/50-folletto.xml" "$SRC/data/50-folletto.xml"
cp -a "$REPO/overlay/mutter/data/org.folletto.wm.gschema.xml" "$SRC/data/org.folletto.wm.gschema.xml"
echo "Sorgenti Folletto pronti in $SRC"
