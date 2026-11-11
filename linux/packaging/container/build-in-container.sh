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
SOURCE=$(dirname "$BUILD")
echo "build tree: $BUILD"
# The install check uses the test window, which is not part of the package
cp "$BUILD/tests/magpie_test_window" /out/

echo
echo "################ the tests with Sway, on the software renderer ################"
# From the package files the image has kept
dnf -y -C install sway > /out/sway-install.log 2>&1 || dnf -y install sway >> /out/sway-install.log 2>&1
echo "sway: $(rpm -q sway)"
# No graphics card and no session here: Sway composes in software, Vulkan is
# llvmpipe, and the tests make runtime directories of their own. One test at
# a time: the software renderer uses every processor core by itself.
"$BUILD/tests/magpie_engine_tests" "[sway]" > /out/sway-tests.log 2>&1
echo "magpie_engine_tests [sway]: exit status $?; $(tail -3 /out/sway-tests.log | tr '\n' ' ')"
"$SOURCE/linux/tests/tools/sway_service_test.sh" "$BUILD" "$SOURCE/src/Effects" > /out/sway-service.log 2>&1
echo "sway_service_test.sh: exit status $?; $(tail -1 /out/sway-service.log)"
