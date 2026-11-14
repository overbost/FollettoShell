# Ambiente e helper condivisi. Sourciare, non eseguire.
# ROOT/prefix isolati: non toccano i pacchetti GNOME Debian.
# Installazione da .deb: FOLLETTO_PREFIX=/opt/folletto

ROOT=/var/tmp/folletto
PREFIX="${FOLLETTO_PREFIX:-$ROOT/prefix}"
FOLLETTO_WAYLAND_SOCKET="${FOLLETTO_WAYLAND_SOCKET:-wayland-folletto}"
FOLLETTO_NESTED_SIZE="${MUTTER_DEBUG_DUMMY_MODE_SPECS:-1280x720@60}"
FOLLETTO_X11_SIZE="${FOLLETTO_X11_SIZE:-1280x720}"

folletto_export_prefix() {
  export PATH="$PREFIX/bin:$PATH"
  export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib/mutter-16:$PREFIX/lib/gnome-shell${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export GI_TYPELIB_PATH="$PREFIX/lib/mutter-16:$PREFIX/lib/gnome-shell${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
  export GSETTINGS_SCHEMA_DIR="$PREFIX/share/glib-2.0/schemas"
  export XDG_DATA_DIRS="$PREFIX/share:${XDG_DATA_DIRS:-/usr/share:/usr/local/share}"
  export MUTTER_PLUGIN_DIR="$PREFIX/lib/mutter-16/plugins"
  export GIO_USE_VFS=local
  export XDG_CURRENT_DESKTOP="${XDG_CURRENT_DESKTOP:-GNOME}"
}

folletto_require_prefix() {
  if [ ! -x "$PREFIX/bin/gnome-shell" ]; then
    echo "Compila prima Mutter+Shell nel prefix (/var/tmp/folletto)." >&2
    exit 1
  fi
}

folletto_runtime_dir() {
  echo "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
}

folletto_nested_pidfile() {
  echo "$(folletto_runtime_dir)/folletto-nested.pid"
}

folletto_x11_pidfile() {
  echo "$(folletto_runtime_dir)/folletto-x11.pid"
}

# Solo nested Folletto: il gnome-shell Debian non ha --wayland-display=wayland-folletto.
folletto_nested_pids() {
  pgrep -f "[g]nome-shell --wayland --nested --wayland-display=${FOLLETTO_WAYLAND_SOCKET}" 2>/dev/null || true
  pgrep -f "[m]utter --wayland --nested --wayland-display=${FOLLETTO_WAYLAND_SOCKET}" 2>/dev/null || true
}

folletto_unique_pids() {
  printf '%s\n' "$@" | awk 'NF && !seen[$1]++ { print $1 }'
}

folletto_stop_pids() {
  pids=$(folletto_unique_pids "$@")
  filtered=
  for p in $pids; do
    if [ "$p" = "$$" ]; then
      continue
    fi
    filtered="$filtered $p"
  done
  pids=$(folletto_unique_pids $filtered)
  if [ -z "$pids" ]; then
    return 0
  fi
  echo "Fermo la sessione Folletto precedente: $(echo "$pids" | tr '\n' ' ')" >&2
  for p in $pids; do
    pkill -TERM -P "$p" 2>/dev/null || true
    kill -TERM "$p" 2>/dev/null || true
  done
  i=0
  while [ "$i" -lt 40 ]; do
    alive=
    for p in $pids; do
      if kill -0 "$p" 2>/dev/null; then
        alive=1
        break
      fi
    done
    if [ -z "$alive" ]; then
      return 0
    fi
    i=$((i + 1))
    sleep 0.1
  done
  for p in $pids; do
    pkill -KILL -P "$p" 2>/dev/null || true
    kill -KILL "$p" 2>/dev/null || true
  done
}

folletto_pids_from_file() {
  file=$1
  if [ ! -f "$file" ]; then
    return 0
  fi
  while read -r p; do
    case "$p" in
      ''|*[!0-9]*) ;;
      *)
        if kill -0 "$p" 2>/dev/null; then
          echo "$p"
        fi
        ;;
    esac
  done < "$file"
}

# Xwayland lasciato dalla nested (auth mutter, non :0 della sessione Debian).
folletto_reap_nested_xwayland() {
  for p in $(pgrep -f '[X]wayland' 2>/dev/null || true); do
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null || true)
    case "$cmd" in
      *.mutter-Xwaylandauth.*)
        case "$cmd" in
          *' :0 '*|*:0$|*' :0')
            continue
            ;;
        esac
        ppid=$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ')
        case "$ppid" in
          1|"")
            echo "Chiudo Xwayland nested rimasto (pid $p)." >&2
            kill -TERM "$p" 2>/dev/null || true
            ;;
        esac
        ;;
    esac
  done
}

folletto_prepare_wayland_socket() {
  runtime=$(folletto_runtime_dir)
  sock="$runtime/$FOLLETTO_WAYLAND_SOCKET"
  pidfile=$(folletto_nested_pidfile)

  old=$(folletto_pids_from_file "$pidfile")
  live=$(folletto_nested_pids)
  folletto_stop_pids $old $live
  folletto_reap_nested_xwayland

  i=0
  while [ "$i" -lt 20 ]; do
    if [ ! -e "$sock" ] && [ ! -e "$sock.lock" ]; then
      break
    fi
    rm -f "$sock" "$sock.lock"
    i=$((i + 1))
    sleep 0.1
  done
  rm -f "$sock" "$sock.lock" "$pidfile"
}

folletto_record_nested_pid() {
  echo "$$" > "$(folletto_nested_pidfile)"
}

folletto_x11_pids() {
  pgrep -f "[g]nome-shell --x11 .*--sm-disable" 2>/dev/null || true
}

folletto_prepare_x11() {
  pidfile=$(folletto_x11_pidfile)
  old=$(folletto_pids_from_file "$pidfile")
  live=$(folletto_x11_pids)
  folletto_stop_pids $old $live
  rm -f "$pidfile"
}

folletto_record_x11_pids() {
  printf '%s\n' "$@" > "$(folletto_x11_pidfile)"
}

# Nested Mutter 48 disegna la finestra sul DISPLAY X11 dell'host.
# WAYLAND_DISPLAY del processo deve restare quello dell'host, non wayland-folletto.
folletto_bind_host_display() {
  runtime=$(folletto_runtime_dir)
  case "${WAYLAND_DISPLAY:-}" in
    "$FOLLETTO_WAYLAND_SOCKET"|wayland-folletto)
      unset WAYLAND_DISPLAY
      ;;
  esac
  if [ -z "${DISPLAY:-}" ] && [ -S /tmp/.X11-unix/X0 ]; then
    export DISPLAY=:0
  fi
  if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -S "$runtime/wayland-0" ]; then
    export WAYLAND_DISPLAY=wayland-0
  fi
  if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "Né DISPLAY né WAYLAND_DISPLAY: impossibile aprire la finestra nested." >&2
    exit 1
  fi
}

folletto_free_x_display() {
  n=10
  while [ "$n" -lt 64 ]; do
    if [ ! -S "/tmp/.X11-unix/X$n" ] && [ ! -e "/tmp/.X$n-lock" ]; then
      echo "$n"
      return 0
    fi
    n=$((n + 1))
  done
  echo "Nessun display X libero (:10–:63)." >&2
  return 1
}

# Servizi minimi sul bus isolato (dbus-run-session non ha systemd né il
# portachiavi Debian). Senza secrets il portal resta in timeout ~25s.
folletto_prepare_nested_services() {
  runtime=$(folletto_runtime_dir)
  kr="$runtime/folletto-keyring"
  mkdir -p "$kr"
  if command -v gnome-keyring-daemon >/dev/null; then
    eval "$(gnome-keyring-daemon --start --components=secrets \
      --control-directory="$kr" 2>/dev/null || true)"
  fi
}

folletto_exec_session_bus() {
  if [ -z "${FOLLETTO_SCRIPT_DIR:-}" ]; then
    echo "FOLLETTO_SCRIPT_DIR non impostato." >&2
    exit 1
  fi
  exec dbus-run-session -- "$FOLLETTO_SCRIPT_DIR/folletto-bus-run.sh" "$@"
}

# Mai diventare WM della sessione Debian in cui stiamo sviluppando.
folletto_refuse_host_x11() {
  disp=${DISPLAY:-}
  case "$disp" in
    :0|:0.0)
      if [ "${XDG_SESSION_TYPE:-}" = wayland ] || pgrep -x gnome-shell >/dev/null; then
        echo "Rifiuto gnome-shell --x11 su $disp: è la sessione Debian." >&2
        echo "Usa ./scripts/run-shell-x11.sh (Xephyr) oppure una sessione GDM Folletto." >&2
        exit 1
      fi
      ;;
  esac
}
