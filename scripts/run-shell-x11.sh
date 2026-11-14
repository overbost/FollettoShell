#!/bin/sh
# Sessione Folletto X11 isolata: gnome-shell --x11 in una finestra.
# Host Wayland: Xwayland rootful. Host Xorg: Xephyr senza glamor + GLX software.
# Xephyr+glamor lascia lo schermo vuoto e il cursore a X (overlay Composite).
# Non sostituisce il WM Debian (niente --replace su :0).
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
# shellcheck source=folletto-common.sh
. "$SCRIPT_DIR/folletto-common.sh"

folletto_require_prefix
folletto_export_prefix
folletto_prepare_x11

runtime=$(folletto_runtime_dir)
HOST_WAYLAND=${WAYLAND_DISPLAY:-}
case "$HOST_WAYLAND" in
  "$FOLLETTO_WAYLAND_SOCKET"|wayland-folletto)
    HOST_WAYLAND=
    ;;
esac
if [ -z "$HOST_WAYLAND" ] && [ -S "$runtime/wayland-0" ]; then
  HOST_WAYLAND=wayland-0
fi

wayland_usable() {
  [ -n "$HOST_WAYLAND" ] || return 1
  python3 - "$runtime" "$HOST_WAYLAND" <<'PY'
import os, socket, sys
runtime, name = sys.argv[1], sys.argv[2]
path = name if name.startswith("/") else os.path.join(runtime, name)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(path)
except OSError:
    sys.exit(1)
sys.exit(0)
PY
}

DISPLAY_NUM=$(folletto_free_x_display)
NESTED_DISPLAY=":$DISPLAY_NUM"
XSERVER_PID=
XSERVER_LOG=/tmp/folletto-x11-server.log
XSERVER_KIND=

start_rootful_xwayland() {
  command -v Xwayland >/dev/null || return 1
  wayland_usable || return 1
  echo "Avvio Xwayland rootful $NESTED_DISPLAY (${FOLLETTO_X11_SIZE})." >&2
  env -u DISPLAY \
    XDG_RUNTIME_DIR="$runtime" \
    WAYLAND_DISPLAY="$HOST_WAYLAND" \
    Xwayland "$NESTED_DISPLAY" \
      -geometry "$FOLLETTO_X11_SIZE" \
      -decorate \
      -noreset \
      -ac \
      >"$XSERVER_LOG" 2>&1 &
  XSERVER_PID=$!
  XSERVER_KIND=xwayland
}

start_xephyr() {
  if ! command -v Xephyr >/dev/null; then
    echo "Manca Xephyr. Installa: sudo apt install xserver-xephyr" >&2
    exit 1
  fi
  echo "Avvio Xephyr $NESTED_DISPLAY (${FOLLETTO_X11_SIZE}), GL software." >&2
  # Niente -glamor: su Xephyr l'overlay Composite di Mutter resta nero.
  Xephyr "$NESTED_DISPLAY" \
    -screen "$FOLLETTO_X11_SIZE" \
    -ac \
    -br \
    -dpms \
    +extension GLX \
    +extension RANDR \
    +extension Composite \
    +extension DAMAGE \
    +extension RENDER \
    +extension XFIXES \
    >"$XSERVER_LOG" 2>&1 &
  XSERVER_PID=$!
  XSERVER_KIND=xephyr
  export LIBGL_ALWAYS_SOFTWARE=1
  export GALLIUM_DRIVER=llvmpipe
  export CLUTTER_DRIVER=gl3
  export CLUTTER_VBLANK=none
}

if ! start_rootful_xwayland; then
  start_xephyr
fi

folletto_record_x11_pids "$XSERVER_PID" "$$"

cleanup() {
  if [ -n "${XSERVER_PID:-}" ]; then
    kill "$XSERVER_PID" 2>/dev/null || true
    wait "$XSERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 80 ]; do
  if [ -S "/tmp/.X11-unix/X$DISPLAY_NUM" ]; then
    break
  fi
  if ! kill -0 "$XSERVER_PID" 2>/dev/null; then
    echo "Il server X ($XSERVER_KIND) è uscito subito. Log:" >&2
    cat "$XSERVER_LOG" >&2 || true
    exit 1
  fi
  i=$((i + 1))
  sleep 0.1
done

if [ ! -S "/tmp/.X11-unix/X$DISPLAY_NUM" ]; then
  echo "Il server X non ha creato $NESTED_DISPLAY. Log:" >&2
  cat "$XSERVER_LOG" >&2 || true
  exit 1
fi

export DISPLAY="$NESTED_DISPLAY"
unset WAYLAND_DISPLAY || true
export XDG_SESSION_TYPE=x11
export GDK_BACKEND=x11
export CLUTTER_BACKEND=x11

echo "Folletto X11 ($XSERVER_KIND $NESTED_DISPLAY, ${FOLLETTO_X11_SIZE}). Super+Shift+t: toggle i3."
FOLLETTO_SCRIPT_DIR=$SCRIPT_DIR
export FOLLETTO_SCRIPT_DIR
dbus-run-session -- "$SCRIPT_DIR/folletto-bus-run.sh" gnome-shell --x11 --display "$NESTED_DISPLAY" --sm-disable "$@"
status=$?
exit "$status"
