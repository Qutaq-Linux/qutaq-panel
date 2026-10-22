# qutaq-panel

## Overview

`qutaq-panel` is top panel with global menu for Qutaq Desktop

## Build

To build the program, be sure to install the libraries

**Arch Linux / Manjaro**

```sh
sudo pacman -S --needed base-devel git cmake qt6-base qt6-wayland layer-shell-qt wayland
```

**Debian / Ubuntu**

```sh
sudo apt install build-essential git cmake pkg-config libwayland-dev \
  qt6-base-dev qt6-base-private-dev qt6-wayland-dev qt6-wayland-private-dev \
  qt6-wayland-dev-tools liblayershellqtinterface-dev
```

**Fedora**

```sh
sudo dnf install gcc-c++ git cmake wayland-devel \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtwayland-devel layer-shell-qt-devel
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
