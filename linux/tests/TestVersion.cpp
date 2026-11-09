#include <catch2/catch_test_macros.hpp>
#include "Version.h"

using Magpie::Version;

TEST_CASE("Version parses two and three numbers", "[Version]") {
	Version v;
	REQUIRE(v.Parse("0.13.2"));
	CHECK(v == Version(0, 13, 2));
	CHECK(v.ToString() == "0.13.2");

	Version twoParts;
	REQUIRE(twoParts.Parse("1.4"));
	CHECK(twoParts == Version(1, 4, 0));
}

TEST_CASE("Version rejects anything else", "[Version]") {
	Version v;
	CHECK_FALSE(v.Parse(""));
	CHECK_FALSE(v.Parse("1"));
	CHECK_FALSE(v.Parse("1.2.3.4"));
	CHECK_FALSE(v.Parse("1.x"));
	CHECK_FALSE(v.Parse("1.2-beta"));
	CHECK_FALSE(v.Parse("v1.2"));
}

TEST_CASE("Versions compare by major, minor, patch", "[Version]") {
	CHECK(Version(0, 13, 0) > Version(0, 12, 9));
	CHECK(Version(1, 0, 0) > Version(0, 99, 99));
	CHECK(Version(0, 13, 1) > Version(0, 13, 0));
	CHECK(Version(0, 13, 0) == Version(0, 13, 0));
}
