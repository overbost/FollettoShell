#!/bin/sh
# Installa SOLO dipendenze di compilazione. Non sostituisce mutter/gnome-shell.
set -eu
echo "Questo installa pacchetti -dev e meson. GNOME in esecuzione resta quello Debian."
sudo apt-get update
sudo apt-get install -y meson ninja-build gcc pkg-config gettext
sudo apt-get build-dep -y mutter gnome-shell
echo "OK. Avvia ./scripts/build.sh"
