# Folletto

Fork di Mutter 48.7 + GNOME Shell 48.7 (Debian 13) con tiling i3 nativo
e due modalità commutabili in sessione: **normal** (GNOME 48) e **i3**.

I sorgenti e la build stanno in `/var/tmp/folletto` (disco di root, 114 GB liberi).
La home è piena: non si clona Mutter lì. I pacchetti Debian `mutter` / `gnome-shell`
non vengono sostituiti.

## Build (isolata, niente `/usr`)

```sh
# 1. dipendenze di compilazione (chiede sudo; NON sostituisce GNOME)
./scripts/install-build-deps.sh

# 2. meson/ninja in prefix /var/tmp/folletto/prefix
./scripts/build.sh

# 3. prova nested (finestra, non sostituisce la sessione)
./scripts/run-nested.sh
```

Toggle in nested mutter: `Super+Shift+t`. In GNOME Shell Folletto: Quick Settings «i3 layout».

## Comandi i3 (C, in Mutter)

`Meta.Display.i3_command()` accetta la sintassi i3:

- `focus|move left|right|up|down`
- `split h|v|toggle`
- `layout splith|splitv|tabbed|stacked|toggle`
- `floating toggle`
- `fullscreen`
- `kill`
- `workspace number N` / `move workspace number N`
- `resize grow|shrink width|height N ppt`
- `gaps inner|outer N`
- `move scratchpad` / `scratchpad show`
- `mode resize|default`
- `toggle_mode`

Keybind di default (solo in modalità i3, eccetto toggle): come i3 con Super.

## Sicurezza host

- prefix: `/var/tmp/folletto/prefix`
- sorgenti: `/var/tmp/folletto/src/{mutter,gnome-shell}`
- mai `meson install` in `/usr`
