// The data of the scaling modes page. Linux counterpart of
// ScalingModesViewModel, ScalingModeItem, ScalingModeEffectItem,
// EffectParametersViewModel and ScalingModesService (src/Magpie).
//
// On Windows every scaling mode and every effect of it is an object of its
// own that the page binds to. Here the page asks this one object with the
// index of the scaling mode and of the effect, and asks again whenever
// `revision` changes.
#pragma once
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include "AppSettings.h"

namespace Magpie {

class EffectsService;
struct EffectInfo;

class ScalingModesViewModel : public QObject {
	Q_OBJECT
	// Counts the changes; a binding that names it is evaluated again after
	// each one
	Q_PROPERTY(int revision READ Revision NOTIFY Changed)
	Q_PROPERTY(int count READ Count NOTIFY Changed)

public:
	ScalingModesViewModel(AppSettings& settings, EffectsService& effects, QObject* parent = nullptr);

	int Revision() const noexcept {
		return _revision;
	}

	int Count() const;

	// The scaling modes
	Q_INVOKABLE QString modeName(int mode) const;
	// The effects in order, for example "FSR_EASU > FSR_RCAS"
	Q_INVOKABLE QString modeDescription(int mode) const;
	Q_INVOKABLE bool hasUnknownEffects(int mode) const;
	// The profiles that use the scaling mode, which are asked about before
	// it is deleted
	Q_INVOKABLE QStringList linkedProfiles(int mode) const;
	Q_INVOKABLE bool isExpanded(int mode) const;
	Q_INVOKABLE void setExpanded(int mode, bool value);
	Q_INVOKABLE bool canRenameMode(int mode, const QString& name) const;
	Q_INVOKABLE void renameMode(int mode, const QString& name);
	Q_INVOKABLE void moveMode(int mode, bool up);
	Q_INVOKABLE void removeMode(int mode);
	// "None" first, then the names of the scaling modes
	Q_INVOKABLE QStringList copyFromList() const;
	Q_INVOKABLE bool canAddMode(const QString& name) const;
	// `copyFrom` is an index into copyFromList
	Q_INVOKABLE void addMode(const QString& name, int copyFrom);

	// The effects of a scaling mode
	Q_INVOKABLE int effectCount(int mode) const;
	Q_INVOKABLE QString effectName(int mode, int effect) const;
	Q_INVOKABLE bool effectCanScale(int mode, int effect) const;
	Q_INVOKABLE bool effectHasParameters(int mode, int effect) const;
	Q_INVOKABLE void addEffect(int mode, const QString& fullName);
	Q_INVOKABLE void removeEffect(int mode, int effect);
	Q_INVOKABLE void moveEffect(int mode, int from, int to);

	// How an effect scales: 0 by a factor, 1 by a factor of the size that
	// fits the screen, 2 to a size in pixels, 3 filling the screen
	Q_INVOKABLE int scalingType(int mode, int effect) const;
	Q_INVOKABLE void setScalingType(int mode, int effect, int value);
	// Factors for the types 0 and 1, pixels for the type 2
	Q_INVOKABLE double scaleX(int mode, int effect) const;
	Q_INVOKABLE double scaleY(int mode, int effect) const;
	Q_INVOKABLE void setScaleX(int mode, int effect, double value);
	Q_INVOKABLE void setScaleY(int mode, int effect, double value);

	// The parameters of an effect: for each a map with "label", "isBool",
	// "value", "minimum", "maximum" and "step"
	Q_INVOKABLE QVariantList parameters(int mode, int effect) const;
	Q_INVOKABLE void setParameter(int mode, int effect, int parameter, double value);

	// The effects for the menu: folders first, each a map with "folder" and
	// "items"; then the effects outside folders. An effect is a map with
	// "name" and "fullName".
	Q_INVOKABLE QVariantList effectMenu() const;

	// Writes all scaling modes to a file, and adds those of a file. False
	// if that fails.
	Q_INVOKABLE bool exportTo(const QUrl& file) const;
	Q_INVOKABLE bool importFrom(const QUrl& file);

Q_SIGNALS:
	void Changed();
	// The scaling modes are not where they were: the page reads again which
	// ones are expanded
	void modesReordered();

private:
	bool _IsValid(int mode) const;
	bool _IsValid(int mode, int effect) const;
	const EffectInfo* _EffectInfo(int mode, int effect) const;
	void _Save();

	AppSettings& _settings;
	EffectsService& _effects;
	int _revision = 0;
	// For every scaling mode whether the page shows its effects
	std::vector<bool> _expanded;
};

}
