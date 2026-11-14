#!/bin/sh
# Eseguito dentro dbus-run-session: avvia i servizi isolati e poi il comando.
set -eu
if [ -z "${FOLLETTO_IN_CHROOT:-}" ] && [ -x /var/tmp/folletto/debian/usr/bin/meson ]; then
  FOLLETTO_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
  exec "$FOLLETTO_SCRIPT_DIR/with-debian.sh" "$FOLLETTO_SCRIPT_DIR/folletto-bus-run.sh" "$@"
fi
FOLLETTO_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
# shellcheck source=folletto-common.sh
. "$FOLLETTO_SCRIPT_DIR/folletto-common.sh"
folletto_prepare_nested_services
runtime=$(folletto_runtime_dir)
printf '%s\n' "${DBUS_SESSION_BUS_ADDRESS:-}" > "$runtime/folletto-bus-address"
exec "$@"
