#include "ProfileRules.h"

namespace Magpie {

static const Profile* FindRule(
	const Settings& settings,
	std::string_view className,
	std::string_view path,
	bool forAutoScale
) noexcept {
	if (className.empty() || path.empty()) {
		return nullptr;
	}

	for (const Profile& profile : settings.profiles) {
		if (forAutoScale && profile.autoScale == AutoScale::Disabled) {
			continue;
		}
		if (!profile.isPackaged && profile.classNameRule == className && profile.pathRule == path) {
			return &profile;
		}
	}
	return nullptr;
}

const Profile* FindProfileForWindow(
	const Settings& settings,
	std::string_view className,
	std::string_view path,
	bool forAutoScale
) noexcept {
	const Profile* profile = FindRule(settings, className, path, forAutoScale);
	return profile || forAutoScale ? profile : &settings.defaultProfile;
}

bool TestNewProfile(const Settings& settings, std::string_view className, std::string_view path) noexcept {
	return !className.empty() && !path.empty() && !FindRule(settings, className, path, false);
}

}
