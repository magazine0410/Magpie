# Compilation guide

The Linux version is in the directory `linux` of the repository and is built with CMake. It uses the effects (`src/Effects`), the texts and the icon of the Windows version from the same repository, so the whole repository is needed.

## Requirements

A C++23 compiler (the port is developed with the GCC of Fedora 44), CMake 3.25 or newer, Ninja, and the development files of: fmt, spdlog, RapidJSON, muParser, libpng, fontconfig, Vulkan (headers and loader), Wayland (client library, wayland-protocols, wlr-protocols, plasma-wayland-protocols), PipeWire, systemd (sd-bus), libxkbcommon, Qt 6.5 or newer (Base, Declarative, Tools with Linguist), Python 3, and Catch2 version 3 for the tests.

On Fedora:

```bash
sudo dnf install git gcc-c++ cmake ninja-build fmt-devel spdlog-devel rapidjson-devel catch-devel vulkan-headers vulkan-loader-devel muParser-devel libpng-devel wayland-devel wayland-protocols-devel wlr-protocols-devel plasma-wayland-protocols-devel pipewire-devel systemd-devel fontconfig-devel libxkbcommon-devel qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel qt6-linguist qt6-qtsvg python3
```

Three things are downloaded when CMake configures the build, each a pinned version checked against its SHA-256:

- the DirectX Shader Compiler (a binary release from Microsoft, x86-64 only), which compiles the effects. No distribution packages it. `-DDXC_ROOT=DIR` uses an installation of your own instead;
- Dear ImGui, for the toolbar (`-DIMGUI_ROOT=DIR`);
- the Fluent UI System Icons font, for the toolbar's icons (`-DMAGPIE_ICON_FONT=FILE`).

`-DMAGPIE_FETCH_DXC=OFF`, `-DMAGPIE_FETCH_IMGUI=OFF` and `-DMAGPIE_FETCH_ICON_FONT=OFF` forbid the downloads.

## Building

```bash
cmake -S linux -B linux/build -G Ninja
```

```bash
cmake --build linux/build
```

The settings application is `linux/build/app/magpie`; the other programs are below `linux/build/tools`. `-DMAGPIE_BUILD_APP=OFF` builds without Qt and without the application.

A program in the build tree uses the effects of the source tree. For the global shortcuts on KDE Plasma the desktop has to know the program: copy `linux/build/app/io.github.magazine0410.Magpie.desktop` to `~/.local/share/applications/`.

## Tests

```bash
ctest --test-dir linux/build -LE slow
```

`-LE slow` leaves out the one test that compiles every effect in six combinations (about a minute). The renderer tests run on every graphics card of the machine and on the software renderer. Some tests need more than a compiler and skip themselves without it:

- the capture tests: a PipeWire session;
- the tests of Sway and of the nested session: the program `sway`. They start a Sway that shows nothing;
- `kwin-virtual`: the program `kwin_wayland` and a PipeWire session. It starts a KWin that shows nothing.

None of these touch the desktop they are started from. The scripts `linux/tests/tools/*_desktop_test.sh` do: they scale test windows on a KDE Plasma desktop and move its pointer, and are run by hand.

## Installing

```bash
cmake -S linux -B linux/build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr
```

```bash
cmake --build linux/build
```

```bash
sudo cmake --install linux/build
```

This installs the programs, the effects and the font (`share/magpie`), the library of the shader compiler (`lib64/magpie`), the `.desktop` file, the icon and the AppStream description.

## The RPM

```bash
linux/packaging/build-rpm.sh
```

builds `magpie-upscaler` from the commit that is checked out, in a new directory below `/tmp`, with the three downloaded files taken from `linux/build/_deps`. The package is not called `magpie` because Fedora has a package of that name.

`linux/packaging/container-check.sh` builds the package in a Fedora container, runs the tests there on the software renderer, and installs the package into a plain Fedora container to try the installed programs. It needs podman.

## Layout

| Directory | Contents |
|---|---|
| `linux/core` | settings, `config.json`, profiles, paths, logging |
| `linux/fx` | the effect compiler: MagpieFX to SPIR-V |
| `linux/render` | the Vulkan renderer, the toolbar, the cursor |
| `linux/bridge` | access to the desktop: KWin, Sway, the portal, the nested session |
| `linux/engine` | capture, the surface of the picture, the scaling session, the service |
| `linux/app` | the settings application |
| `linux/tools` | the command-line programs |
| `linux/tests` | tests and the scripts for desktop checks |
| `linux/packaging` | the RPM |
