#!/usr/bin/env bash
# The check of the package that needs no second machine: in a container with
# Fedora, without a graphics card and without anything of this machine's
# installation,
#   1. the RPM is built from a git archive of HEAD, with the tests of its
#      spec file, and the tests of Sway and of the nested session are run
#      on the software renderer (what a CI job would do);
#   2. the RPM is installed into a plain Fedora with dnf, which shows a
#      dependency the package does not name, and the installed programs
#      are tried.
#
# The first run downloads the Fedora image and the packages of the build
# (about 1 GB) into podman's storage as the image localhost/magpie-build,
# and the packages of the installation into OUT/dnf-cache. Later runs reuse
# both. The repository is mounted read-only.
#
# Usage: container-check.sh [build] [install]   (default: both)
# Environment: OUT, the directory for packages and logs (default: a new one
#   in /tmp)
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$REPO/linux/packaging
OUT=${OUT:-$(mktemp -d /tmp/magpie-container.XXXXXX)}
mkdir -p "$OUT/dnf-cache"
STEPS=("$@")
[ ${#STEPS[@]} -eq 0 ] && STEPS=(build install)
echo "packages and logs: $OUT"

for step in "${STEPS[@]}"; do
	case "$step" in
	build)
		podman build -q -t localhost/magpie-build:f44 -f "$HERE/container/Containerfile" "$HERE"
		# Without a label of its own the container can read the repository
		# as it is; nothing of it is relabelled
		# SYS_NICE: Fedora's sway has that capability on its file, and a
		# container without it cannot start the program at all
		podman run --rm --security-opt label=disable --cap-add SYS_NICE --tmpfs /work:exec \
			-v "$REPO":/src:ro -v "$OUT":/out \
			localhost/magpie-build:f44 bash /src/linux/packaging/container/build-in-container.sh
		;;
	install)
		podman run --rm --security-opt label=disable --cap-add SYS_NICE \
			-v "$HERE/container":/scripts:ro -v "$OUT":/out -v "$OUT/dnf-cache":/var/cache/libdnf5 \
			registry.fedoraproject.org/fedora:44 bash /scripts/install-check.sh
		;;
	*)
		echo "unknown step $step" >&2
		exit 2
		;;
	esac
done
