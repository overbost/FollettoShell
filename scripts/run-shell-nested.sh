#!/bin/sh
# GNOME Shell Folletto nested (compositor Wayland in una finestra).
# Non sostituisce GNOME Debian. Socket: wayland-folletto.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
if [ -z "${FOLLETTO_IN_CHROOT:-}" ] && [ -x /var/tmp/folletto/debian/usr/bin/meson ]; then
  exec "$SCRIPT_DIR/with-debian.sh" "$SCRIPT_DIR/run-shell-nested.sh" "$@"
fi
# shellcheck source=folletto-common.sh
. "$SCRIPT_DIR/folletto-common.sh"

folletto_require_prefix
folletto_export_prefix
folletto_prepare_wayland_socket
folletto_bind_host_display

export MUTTER_DEBUG_DUMMY_MODE_SPECS="${MUTTER_DEBUG_DUMMY_MODE_SPECS:-$FOLLETTO_NESTED_SIZE}"

echo "Nested Folletto Shell ${FOLLETTO_NESTED_SIZE%@*} (Wayland). Top bar: layout. Super+Shift+t: toggle i3."
FOLLETTO_SCRIPT_DIR=$SCRIPT_DIR
export FOLLETTO_SCRIPT_DIR
folletto_record_nested_pid
folletto_exec_session_bus gnome-shell --wayland --nested --wayland-display="$FOLLETTO_WAYLAND_SOCKET" "$@"
