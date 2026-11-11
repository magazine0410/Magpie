#!/usr/bin/env bash
# Builds the RPM of the Linux version from the commit that is checked out.
#
# The source is a git archive of HEAD, so changes that are not committed are
# not in the package. The three files the build would otherwise download
# (the DirectX Shader Compiler, Dear ImGui, the icon font; see
# linux/cmake/Dxc.cmake and ImGui.cmake) are taken from DEPS_DIR, where an
# ordinary build has put them, so that rpmbuild needs no network.
#
# Usage: build-rpm.sh [RPMBUILD_OPTION]...   (default: -ba)
# Environment:
#   TOPDIR    where rpmbuild works and leaves the packages (default: a new
#             directory in /tmp); nothing in ~/rpmbuild is touched
#   DEPS_DIR  the directory with the downloaded files (default:
#             linux/build/_deps)
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
DEPS_DIR=${DEPS_DIR:-$REPO/linux/build/_deps}
TOPDIR=${TOPDIR:-$(mktemp -d /tmp/magpie-rpmbuild.XXXXXX)}
SPEC=$REPO/linux/packaging/magpie-upscaler.spec
[ $# -eq 0 ] && set -- -ba

# v0.13.0-42-g08cce49a: the last release, and the commit
description=$(git -C "$REPO" describe --tags --long --abbrev=8)
base=$(echo "$description" | sed -E 's/^v([0-9.]+).*/\1/')
count=$(echo "$description" | sed -E 's/^v[0-9.]+-([0-9]+)-g.*/\1/')
commit=$(git -C "$REPO" rev-parse --short=8 HEAD)
date=$(git -C "$REPO" log -1 --format=%cd --date=format:%Y%m%d)

macro() { sed -nE "s/^%global $1 (.*)$/\1/p" "$SPEC"; }
dxc_archive=$(macro dxc_archive)
imgui_version=$(macro imgui_version)

mkdir -p "$TOPDIR/SOURCES" "$TOPDIR/SPECS"
for file in "$DEPS_DIR/$dxc_archive" "$DEPS_DIR/imgui-$imgui_version.tar.gz" \
	"$DEPS_DIR/fluent-icons/FluentSystemIcons-Resizable.ttf"; do
	if [ ! -f "$file" ]; then
		echo "$file is missing; build Magpie once (cmake -S linux -B linux/build) or set DEPS_DIR" >&2
		exit 1
	fi
	cp "$file" "$TOPDIR/SOURCES/"
done
git -C "$REPO" archive --format=tar.gz --prefix="magpie-$commit/" -o "$TOPDIR/SOURCES/magpie-$commit.tar.gz" HEAD
cp "$SPEC" "$TOPDIR/SPECS/"

rpmbuild "$@" \
	--define "_topdir $TOPDIR" \
	--define "magpie_base $base" --define "magpie_date $date" --define "magpie_count $count" \
	--define "magpie_commit $commit" \
	"$TOPDIR/SPECS/magpie-upscaler.spec"
echo
echo "packages:"
find "$TOPDIR/RPMS" "$TOPDIR/SRPMS" -name '*.rpm' 2> /dev/null | sort
