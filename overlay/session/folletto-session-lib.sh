# Ambiente e prep comuni alle sessioni Folletto (Wayland / X11).
# Sourced da folletto-session e folletto-session-x11 — non eseguire direttamente.

folletto_setup_prefix() {
  PREFIX="${FOLLETTO_PREFIX:-/opt/folletto}"
  if [ ! -x "$PREFIX/bin/gnome-shell" ]; then
    echo "Folletto non trovato in $PREFIX (manca gnome-shell)." >&2
    exit 1
  fi
  if [ ! -x /usr/bin/folletto-gnome-shell ] && [ ! -x "$PREFIX/bin/folletto-gnome-shell" ]; then
    echo "Manca folletto-gnome-shell (wrapper). Reinstalla il pacchetto folletto-shell." >&2
    exit 1
  fi

  # Solo path/dati di sessione: NON impostare LD_LIBRARY_PATH qui.
  # Le lib Folletto restano nel wrapper folletto-gnome-shell, altrimenti
  # gnome-settings-daemon e GDM vedono librerie incompatibili → nero / freeze.
  export PATH="$PREFIX/bin:/usr/bin:${PATH:-/usr/bin}"
  export XDG_DATA_DIRS="$PREFIX/share:/usr/share:/usr/local/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
  export XDG_CURRENT_DESKTOP="${XDG_CURRENT_DESKTOP:-GNOME}"
  # Schema Folletto anche per gsettings della sessione (senza forzare tutto il prefix)
  if [ -d "$PREFIX/share/glib-2.0/schemas" ]; then
    export GSETTINGS_SCHEMA_DIR="$PREFIX/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}"
  fi

  log_dir="${XDG_STATE_HOME:-$HOME/.local/state}/folletto"
  mkdir -p "$log_dir"
  export FOLLETTO_SESSION_LOG="$log_dir/session-$(date +%Y%m%d-%H%M%S).log"
  {
    echo "=== folletto session $(date -Is) ==="
    echo "PREFIX=$PREFIX"
    echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-}"
    echo "DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
    echo "PATH=$PATH"
    echo "XDG_DATA_DIRS=$XDG_DATA_DIRS"
  } >>"$FOLLETTO_SESSION_LOG" 2>&1
}

# Al primo avvio Folletto disabilita le estensioni utente (conflitti con tiling).
folletto_disable_extensions_on_first_run() {
  config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/folletto"
  marker="$config_dir/extensions-first-run-done"
  if [ -f "$marker" ]; then
    return 0
  fi
  mkdir -p "$config_dir"
  if command -v gsettings >/dev/null 2>&1; then
    gsettings set org.gnome.shell disable-user-extensions true 2>/dev/null || true
  fi
  touch "$marker"
}
