#!/bin/sh
# Esercita layout i3 e lo spostamento mouse nella nested Folletto.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
if [ -z "${FOLLETTO_IN_CHROOT:-}" ] && [ -x /var/tmp/folletto/debian/usr/bin/meson ]; then
  exec "$SCRIPT_DIR/with-debian.sh" "$SCRIPT_DIR/test-nested-tiling.sh" "$@"
fi
# shellcheck source=folletto-common.sh
. "$SCRIPT_DIR/folletto-common.sh"

folletto_export_prefix
runtime=$(folletto_runtime_dir)
busfile="$runtime/folletto-bus-address"
sock="$runtime/$FOLLETTO_WAYLAND_SOCKET"

if [ ! -S "$sock" ]; then
  echo "Nested Folletto non in esecuzione. Avvia ./scripts/run-shell-nested.sh" >&2
  exit 1
fi

i=0
while [ "$i" -lt 50 ]; do
  if [ -s "$busfile" ]; then
    break
  fi
  i=$((i + 1))
  sleep 0.1
done
BUS=$(cat "$busfile")
if [ -z "$BUS" ]; then
  echo "Nessun indirizzo D-Bus nested." >&2
  exit 1
fi

call() {
  gdbus call --address "$BUS" --dest org.folletto.Wm \
    --object-path /org/folletto/Wm --method "$@"
}

i=0
while [ "$i" -lt 40 ]; do
  if call org.folletto.Wm.GetMode >/dev/null 2>&1; then
    break
  fi
  i=$((i + 1))
  sleep 0.25
done

GSETTINGS_SCHEMA_DIR="$PREFIX/share/glib-2.0/schemas"
export GSETTINGS_SCHEMA_DIR
gsettings set org.folletto.wm show-decorations false
gsettings set org.folletto.wm mode i3
call org.folletto.Wm.SetMode i3 >/dev/null

HOST_DISPLAY="${DISPLAY:-:0}"
HOST_XAUTHORITY="${XAUTHORITY:-}"
auth=$(ls -1 "$runtime"/.mutter-Xwaylandauth.* 2>/dev/null | head -n 1 || true)
xdpy=":1"
for n in 1 2 3 4 5 11 12; do
  if [ -S "/tmp/.X11-unix/X$n" ]; then
    xdpy=":$n"
    break
  fi
done
export WAYLAND_DISPLAY="$FOLLETTO_WAYLAND_SOCKET"
export DBUS_SESSION_BUS_ADDRESS="$BUS"
if [ -n "$auth" ]; then
  export XAUTHORITY="$auth"
fi
export DISPLAY="$xdpy"

launch() {
  title=$1
  shift
  WAYLAND_DISPLAY="$FOLLETTO_WAYLAND_SOCKET" DISPLAY="$xdpy" \
    DBUS_SESSION_BUS_ADDRESS="$BUS" "$@" >/tmp/folletto-app-$$.log 2>&1 &
  echo "Avviato $title pid $!"
}

launch calc gnome-calculator || launch calc gnome-calculator
sleep 0.6
launch editor gnome-text-editor || true
sleep 0.6
if command -v xterm >/dev/null; then
  launch xterm xterm -T folletto-xterm
elif command -v xclock >/dev/null; then
  launch xclock xclock
fi
sleep 1.2

echo "Layout splith"
call org.folletto.Wm.Command 'layout splith'
sleep 0.8
echo "Layout splitv"
call org.folletto.Wm.Command 'layout splitv'
sleep 0.8
echo "Layout splith (drag)"
call org.folletto.Wm.Command 'layout splith'
sleep 0.8

DISPLAY="$HOST_DISPLAY" XAUTHORITY="${HOST_XAUTHORITY}" WAYLAND_DISPLAY= python3 - <<'PY'
import time
from Xlib import X, display
from Xlib.ext.xtest import fake_input

d = display.Display()
root = d.screen().root
found = []

def walk(w):
    try:
        wm = w.get_wm_class()
        name = w.get_wm_name() or ""
    except Exception:
        wm, name = None, ""
    cls = " ".join(wm).lower() if wm else ""
    if "gnome-shell" in cls or "mutter" in cls or "GNOME Shell" in name:
        geom = w.get_geometry()
        if geom.width >= 800 and geom.height >= 500:
            found.append((w, geom))
    try:
        for c in w.query_tree().children:
            walk(c)
    except Exception:
        pass

walk(root)
if not found:
    print("Finestra nested non trovata (skip drag visivo).")
    raise SystemExit(0)
win, geom = found[-1]
abs_x, abs_y = geom.x, geom.y
parent = win.query_tree().parent
try:
    t = win.translate_coords(root, 0, 0)
    abs_x, abs_y = t.x, t.y
except Exception:
    pass
x1 = abs_x + geom.width // 4
x2 = abs_x + 3 * geom.width // 4
y = abs_y + min(220, geom.height // 2)
print(f"Drag mouse nested {win.id} {x1},{y} -> {x2},{y}")
win.set_input_focus(X.RevertToParent, X.CurrentTime)
d.sync()
fake_input(d, X.MotionNotify, x=x1, y=y)
d.sync()
time.sleep(0.15)
fake_input(d, X.ButtonPress, 1)
d.sync()
time.sleep(0.1)
fake_input(d, X.MotionNotify, x=x2, y=y)
d.sync()
time.sleep(0.35)
fake_input(d, X.ButtonRelease, 1)
d.sync()
time.sleep(0.4)
print("Drag completato")
PY

sleep 0.8
echo "Layout tabbed"
call org.folletto.Wm.Command 'layout tabbed'
sleep 1.2
echo "Layout stacked"
call org.folletto.Wm.Command 'layout stacked'
sleep 1.2
echo "Layout splith"
call org.folletto.Wm.Command 'layout splith'
sleep 0.5

echo "Mode/layout: $(call org.folletto.Wm.GetMode) $(call org.folletto.Wm.GetLayout)"
echo "Test tiling nested completato."
