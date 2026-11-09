// Shared by the test files.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

// Sets or removes an environment variable and restores the old state.
class ScopedEnv {
public:
	ScopedEnv(const char* name, const char* value) : _name(name) {
		if (const char* old = std::getenv(name)) {
			_old = old;
		}
		if (value) {
			setenv(name, value, 1);
		} else {
			unsetenv(name);
		}
	}

	~ScopedEnv() {
		if (_old) {
			setenv(_name.c_str(), _old->c_str(), 1);
		} else {
			unsetenv(_name.c_str());
		}
	}

private:
	std::string _name;
	std::optional<std::string> _old;
};

// A fresh directory below the system's temporary directory, removed again at
// the end of the test.
class TempDir {
public:
	TempDir() {
		std::string pattern = (std::filesystem::temp_directory_path() / "magpie-test-XXXXXX").string();
		_path = mkdtemp(pattern.data());
	}

	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
	}

	const std::filesystem::path& Path() const {
		return _path;
	}

private:
	std::filesystem::path _path;
};

inline std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	std::ostringstream content;
	content << file.rdbuf();
	return std::move(content).str();
}
