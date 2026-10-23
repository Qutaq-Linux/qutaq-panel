# qutaq-panel

## Overview

`qutaq-panel` is top panel with global menu for Qutaq Desktop

## Build

To build the program, be sure to install the libraries

**Arch Linux / Manjaro**

```sh
sudo pacman -S --needed base-devel git cmake qt6-base qt6-wayland qt6-tools layer-shell-qt wayland
```

**Debian / Ubuntu**

```sh
sudo apt install build-essential git cmake pkg-config libwayland-dev \
  qt6-base-dev qt6-base-private-dev qt6-wayland-dev qt6-wayland-private-dev \
  qt6-wayland-dev-tools qt6-tools-dev qt6-l10n-tools liblayershellqtinterface-dev
```

**Fedora**

```sh
sudo dnf install gcc-c++ git cmake wayland-devel \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtwayland-devel qt6-qttools-devel qt6-linguist \
  layer-shell-qt-devel
```

Building:
```sh
git clone https://github.com/Qutaq-Linux/qutaq-panel.git
cd qutaq-panel
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Using

Add **first line** to `~/.config/labwc/autostart`:
```sh
QT_FORCE_STDERR_LOGGING=1 ~/.local/bin/qutaq-panel >/tmp/qutaq-panel.log 2>&1 &
sleep 1
```
Add to `~/.config/labwc/environment`:

```sh
GTK_MODULES=appmenu-gtk-module
```

Add to `~/.config/labwc/rc.xml`:
```
  <theme>
    <maximizedDecoration>none</maximizedDecoration>
  </theme>
```

## Languages

The panel speaks English, Russian, Ukrainian and German. It follows the system language (`LANG`);
to choose another one, add to `~/.config/qutaq-panel/panel.conf`:

```ini
[panel]
language=de
```

Translations need Qt Linguist tools at build time (`qt6-tools` / `qt6-tools-dev qt6-l10n-tools` /
`qt6-qttools-devel qt6-linguist`, already listed above). Without them the panel still builds, in English only.

Want to add your language or fix a translation? See [TRANSLATING.md](TRANSLATING.md).
