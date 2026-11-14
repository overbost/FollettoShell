#!/bin/sh
# Clona Mutter 48.7 in /var/tmp/folletto e ci applica l'overlay Folletto.
set -eu

ROOT=/var/tmp/folletto
SRC=$ROOT/src/mutter
SHELL_SRC=$ROOT/src/gnome-shell
REPO=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PATCH=$REPO/overlay/mutter-48.7.patch
SHELL_PATCH=$REPO/overlay/gnome-shell-48.7.patch

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

if [ ! -d "$SHELL_SRC/.git" ]; then
  git clone --depth 1 --branch 48.7 https://gitlab.gnome.org/GNOME/gnome-shell.git "$SHELL_SRC"
fi
if [ -f "$SHELL_SRC/.git/info/sparse-checkout" ]; then
  git -C "$SHELL_SRC" sparse-checkout disable
fi
if git -C "$SHELL_SRC" apply --reverse --check "$SHELL_PATCH" >/dev/null 2>&1; then
  echo "Patch GNOME Shell già applicata."
else
  git -C "$SHELL_SRC" apply "$SHELL_PATCH"
fi
mkdir -p "$SHELL_SRC/js/ui/status"
cp -a "$REPO/overlay/gnome-shell/js/ui/status/folletto.js" "$SHELL_SRC/js/ui/status/folletto.js"
echo "Sorgenti Folletto pronti in $SRC e $SHELL_SRC"
