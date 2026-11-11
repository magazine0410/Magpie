# Magpie for Linux. Built from a git archive by build-rpm.sh, which also
# gives the four macros below; a build without them takes these defaults.
# The count is the number of commits since the release named by the base,
# which puts two packages of the same day in order.
%{!?magpie_base:%global magpie_base 0.13.0}
%{!?magpie_date:%global magpie_date 20261111}
%{!?magpie_count:%global magpie_count 0}
%{!?magpie_commit:%global magpie_commit 0000000}

%global app_id io.github.magazine0410.Magpie
# The pinned versions of linux/cmake/Dxc.cmake and ImGui.cmake
%global dxc_version v1.9.2609
%global dxc_archive linux_dxc_2026_09_28.x86_x64.tar.gz
%global imgui_version 1.91.8
%global icon_font_commit 9eb184e96268d3293462f9d9d19225b4c7b07774

# The source of the Linux version is in a subdirectory
%global _vpath_srcdir linux

# The library of the shader compiler is Magpie's own copy in a directory of
# its own; it is neither offered to other packages nor looked for in one
%global __provides_exclude_from ^%{_libdir}/magpie/.*$
%global __requires_exclude ^libdxcompiler\\.so.*$

Name:           magpie-upscaler
Version:        %{magpie_base}^%{magpie_date}.%{magpie_count}.git%{magpie_commit}
Release:        1%{?dist}
Summary:        Scale windows with a choice of scaling algorithms

# Magpie is GPL-3.0. Built in: Dear ImGui (MIT). Shipped with it: the
# DirectX Shader Compiler (NCSA, with parts under MIT) and the Fluent UI
# System Icons font (MIT).
License:        GPL-3.0-only AND MIT AND NCSA
URL:            https://github.com/magazine0410/Magpie
Source0:        magpie-%{magpie_commit}.tar.gz
Source1:        https://github.com/microsoft/DirectXShaderCompiler/releases/download/%{dxc_version}/%{dxc_archive}
Source2:        https://github.com/ocornut/imgui/archive/refs/tags/v%{imgui_version}.tar.gz#/imgui-%{imgui_version}.tar.gz
Source3:        https://raw.githubusercontent.com/microsoft/fluentui-system-icons/%{icon_font_commit}/fonts/FluentSystemIcons-Resizable.ttf

# Microsoft publishes the shader compiler for this architecture only
ExclusiveArch:  x86_64

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  python3
BuildRequires:  fmt-devel
BuildRequires:  spdlog-devel
BuildRequires:  rapidjson-devel
BuildRequires:  muParser-devel
BuildRequires:  libpng-devel
BuildRequires:  fontconfig-devel
BuildRequires:  vulkan-headers
BuildRequires:  vulkan-loader-devel
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  wlr-protocols-devel
BuildRequires:  plasma-wayland-protocols-devel
BuildRequires:  pipewire-devel
BuildRequires:  systemd-devel
BuildRequires:  libxkbcommon-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qttools-devel
BuildRequires:  qt6-linguist
BuildRequires:  desktop-file-utils
BuildRequires:  appstream
# For the tests: the test framework, a Vulkan implementation that needs no
# graphics card, the icon format of the window, and a font for the toolbar
BuildRequires:  catch-devel
BuildRequires:  mesa-vulkan-drivers
BuildRequires:  qt6-qtsvg
BuildRequires:  dejavu-sans-fonts

Requires:       vulkan-loader
Requires:       hicolor-icon-theme
# What the window needs and no library names: the Wayland platform of Qt
# and the reader for the icon
Requires:       qt6-qtwayland
Requires:       qt6-qtsvg
# The look of the desktop on KDE Plasma; without it the window has Qt's own
Recommends:     kf6-qqc2-desktop-style
# Sets the mode of the virtual output on KDE Plasma
Recommends:     libkscreen
# Nested sessions run in a Sway that Magpie starts for them
Recommends:     sway

%description
Magpie magnifies a window to the size of the screen or into a larger window,
with a choice of scaling algorithms and filters: Lanczos, FSR, Anime4K, CRT
shaders and many more, which can be combined into scaling modes. It is made
for games and other programs that only run at a low resolution.

This is the Linux version of the Windows program of the same name. On KDE
Plasma and on Sway it scales windows that are open already. On every desktop
it can start a program in a session of its own and scale that.

%prep
%autosetup -n magpie-%{magpie_commit}
mkdir -p _deps/dxc
tar -xf %{SOURCE1} -C _deps/dxc
tar -xf %{SOURCE2} -C _deps

%build
%cmake -G Ninja \
    -DMAGPIE_FETCH_DXC=OFF -DDXC_ROOT=$PWD/_deps/dxc \
    -DMAGPIE_FETCH_IMGUI=OFF -DIMGUI_ROOT=$PWD/_deps/imgui-%{imgui_version} \
    -DMAGPIE_FETCH_ICON_FONT=OFF -DMAGPIE_ICON_FONT=%{SOURCE3} \
    -DMAGPIE_VERSION_STRING=%{magpie_base}+%{magpie_date} \
    -DMAGPIE_COMMIT_ID=%{magpie_commit} \
    -DMAGPIE_WARNINGS_AS_ERRORS=OFF
%cmake_build

%install
%cmake_install
install -D -m 0644 _deps/dxc/LICENSE-LLVM.txt %{buildroot}%{_licensedir}/%{name}/DirectXShaderCompiler-LICENSE-LLVM.txt
install -D -m 0644 _deps/dxc/LICENSE-MS.txt %{buildroot}%{_licensedir}/%{name}/DirectXShaderCompiler-LICENSE-MS.txt
install -D -m 0644 _deps/imgui-%{imgui_version}/LICENSE.txt %{buildroot}%{_licensedir}/%{name}/DearImGui-LICENSE.txt
install -D -m 0644 linux/packaging/FluentSystemIcons-LICENSE.txt %{buildroot}%{_licensedir}/%{name}/FluentSystemIcons-LICENSE.txt

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{app_id}.desktop
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/%{app_id}.metainfo.xml
# Everything except the test that compiles every effect in six combinations.
# The renderer tests run on the software implementation of Vulkan; the tests
# that need a PipeWire session or the program "sway" skip themselves.
%ctest -LE slow

%files
%license LICENSE
%license %{_licensedir}/%{name}/DirectXShaderCompiler-LICENSE-LLVM.txt
%license %{_licensedir}/%{name}/DirectXShaderCompiler-LICENSE-MS.txt
%license %{_licensedir}/%{name}/DearImGui-LICENSE.txt
%license %{_licensedir}/%{name}/FluentSystemIcons-LICENSE.txt
%doc linux/docs/*.md
%{_bindir}/magpie
%{_bindir}/magpie-ctl
%{_bindir}/magpie-engine
%{_bindir}/magpie-fxc
%{_bindir}/magpie-scale
%{_bindir}/magpie-scale-image
%{_bindir}/magpie-viewer
%{_libdir}/magpie/
%{_datadir}/magpie/
%{_datadir}/applications/%{app_id}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_metainfodir}/%{app_id}.metainfo.xml

%changelog
* Wed Nov 11 2026 Magpie contributors - 0.13.0-1
- First package of the Linux version
