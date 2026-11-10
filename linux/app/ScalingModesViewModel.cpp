#include "ScalingModesViewModel.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include "EffectsService.h"
#include "SettingsJson.h"

namespace Magpie {

ScalingModesViewModel::ScalingModesViewModel(AppSettings& settings, EffectsService& effects, QObject* parent) :
	QObject(parent), _settings(settings), _effects(effects),
	_expanded(settings.Data().scalingModes.size(), false) {}

int ScalingModesViewModel::Count() const {
	return (int)_settings.Data().scalingModes.size();
}

bool ScalingModesViewModel::_IsValid(int mode) const {
	return mode >= 0 && mode < Count();
}

bool ScalingModesViewModel::_IsValid(int mode, int effect) const {
	return _IsValid(mode) && effect >= 0 &&
		effect < (int)_settings.Data().scalingModes[(size_t)mode].effects.size();
}

const EffectInfo* ScalingModesViewModel::_EffectInfo(int mode, int effect) const {
	return _IsValid(mode, effect) ?
		_effects.GetEffect(_settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect].name) : nullptr;
}

void ScalingModesViewModel::_Save() {
	_settings.Save();
	++_revision;
	Q_EMIT Changed();
}

QString ScalingModesViewModel::modeName(int mode) const {
	return _IsValid(mode) ? QString::fromStdString(_settings.Data().scalingModes[(size_t)mode].name) : QString();
}

QString ScalingModesViewModel::modeDescription(int mode) const {
	if (!_IsValid(mode)) {
		return {};
	}

	QString result;
	for (const EffectItem& effect : _settings.Data().scalingModes[(size_t)mode].effects) {
		if (!result.isEmpty()) {
			result += " > ";
		}

		if (_effects.GetEffect(effect.name)) {
			const std::string_view name = EffectsService::DisplayName(effect.name);
			result += QString::fromUtf8(name.data(), (qsizetype)name.size());
		} else {
			result += "(" + qtTrId("ScalingModes_Description_UnknownEffect") + ")";
		}
	}
	return result;
}

bool ScalingModesViewModel::hasUnknownEffects(int mode) const {
	if (!_IsValid(mode)) {
		return false;
	}
	return std::ranges::any_of(_settings.Data().scalingModes[(size_t)mode].effects,
		[&](const EffectItem& effect) { return !_effects.GetEffect(effect.name); });
}

QStringList ScalingModesViewModel::linkedProfiles(int mode) const {
	QStringList profiles;
	const Settings& data = _settings.Data();
	if (data.defaultProfile.scalingMode == mode) {
		profiles.push_back(qtTrId("Root_Defaults.Content"));
	}
	for (const Profile& profile : data.profiles) {
		if (profile.scalingMode == mode) {
			profiles.push_back(QString::fromStdString(profile.name));
		}
	}
	return profiles;
}

bool ScalingModesViewModel::isExpanded(int mode) const {
	return mode >= 0 && mode < (int)_expanded.size() && _expanded[(size_t)mode];
}

void ScalingModesViewModel::setExpanded(int mode, bool value) {
	if (mode >= 0 && mode < (int)_expanded.size()) {
		_expanded[(size_t)mode] = value;
	}
}

bool ScalingModesViewModel::canRenameMode(int mode, const QString& name) const {
	const QString trimmed = name.trimmed();
	return _IsValid(mode) && !trimmed.isEmpty() && trimmed != modeName(mode);
}

void ScalingModesViewModel::renameMode(int mode, const QString& name) {
	if (!canRenameMode(mode, name)) {
		return;
	}
	_settings.Data().scalingModes[(size_t)mode].name = name.trimmed().toStdString();
	_Save();
}

void ScalingModesViewModel::moveMode(int mode, bool up) {
	const int target = up ? mode - 1 : mode + 1;
	if (!_IsValid(mode) || !_IsValid(target)) {
		return;
	}

	Settings& data = _settings.Data();
	std::swap(data.scalingModes[(size_t)mode], data.scalingModes[(size_t)target]);
	std::vector<bool>::swap(_expanded[(size_t)mode], _expanded[(size_t)target]);

	// The profiles name their scaling mode by its place
	const auto update = [&](Profile& profile) {
		if (profile.scalingMode == mode) {
			profile.scalingMode = target;
		} else if (profile.scalingMode == target) {
			profile.scalingMode = mode;
		}
	};
	update(data.defaultProfile);
	std::ranges::for_each(data.profiles, update);

	_Save();
	Q_EMIT modesReordered();
}

void ScalingModesViewModel::removeMode(int mode) {
	if (!_IsValid(mode)) {
		return;
	}

	Settings& data = _settings.Data();
	data.scalingModes.erase(data.scalingModes.begin() + mode);
	_expanded.erase(_expanded.begin() + mode);

	const auto update = [&](Profile& profile) {
		if (profile.scalingMode == mode) {
			profile.scalingMode = -1;
		} else if (profile.scalingMode > mode) {
			--profile.scalingMode;
		}
	};
	update(data.defaultProfile);
	std::ranges::for_each(data.profiles, update);

	_Save();
	Q_EMIT modesReordered();
}

QStringList ScalingModesViewModel::copyFromList() const {
	QStringList list{ qtTrId("ScalingModes_NewScalingModeFlyout_CopyFrom_None") };
	for (const ScalingMode& scalingMode : _settings.Data().scalingModes) {
		list.push_back(QString::fromStdString(scalingMode.name));
	}
	return list;
}

bool ScalingModesViewModel::canAddMode(const QString& name) const {
	return !name.trimmed().isEmpty();
}

void ScalingModesViewModel::addMode(const QString& name, int copyFrom) {
	if (!canAddMode(name)) {
		return;
	}

	std::vector<ScalingMode>& scalingModes = _settings.Data().scalingModes;
	ScalingMode scalingMode;
	if (_IsValid(copyFrom - 1)) {
		scalingMode = scalingModes[(size_t)copyFrom - 1];
	}
	scalingMode.name = name.trimmed().toStdString();
	scalingModes.push_back(std::move(scalingMode));
	// A scaling mode the user just made shows its effects
	_expanded.push_back(true);

	_Save();
	Q_EMIT modesReordered();
}

int ScalingModesViewModel::effectCount(int mode) const {
	return _IsValid(mode) ? (int)_settings.Data().scalingModes[(size_t)mode].effects.size() : 0;
}

QString ScalingModesViewModel::effectName(int mode, int effect) const {
	if (!_IsValid(mode, effect)) {
		return {};
	}

	const std::string& name = _settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect].name;
	if (_effects.GetEffect(name)) {
		const std::string_view displayName = EffectsService::DisplayName(name);
		return QString::fromUtf8(displayName.data(), (qsizetype)displayName.size());
	}
	return qtTrId("ScalingModes_Description_UnknownEffect") + " (" + QString::fromStdString(name) + ")";
}

bool ScalingModesViewModel::effectCanScale(int mode, int effect) const {
	const EffectInfo* info = _EffectInfo(mode, effect);
	return info && info->canScale;
}

bool ScalingModesViewModel::effectHasParameters(int mode, int effect) const {
	const EffectInfo* info = _EffectInfo(mode, effect);
	return info && !info->params.empty();
}

void ScalingModesViewModel::addEffect(int mode, const QString& fullName) {
	const EffectInfo* info = _effects.GetEffect(fullName.toStdString());
	if (!_IsValid(mode) || !info) {
		return;
	}

	EffectItem& effect = _settings.Data().scalingModes[(size_t)mode].effects.emplace_back();
	effect.name = info->name;
	if (info->canScale) {
		effect.scalingType = ScalingType::Fit;
	}
	_Save();
}

void ScalingModesViewModel::removeEffect(int mode, int effect) {
	if (!_IsValid(mode, effect)) {
		return;
	}
	std::vector<EffectItem>& effects = _settings.Data().scalingModes[(size_t)mode].effects;
	effects.erase(effects.begin() + effect);
	_Save();
}

void ScalingModesViewModel::moveEffect(int mode, int from, int to) {
	if (!_IsValid(mode, from) || !_IsValid(mode, to) || from == to) {
		return;
	}

	std::vector<EffectItem>& effects = _settings.Data().scalingModes[(size_t)mode].effects;
	EffectItem moved = std::move(effects[(size_t)from]);
	effects.erase(effects.begin() + from);
	effects.insert(effects.begin() + to, std::move(moved));
	_Save();
}

int ScalingModesViewModel::scalingType(int mode, int effect) const {
	return _IsValid(mode, effect) ?
		(int)_settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect].scalingType : 0;
}

void ScalingModesViewModel::setScalingType(int mode, int effect, int value) {
	if (!_IsValid(mode, effect) || value < 0 || value >= (int)ScalingType::COUNT) {
		return;
	}

	EffectItem& data = _settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect];
	const ScalingType type = (ScalingType)value;
	if (data.scalingType == type) {
		return;
	}

	if (data.scalingType == ScalingType::Absolute) {
		// From pixels back to a factor
		data.scale = { 1.0f, 1.0f };
	} else if (type == ScalingType::Absolute) {
		// The size of the screen is the size to start from
		QSize size(400, 300);
		if (const QScreen* screen = QGuiApplication::primaryScreen()) {
			size = screen->size() * screen->devicePixelRatio();
		}
		data.scale = { (float)size.width(), (float)size.height() };
	}
	data.scalingType = type;
	_Save();
}

double ScalingModesViewModel::scaleX(int mode, int effect) const {
	return _IsValid(mode, effect) ?
		_settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect].scale.first : 0.0;
}

double ScalingModesViewModel::scaleY(int mode, int effect) const {
	return _IsValid(mode, effect) ?
		_settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect].scale.second : 0.0;
}

// A factor has to be positive, a number of pixels at least one. An invalid
// value is not taken, and the page shows the old one again.
static bool AcceptScale(ScalingType type, double value, float& scale) {
	if (std::isnan(value)) {
		return false;
	}
	if (type == ScalingType::Absolute) {
		if (value <= 0.5) {
			return false;
		}
		scale = (float)std::round(value);
		return true;
	}
	if (type == ScalingType::Normal || type == ScalingType::Fit) {
		if (value + std::numeric_limits<float>::epsilon() <= 1e-4) {
			return false;
		}
		scale = (float)value;
		return true;
	}
	return false;
}

void ScalingModesViewModel::setScaleX(int mode, int effect, double value) {
	if (!_IsValid(mode, effect)) {
		return;
	}
	EffectItem& data = _settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect];
	AcceptScale(data.scalingType, value, data.scale.first);
	_Save();
}

void ScalingModesViewModel::setScaleY(int mode, int effect, double value) {
	if (!_IsValid(mode, effect)) {
		return;
	}
	EffectItem& data = _settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect];
	AcceptScale(data.scalingType, value, data.scale.second);
	_Save();
}

// A parameter of the type int that is 0 or 1 is shown as a check box
static bool IsBoolParameter(const EffectParameterDesc& param) {
	if (param.constant.index() != 1) {
		return false;
	}
	const EffectConstant<int>& constant = std::get<1>(param.constant);
	return constant.minValue == 0 && constant.maxValue == 1 && constant.step == 1;
}

QVariantList ScalingModesViewModel::parameters(int mode, int effect) const {
	const EffectInfo* info = _EffectInfo(mode, effect);
	if (!info) {
		return {};
	}

	const std::map<std::string, float>& values =
		_settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect].parameters;

	QVariantList result;
	for (const EffectParameterDesc& param : info->params) {
		double defaultValue, minimum, maximum, step;
		if (param.constant.index() == 0) {
			const EffectConstant<float>& constant = std::get<0>(param.constant);
			defaultValue = constant.defaultValue;
			minimum = constant.minValue;
			maximum = constant.maxValue;
			step = constant.step;
		} else {
			const EffectConstant<int>& constant = std::get<1>(param.constant);
			defaultValue = constant.defaultValue;
			minimum = constant.minValue;
			maximum = constant.maxValue;
			step = constant.step;
		}

		const auto it = values.find(param.name);
		const double value = it == values.end() ? defaultValue : it->second;
		const bool isBool = IsBoolParameter(param);

		result.push_back(QVariantMap{
			{ "label", QString::fromStdString(param.label.empty() ? param.name : param.label) },
			{ "isBool", isBool },
			{ "value", isBool ? double(std::abs(value) > 1e-6) : value },
			{ "minimum", minimum },
			{ "maximum", maximum },
			{ "step", step }
		});
	}
	return result;
}

void ScalingModesViewModel::setParameter(int mode, int effect, int parameter, double value) {
	const EffectInfo* info = _EffectInfo(mode, effect);
	if (!info || parameter < 0 || parameter >= (int)info->params.size()) {
		return;
	}

	float& stored = _settings.Data().scalingModes[(size_t)mode].effects[(size_t)effect]
		.parameters[info->params[(size_t)parameter].name];
	if (stored == (float)value) {
		return;
	}
	stored = (float)value;
	_Save();
}

QVariantList ScalingModesViewModel::effectMenu() const {
	// Ordered by sortName within a folder
	std::vector<const EffectInfo*> effects;
	for (const EffectInfo& effect : _effects.Effects()) {
		effects.push_back(&effect);
	}
	std::ranges::sort(effects, {}, &EffectInfo::sortName);

	std::map<QString, QVariantList> folders;
	std::map<QString, QVariant> rootItems;
	for (const EffectInfo* effect : effects) {
		const QString fullName = QString::fromStdString(effect->name);
		const qsizetype pos = fullName.lastIndexOf('\\');
		const QVariantMap item{ { "name", fullName.mid(pos + 1) }, { "fullName", fullName } };
		if (pos < 0) {
			rootItems.emplace(fullName, item);
		} else {
			folders[fullName.left(pos)].push_back(item);
		}
	}

	QVariantList menu;
	for (const auto& [folder, items] : folders) {
		menu.push_back(QVariantMap{ { "folder", folder }, { "items", items } });
	}
	for (const auto& [name, item] : rootItems) {
		menu.push_back(item);
	}
	return menu;
}

bool ScalingModesViewModel::exportTo(const QUrl& file) const {
	const std::string json = WriteScalingModesJson(_settings.Data().scalingModes);
	QFile output(file.toLocalFile());
	return output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
		output.write(json.data(), (qint64)json.size()) == (qint64)json.size();
}

bool ScalingModesViewModel::importFrom(const QUrl& file) {
	QFile input(file.toLocalFile());
	if (!input.open(QIODevice::ReadOnly)) {
		return false;
	}
	const QByteArray json = input.readAll();

	std::vector<ScalingMode>& scalingModes = _settings.Data().scalingModes;
	if (json.isEmpty() || !ReadScalingModesJson(std::string_view(json.data(), (size_t)json.size()), scalingModes)) {
		return false;
	}

	if (scalingModes.size() != _expanded.size()) {
		_expanded.resize(scalingModes.size(), false);
		_Save();
		Q_EMIT modesReordered();
	}
	return true;
}

}
