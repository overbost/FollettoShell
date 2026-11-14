#!/bin/sh
# Bootstrap idempotente: chroot Debian 13, sorgenti Mutter 48.7, prefix Folletto.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

"$SCRIPT_DIR/with-debian.sh" --ensure
"$SCRIPT_DIR/sync-upstream.sh"
"$SCRIPT_DIR/with-debian.sh" "$SCRIPT_DIR/build.sh"
"$SCRIPT_DIR/with-debian.sh" glib-compile-schemas /var/tmp/folletto/prefix/share/glib-2.0/schemas
echo "Prefix pronto: /var/tmp/folletto/prefix"
