#!/bin/sh
# Costruisce folletto-shell_*.deb dal prefix compilato + overlay di sessione.
#
# Uso:
#   ./scripts/build.sh          # (o build mutter+shell nel prefix)
#   ./scripts/build-deb.sh
#
# Variabili:
#   FOLLETTO_PREFIX   prefix runtime (default: /var/tmp/folletto/prefix)
#   FOLLETTO_VERSION  versione pacchetto Debian (default: 0.1)
#   FOLLETTO_DEB_OUT  directory output (default: <repo>/dist)
#   FOLLETTO_STUB=1   crea un prefix minimo per smoke-test del packaging
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PREFIX="${FOLLETTO_PREFIX:-/var/tmp/folletto/prefix}"
VERSION="${FOLLETTO_VERSION:-0.1}"
OUTDIR="${FOLLETTO_DEB_OUT:-$REPO_ROOT/dist}"
ARCH=$(dpkg --print-architecture)
PKG_NAME=folletto-shell
OPT_PREFIX=/opt/folletto

die() {
  echo "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Comando richiesto non trovato: $1"
}

need_cmd dpkg-deb
need_cmd dpkg

make_stub_prefix() {
  stub=$1
  echo "FOLLETTO_STUB=1: creo prefix minimo in $stub" >&2
  mkdir -p \
    "$stub/bin" \
    "$stub/lib/mutter-16/plugins" \
    "$stub/lib/gnome-shell" \
    "$stub/share/glib-2.0/schemas" \
    "$stub/share/gnome-control-center/keybindings"
  printf '#!/bin/sh\necho "folletto stub gnome-shell $*"\n' > "$stub/bin/gnome-shell"
  printf '#!/bin/sh\necho "folletto stub mutter $*"\n' > "$stub/bin/mutter"
  chmod 755 "$stub/bin/gnome-shell" "$stub/bin/mutter"
  cp "$REPO_ROOT/overlay/mutter/data/org.folletto.wm.gschema.xml" \
    "$stub/share/glib-2.0/schemas/"
  cp "$REPO_ROOT/overlay/mutter/data/50-folletto.xml" \
    "$stub/share/gnome-control-center/keybindings/"
  if command -v glib-compile-schemas >/dev/null 2>&1; then
    glib-compile-schemas "$stub/share/glib-2.0/schemas"
  fi
}

if [ "${FOLLETTO_STUB:-0}" = "1" ]; then
  PREFIX=$(mktemp -d /tmp/folletto-stub-prefix.XXXXXX)
  trap 'rm -rf "$PREFIX"' EXIT
  make_stub_prefix "$PREFIX"
fi

[ -d "$PREFIX" ] || die "Prefix mancante: $PREFIX (compila con ./scripts/build.sh oppure FOLLETTO_STUB=1)"
[ -x "$PREFIX/bin/gnome-shell" ] || die "Manca $PREFIX/bin/gnome-shell"
[ -x "$PREFIX/bin/mutter" ] || die "Manca $PREFIX/bin/mutter"

STAGE=$(mktemp -d /tmp/folletto-deb-stage.XXXXXX)
cleanup() {
  rm -rf "$STAGE"
  if [ "${FOLLETTO_STUB:-0}" = "1" ] && [ -n "${PREFIX:-}" ]; then
    rm -rf "$PREFIX"
  fi
}
trap cleanup EXIT

PKGDIR=$STAGE/$PKG_NAME
mkdir -p \
  "$PKGDIR/DEBIAN" \
  "$PKGDIR$OPT_PREFIX" \
  "$PKGDIR/usr/bin" \
  "$PKGDIR/usr/share/wayland-sessions" \
  "$PKGDIR/usr/share/xsessions" \
  "$PKGDIR/usr/share/gnome-session/sessions" \
  "$PKGDIR/usr/share/applications" \
  "$PKGDIR/usr/share/glib-2.0/schemas" \
  "$PKGDIR/usr/share/gnome-control-center/keybindings" \
  "$PKGDIR/usr/share/doc/$PKG_NAME" \
  "$PKGDIR/usr/lib/systemd/user" \
  "$PKGDIR/usr/lib/systemd/user/gnome-session@folletto.target.d" \
  "$PKGDIR$OPT_PREFIX/libexec"

echo "Copio runtime da $PREFIX → $OPT_PREFIX ..."
# Evita di portare cache/build junk; copia il tree installato.
tar -C "$PREFIX" -cf - . | tar -C "$PKGDIR$OPT_PREFIX" -xf -

# Riscrivi path dev nei file di testo (dbus, gjs, .desktop nel prefix).
find "$PKGDIR$OPT_PREFIX" -type f \
  ! -path '*/bin/*' ! -path '*/lib/*' ! -path '*/libexec/*' \
  -exec grep -Il '/var/tmp/folletto/prefix' {} + 2>/dev/null \
  | xargs -r sed -i "s|/var/tmp/folletto/prefix|$OPT_PREFIX|g" || true

# Schema overlay (default mode, keybind) sempre allineato al repo.
install -m 644 "$REPO_ROOT/overlay/mutter/data/org.folletto.wm.gschema.xml" \
  "$PKGDIR$OPT_PREFIX/share/glib-2.0/schemas/org.folletto.wm.gschema.xml"
install -m 644 "$REPO_ROOT/overlay/mutter/data/org.folletto.wm.gschema.xml" \
  "$PKGDIR/usr/share/glib-2.0/schemas/org.folletto.wm.gschema.xml"
if command -v glib-compile-schemas >/dev/null 2>&1; then
  glib-compile-schemas "$PKGDIR$OPT_PREFIX/share/glib-2.0/schemas"
  glib-compile-schemas "$PKGDIR/usr/share/glib-2.0/schemas"
fi

if strings "$PKGDIR$OPT_PREFIX/lib/libmutter-16.so.0" 2>/dev/null \
  | grep -q '/var/tmp/folletto/prefix'; then
  echo "NOTA: libmutter contiene ancora /var/tmp/folletto/prefix (compile-time)." >&2
  echo "      postinst crea symlink compat; per fix definitivo: ./scripts/build-for-deb.sh" >&2
fi

# Settings: installa lo script nel prefix e un wrapper in /usr/bin
install -m 755 "$REPO_ROOT/overlay/settings/folletto-settings" \
  "$PKGDIR$OPT_PREFIX/libexec/folletto-settings"
install -m 755 "$REPO_ROOT/overlay/session/folletto-settings-wrapper" \
  "$PKGDIR/usr/bin/folletto-settings"

# Session wrappers + shell launcher (lib isolate solo per gnome-shell)
install -d "$PKGDIR$OPT_PREFIX/libexec"
install -m 644 "$REPO_ROOT/overlay/session/folletto-session-lib.sh" \
  "$PKGDIR$OPT_PREFIX/libexec/folletto-session-lib.sh"
install -m 755 "$REPO_ROOT/overlay/session/folletto-gnome-shell" \
  "$PKGDIR/usr/bin/folletto-gnome-shell"
install -m 755 "$REPO_ROOT/overlay/session/folletto-session" \
  "$PKGDIR/usr/bin/folletto-session"
install -m 755 "$REPO_ROOT/overlay/session/folletto-session-x11" \
  "$PKGDIR/usr/bin/folletto-session-x11"

# Voci login manager + componente sessione (non org.gnome.Shell di sistema)
install -m 644 "$REPO_ROOT/overlay/session/folletto.desktop" \
  "$PKGDIR/usr/share/wayland-sessions/folletto.desktop"
install -m 644 "$REPO_ROOT/overlay/session/folletto-xorg.desktop" \
  "$PKGDIR/usr/share/xsessions/folletto-xorg.desktop"
install -m 644 "$REPO_ROOT/overlay/session/folletto.session" \
  "$PKGDIR/usr/share/gnome-session/sessions/folletto.session"
install -m 644 "$REPO_ROOT/overlay/session/org.folletto.Shell.desktop" \
  "$PKGDIR/usr/share/applications/org.folletto.Shell.desktop"
install -m 644 "$REPO_ROOT/overlay/session/org.folletto.Settings.desktop" \
  "$PKGDIR/usr/share/applications/org.folletto.Settings.desktop"

# Unit systemd utente (org.folletto.Shell + gnome-session@folletto)
install -m 644 "$REPO_ROOT/overlay/session/systemd/org.folletto.Shell.target" \
  "$PKGDIR/usr/lib/systemd/user/org.folletto.Shell.target"
install -m 644 "$REPO_ROOT/overlay/session/systemd/org.folletto.Shell@wayland.service" \
  "$PKGDIR/usr/lib/systemd/user/org.folletto.Shell@wayland.service"
install -m 644 "$REPO_ROOT/overlay/session/systemd/org.folletto.Shell@x11.service" \
  "$PKGDIR/usr/lib/systemd/user/org.folletto.Shell@x11.service"
install -m 644 \
  "$REPO_ROOT/overlay/session/systemd/gnome-session@folletto.target.d/folletto.session.conf" \
  "$PKGDIR/usr/lib/systemd/user/gnome-session@folletto.target.d/folletto.session.conf"

# Le unit systemd del prefix puntano al path di build (/var/tmp/...):
# riscrivi verso /opt/folletto (fallback per avvii non-packaged).
if [ -d "$PKGDIR$OPT_PREFIX/lib/systemd/user" ]; then
  find "$PKGDIR$OPT_PREFIX/lib/systemd/user" -type f \( -name '*.service' -o -name '*.target' \) \
    -exec sed -i \
      -e "s|/var/tmp/folletto/prefix|$OPT_PREFIX|g" \
      -e "s|ExecStart=$OPT_PREFIX/bin/gnome-shell|ExecStart=/usr/bin/folletto-gnome-shell|g" \
      {} +
fi
# Desktop file shell nel prefix: punta al wrapper
if [ -f "$PKGDIR$OPT_PREFIX/share/applications/org.gnome.Shell.desktop" ]; then
  sed -i 's|^Exec=.*|Exec=/usr/bin/folletto-gnome-shell|' \
    "$PKGDIR$OPT_PREFIX/share/applications/org.gnome.Shell.desktop"
fi

# Schema e keybinding anche in /usr così Impostazioni/GNOME li vedono
install -m 644 "$REPO_ROOT/overlay/mutter/data/org.folletto.wm.gschema.xml" \
  "$PKGDIR/usr/share/glib-2.0/schemas/org.folletto.wm.gschema.xml"
install -m 644 "$REPO_ROOT/overlay/mutter/data/50-folletto.xml" \
  "$PKGDIR/usr/share/gnome-control-center/keybindings/50-folletto.xml"

# Docs
install -m 644 "$REPO_ROOT/README.md" "$PKGDIR/usr/share/doc/$PKG_NAME/README.md"
install -m 644 "$REPO_ROOT/packaging/copyright" "$PKGDIR/usr/share/doc/$PKG_NAME/copyright"
{
  echo "$PKG_NAME ($VERSION) unstable; urgency=medium"
  echo
  echo "  * Package FollettoShell runtime, settings and login sessions."
  echo
  echo " -- FollettoShell maintainers <folletto@localhost>  $(date -R)"
} > "$PKGDIR/usr/share/doc/$PKG_NAME/changelog.Debian"
gzip -9n -f "$PKGDIR/usr/share/doc/$PKG_NAME/changelog.Debian"

# Script maintainer
install -m 755 "$REPO_ROOT/packaging/DEBIAN/postinst" "$PKGDIR/DEBIAN/postinst"
install -m 755 "$REPO_ROOT/packaging/DEBIAN/prerm" "$PKGDIR/DEBIAN/prerm"
install -m 755 "$REPO_ROOT/packaging/DEBIAN/postrm" "$PKGDIR/DEBIAN/postrm"

# Dimensione installata in KiB (campo Debian Installed-Size)
SIZE=$(du -sk "$PKGDIR" | awk '{print $1}')
if [ -z "$SIZE" ] || [ "$SIZE" -lt 1 ]; then
  SIZE=1
fi

sed \
  -e "s/@VERSION@/$VERSION/g" \
  -e "s/@ARCH@/$ARCH/g" \
  -e "s/@SIZE@/$SIZE/g" \
  "$REPO_ROOT/packaging/DEBIAN/control.in" > "$PKGDIR/DEBIAN/control"

# md5sums (opzionale ma utile)
(
  cd "$PKGDIR"
  find . -type f ! -path './DEBIAN/*' -print0 | sort -z | xargs -0 md5sum | sed 's|  \./|  |' 
) > "$PKGDIR/DEBIAN/md5sums"

mkdir -p "$OUTDIR"
DEB_PATH="$OUTDIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"

echo "Costruisco $DEB_PATH ..."
if command -v fakeroot >/dev/null 2>&1; then
  fakeroot dpkg-deb --build --root-owner-group "$PKGDIR" "$DEB_PATH"
else
  # Ambienti senza fakeroot: dpkg-deb --root-owner-group evita uid locali
  dpkg-deb --build --root-owner-group "$PKGDIR" "$DEB_PATH"
fi

echo "OK: $DEB_PATH"
dpkg-deb --info "$DEB_PATH"
echo "--- Contenuto (sessioni / wrapper) ---"
dpkg-deb --contents "$DEB_PATH" | grep -E 'usr/(bin|share/(wayland-sessions|xsessions|gnome-session|applications))|opt/folletto/(bin|libexec)/' || true
echo
echo "Installazione: sudo apt install ./$(basename "$DEB_PATH")"
echo "Poi al login scegli «Folletto» (Wayland) o «Folletto on Xorg»."
