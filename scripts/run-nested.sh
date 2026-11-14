#!/bin/sh
# Mutter Folletto nested, senza GNOME Shell. Non tocca la sessione Debian.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
# shellcheck source=folletto-common.sh
. "$SCRIPT_DIR/folletto-common.sh"

if [ ! -x "$PREFIX/bin/mutter" ]; then
  echo "Compila prima: ./scripts/build.sh" >&2
  exit 1
fi

folletto_export_prefix
folletto_prepare_wayland_socket
folletto_bind_host_display
export MUTTER_DEBUG_DUMMY_MODE_SPECS="${MUTTER_DEBUG_DUMMY_MODE_SPECS:-$FOLLETTO_NESTED_SIZE}"

echo "Nested Mutter Folletto. Super+Shift+t = toggle i3/normal."
FOLLETTO_SCRIPT_DIR=$SCRIPT_DIR
export FOLLETTO_SCRIPT_DIR
folletto_record_nested_pid
folletto_exec_session_bus mutter --wayland --nested --wayland-display="$FOLLETTO_WAYLAND_SOCKET" "$@"
