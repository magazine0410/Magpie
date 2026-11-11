// Back end for KWin on Wayland. It uses interfaces that KWin 6.8 offers to
// ordinary programs (see linux/spikes/RESULTS.md and the Phase 5 notes of the
// plan):
// - org_kde_plasma_window_management: the windows and their state
// - zkde_screencast_unstable_v1: screen casts without a dialog, and virtual
//   outputs
// - org_kde_kwin_fake_input: moving the pointer
// - KWin's scripting interface on D-Bus: placing a window exactly
// - the program kscreen-doctor: position and refresh rate of a virtual output
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Bridge.h"

namespace Magpie {

// The modes of outputs as the program kscreen-doctor reports them
struct KWinOutputModes {
	struct Mode {
		// What kscreen-doctor selects the mode by
		std::string id;
		Size size;
		// In hertz
		double refreshRate = 0.0;
	};

	// The modes of the output with this name in what "kscreen-doctor -j"
	// prints; empty if the text is something else or has no such output
	static std::vector<Mode> Parse(const std::string& json, const std::string& outputName) noexcept;

	// The mode of this size whose refresh rate is closest to `refreshRate`
	// (in thousandths of a hertz); null if no mode has the size
	static const Mode* Choose(const std::vector<Mode>& modes, Size size, int32_t refreshRate) noexcept;

	// Whether the mode serves for a wanted refresh rate, or a new mode has
	// to be made: rates that are computed for a size come out a little
	// different from the one asked for
	static bool IsCloseEnough(const Mode& mode, int32_t refreshRate) noexcept;
};

// The arrangement of the outputs as the program kscreen-doctor reports it.
// KWin keeps one arrangement for every set of outputs, and a virtual output
// makes it another set: the arrangement KWin kept for that set comes back
// when the virtual output appears, with whatever was true when it was last
// in use: a monitor that was disabled then, or one that stood elsewhere.
// With this the monitors are given what they had a moment before.
struct KWinOutputSetup {
	struct Output {
		std::string name;
		bool isEnabled = false;
		Point position;
		// The mode it has: its size and its refresh rate in hertz; zero if
		// not known. KWin numbers the modes anew when the set of outputs
		// changes, so a mode is known by these and not by its number.
		Size modeSize;
		double refreshRate = 0.0;
		// The modes it can have, with the numbers kscreen-doctor takes
		std::vector<KWinOutputModes::Mode> modes;
		double scale = 1.0;
		int priority = 0;
	};

	// The outputs in what "kscreen-doctor -j" prints; empty if the text is
	// something else
	static std::vector<Output> Parse(const std::string& json) noexcept;

	// The arguments for kscreen-doctor that give the outputs of `before`
	// back what they had, as far as `now` differs. An output that is in only
	// one of the two is left alone, and so is `except`.
	static std::vector<std::string> RestoreArguments(
		const std::vector<Output>& before,
		const std::vector<Output>& now,
		const std::string& except
	) noexcept;
};

class KWinBridge final : public DesktopBridge {
public:
	KWinBridge() noexcept;
	KWinBridge(const KWinBridge&) = delete;
	KWinBridge(KWinBridge&&) = delete;
	~KWinBridge() override;

	// Opens a Wayland connection of its own. Returns false if there is no
	// compositor or it offers neither the window list nor the screen cast.
	bool Initialize() noexcept;

	// The version in use, or 0 if the interface is not offered
	uint32_t WindowManagementVersion() const noexcept;
	uint32_t ScreencastVersion() const noexcept;
	bool HasFakeInput() const noexcept;

	const char* Name() const noexcept override {
		return "KWin";
	}

	Traits GetTraits() const noexcept override;

	std::string MissingForScaling() const noexcept override;

	int EventFd() const noexcept override;

	bool Dispatch() noexcept override;

	std::vector<WindowInfo> ListWindows() noexcept override;

	bool GetWindow(const std::string& windowId, WindowInfo& info) const noexcept override;

	std::string ActiveWindowId() const noexcept override;

	std::vector<OutputInfo> ListOutputs() noexcept override;

	// Starts a screen cast of the window, with the cursor as metadata of the
	// stream. Returns null if KWin refuses.
	std::unique_ptr<CaptureSession> CaptureWindow(const std::string& windowId) noexcept;

	// KWin takes the output's size from the stream's negotiation, so the
	// consumer must ask for exactly `size`.
	std::unique_ptr<CaptureSession> CreateVirtualOutput(
		const std::string& name, Size size, std::string& outputName) noexcept override;

	// A new virtual output has 60 Hz, or whatever mode KWin remembers for an
	// output of its name, which may have another size.
	bool ConfigureOutput(
		const std::string& outputName, Size size, Point position, int32_t refreshRate) noexcept override;

	bool PlaceWindow(const std::string& windowId, const WindowPlacement& placement) noexcept override;

	bool SetWindowProperties(const std::string& windowId, const WindowProperties& properties) noexcept override;

	bool MovePointer(Point position) noexcept override;

	// Through the properties KWin publishes on D-Bus, see KWinPointerSpeed.h
	std::vector<PointerDevice> ReadPointerDevices() noexcept override;

	void ApplyPointerSpeed(std::span<const PointerDevice> devices) noexcept override;

private:
	struct Connection;
	class Session;

	bool _RunScript(const std::string& script) noexcept;

	// Shared with the capture sessions, which may outlive this object
	std::shared_ptr<Connection> _connection;
	// The arrangement before the last virtual output was made
	std::vector<KWinOutputSetup::Output> _outputsBeforeVirtual;
};

}
