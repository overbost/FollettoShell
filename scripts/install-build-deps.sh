#!/bin/sh
# Installa SOLO dipendenze di compilazione. Non sostituisce mutter/gnome-shell.
set -eu

# L'immagine dell'agente è Ubuntu. Mutter 48.7 chiede le librerie di Debian 13,
# quindi le dipendenze stanno in una chroot e non toccano i pacchetti Ubuntu.
if [ -z "${FOLLETTO_IN_CHROOT:-}" ]; then
  os_id=
  if [ -r /etc/os-release ]; then
    os_id=$(. /etc/os-release && echo "$ID")
  fi
  if [ "$os_id" != "debian" ]; then
    SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
    exec "$SCRIPT_DIR/with-debian.sh" --ensure
  fi
fi

echo "Questo installa pacchetti -dev e meson. GNOME in esecuzione resta quello Debian."
sudo apt-get update
sudo apt-get install -y meson ninja-build gcc pkg-config gettext
sudo apt-get build-dep -y mutter gnome-shell
echo "OK. Avvia ./scripts/build.sh"
