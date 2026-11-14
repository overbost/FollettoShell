#!/bin/sh
# Chroot Debian 13 per compilare ed eseguire Folletto sull'immagine Ubuntu
# dell'agente. Su una macchina Debian nativa gli altri script non passano di qui.
set -eu

CHROOT=/var/tmp/folletto/debian
REPO_HOST=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

if [ "$(id -u)" -ne 0 ]; then
  exec sudo -- "$0" "$@"
fi

mount_one() {
  src=$1
  dst=$2
  mkdir -p "$dst"
  if ! mountpoint -q "$dst"; then
    mount --bind "$src" "$dst"
  fi
}

ensure_mounts() {
  mount_one /proc "$CHROOT/proc"
  mount_one /sys "$CHROOT/sys"
  mount_one /dev "$CHROOT/dev"
  mount_one /dev/pts "$CHROOT/dev/pts"
  mount_one /tmp "$CHROOT/tmp"
  mount_one /run "$CHROOT/run"
  mkdir -p /run/user/1000
  chown 1000:1000 /run/user/1000
  chmod 700 /run/user/1000
  mkdir -p "$CHROOT/var/tmp" "$CHROOT/home/ubuntu" "$CHROOT$REPO_HOST"
  mount_one /var/tmp/folletto "$CHROOT/var/tmp/folletto"
  mount_one "$REPO_HOST" "$CHROOT$REPO_HOST"
  if [ -d /home/ubuntu ]; then
    mount_one /home/ubuntu "$CHROOT/home/ubuntu"
  fi
}

ensure_chroot() {
  if [ ! -x "$CHROOT/usr/bin/apt-get" ]; then
    mkdir -p "$CHROOT"
    debootstrap --variant=minbase trixie "$CHROOT" http://deb.debian.org/debian
  fi
  if ! grep -q '^ubuntu:' "$CHROOT/etc/passwd"; then
    echo 'ubuntu:x:1000:1000:Ubuntu:/home/ubuntu:/bin/sh' >> "$CHROOT/etc/passwd"
    echo 'ubuntu:x:1000:' >> "$CHROOT/etc/group"
  fi
  cat > "$CHROOT/etc/apt/sources.list" <<'EOF'
deb http://deb.debian.org/debian trixie main
deb-src http://deb.debian.org/debian trixie main
EOF
  ensure_mounts
  DEBIAN_FRONTEND=noninteractive chroot "$CHROOT" apt-get update
  DEBIAN_FRONTEND=noninteractive chroot "$CHROOT" apt-get install -y \
    build-essential meson ninja-build pkg-config gettext git ca-certificates \
    dbus libglib2.0-bin python3 python3-xlib mesa-utils libgl1-mesa-dri \
    foot fonts-dejavu-core
  DEBIAN_FRONTEND=noninteractive chroot "$CHROOT" apt-get build-dep -y mutter gnome-shell
}

if [ "${1:-}" = "--ensure" ]; then
  ensure_chroot
  exit 0
fi

if [ ! -x "$CHROOT/usr/bin/meson" ]; then
  echo "Chroot Debian assente. Esegui ./scripts/install-build-deps.sh" >&2
  exit 1
fi

ensure_mounts

exec chroot --userspec=1000:1000 "$CHROOT" /usr/bin/env \
  FOLLETTO_IN_CHROOT=1 \
  FOLLETTO_REPO="$REPO_HOST" \
  HOME=/home/ubuntu \
  LANG=C.UTF-8 \
  LC_ALL=C.UTF-8 \
  USER="${USER:-ubuntu}" \
  LOGNAME="${USER:-ubuntu}" \
  DISPLAY="${DISPLAY:-:1}" \
  XAUTHORITY="${XAUTHORITY:-/home/ubuntu/.Xauthority}" \
  XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}" \
  LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}" \
  GALLIUM_DRIVER="${GALLIUM_DRIVER:-llvmpipe}" \
  NINJA_JOBS="${NINJA_JOBS:-2}" \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  /bin/sh -c 'cd "$FOLLETTO_REPO" && exec "$@"' sh "$@"
