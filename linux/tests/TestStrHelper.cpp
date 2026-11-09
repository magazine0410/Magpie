#include <catch2/catch_test_macros.hpp>
#include "StrHelper.h"

using Magpie::StrHelper;

TEST_CASE("Trim removes white space at both ends", "[StrHelper]") {
	std::string_view view = " \t text with spaces\r\n";
	StrHelper::Trim(view);
	CHECK(view == "text with spaces");

	std::string_view blank = " \t\n";
	StrHelper::Trim(blank);
	CHECK(blank.empty());

	std::string str = "  x ";
	StrHelper::Trim(str);
	CHECK(str == "x");
}

TEST_CASE("Split keeps empty pieces between delimiters", "[StrHelper]") {
	const auto parts = StrHelper::Split("0.13.0", '.');
	REQUIRE(parts.size() == 3);
	CHECK(parts[0] == "0");
	CHECK(parts[1] == "13");
	CHECK(parts[2] == "0");

	const auto withEmpty = StrHelper::Split("a,,b", ',');
	REQUIRE(withEmpty.size() == 3);
	CHECK(withEmpty[1].empty());

	CHECK(StrHelper::Split("", ',').empty());
	CHECK(StrHelper::Split("single", ',').size() == 1);
}

TEST_CASE("Case conversion changes ASCII letters only", "[StrHelper]") {
	CHECK(StrHelper::ToLowerCase(std::string_view("ZH-Hans")) == "zh-hans");
	CHECK(StrHelper::ToUpperCase(std::string_view("pass 1")) == "PASS 1");
	// Bytes of multi-byte characters stay as they are
	CHECK(StrHelper::ToLowerCase(std::string_view("月姫 A")) == "月姫 a");
}

TEST_CASE("Concat joins its arguments", "[StrHelper]") {
	const std::string name = "config";
	CHECK(StrHelper::Concat(name, ".", std::string_view("json")) == "config.json");
	CHECK(StrHelper::Concat("a") == "a");
}
