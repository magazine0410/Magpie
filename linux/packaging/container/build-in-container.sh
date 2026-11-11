#!/usr/bin/env bash
# Runs inside the build image, started by container-check.sh: builds the RPM
# from the repository mounted at /src, then runs the tests that the package's
# own check leaves out because they need the program "sway".
set -u
export HOME=/root
git config --global --add safe.directory /src
cd /src

echo "################ rpmbuild ################"
TOPDIR=/work/rpmbuild DEPS_DIR=/src/linux/build/_deps linux/packaging/build-rpm.sh -ba --noclean > /out/rpmbuild.log 2>&1
status=$?
grep -E "^(Wrote|error|Error|RPM build errors|[0-9]+% tests passed|The following tests FAILED|\s+[0-9]+ - )" /out/rpmbuild.log | tail -30
echo "rpmbuild exit status: $status"
[ "$status" -eq 0 ] || exit 1
cp /work/rpmbuild/RPMS/*/*.rpm /work/rpmbuild/SRPMS/*.rpm /out/

BUILD=$(dirname "$(find /work/rpmbuild/BUILD -name CTestTestfile.cmake -path '*redhat-linux-build/CTestTestfile.cmake' | head -1)")
SOURCE=$(dirname "$(dirname "$BUILD")")
echo "build tree: $BUILD"
# The install check uses the test window, which is not part of the package
cp "$BUILD/tests/magpie_test_window" /out/

echo
echo "################ the tests with Sway, on the software renderer ################"
# No graphics card and no session here: Sway composes with pixman, Vulkan is
# llvmpipe, and the runtime directory is one of the tests' own.
export XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_RENDERER=pixman
"$BUILD/tests/magpie_engine_tests" "[sway]" > /out/sway-tests.log 2>&1
echo "magpie_engine_tests [sway]: exit status $?; $(tail -3 /out/sway-tests.log | tr '\n' ' ')"
"$SOURCE/linux/tests/tools/sway_service_test.sh" "$BUILD" "$SOURCE/src/Effects" > /out/sway-service.log 2>&1
echo "sway_service_test.sh: exit status $?; $(tail -1 /out/sway-service.log)"
