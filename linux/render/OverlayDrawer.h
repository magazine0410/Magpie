// The toolbar and the profiler window that are drawn over the scaled
// picture. Linux counterpart of OverlayDrawer, ImGuiImpl, ImGuiBackend and
// OverlayHelper in src/Magpie.Core.
//
// Differences from the Windows version:
// - Dear ImGui's own Vulkan back end draws; the Windows version has a back
//   end of its own for Direct3D 11.
// - The class knows nothing of windows and pointers. The caller passes in
//   where the pointer is and which buttons are down, and gets back what the
//   user asked for.
// - The texts are English. The translations of the Windows version are in a
//   format of that system; they come with the settings application.
// - There is no font cache. It exists on Windows because the glyphs of
//   Chinese, Japanese and Korean take long to rasterize.
#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>
#include "EffectDesc.h"
#include "Geometry.h"
#include "ScalingOptions.h"
#include "SmallVector.h"

struct ImFont;

namespace Magpie {

class VulkanDevice;

class OverlayDrawer {
public:
	// What stays the same while a window is scaled
	struct Config {
		// The file with the icons of the toolbar; without it the buttons
		// carry short texts
		std::filesystem::path iconFontPath;
		// How the picture of the source is obtained, for the profiler
		std::string captureMethod;
		bool isWindowedMode = false;
		bool isDeveloperMode = false;
		// The profiler shows the statistics of the duplicate frame detection
		bool showDynamicDetectionStatistics = false;
		// Whether the source can be minimized by the toolbar
		bool canMinimizeSource = true;
		// The scale factor of the monitor, by which texts and controls are
		// enlarged. Counterpart of the DPI scaling of the Windows version.
		float dpiScale = 1.0f;
	};

	// The pointer and the numbers of one frame
	struct Input {
		// The size of the scaled picture, which is the area of the overlay
		Size displaySize;
		// Where the pointer is on the picture, relative to it. Without a
		// value it is somewhere else.
		std::optional<Point> mousePos;
		// The pointer is where clicks reach the overlay. If not, the overlay
		// shows what is under the pointer but cannot be operated.
		bool canClick = false;
		// What the buttons did since the last frame, in order: 0 for the
		// left and 1 for the right button, and whether it went down. Every
		// one is passed on, so that a press and a release that arrive
		// together still make a click.
		std::span<const std::pair<int, bool>> buttonEvents;
		// Scrolled since the last frame, in notches, positive upwards
		float wheel = 0.0f;
		uint32_t fps = 0;
		// Milliseconds per pass of the frame that was just scaled; empty if
		// none was
		std::span<const float> effectTimings;
		// See DuplicateFrameDetector
		std::pair<uint32_t, uint32_t> dynamicDetectionStatistics;
	};

	// What the user asked for with the toolbar
	struct Requests {
		bool stopScaling = false;
		// From fullscreen to windowed scaling or back
		bool switchMode = false;
		bool minimizeSource = false;
		bool takeScreenshot = false;
		// With takeScreenshot: the effect whose output is wanted; without a
		// value the scaled picture
		std::optional<uint32_t> screenshotEffect;
	};

	OverlayDrawer() = default;
	OverlayDrawer(const OverlayDrawer&) = delete;
	OverlayDrawer(OverlayDrawer&&) = delete;

	// No GPU work that uses the drawer may be pending
	~OverlayDrawer();

	// `targetFormat` is the format of the images that Draw is given.
	// `overlayOptions` holds where the user put the profiler window and the
	// texts of the shortcuts; it has to stay valid and is updated.
	bool Initialize(
		VulkanDevice& device,
		VkFormat targetFormat,
		const Config& config,
		OverlayOptions& overlayOptions,
		const std::vector<const EffectDesc*>& effectDescs
	) noexcept;

	// Call when the effects of the chain have changed
	void UpdateAfterActiveEffectsChanged(const std::vector<const EffectDesc*>& effectDescs) noexcept;

	ToolbarState GetToolbarState() const noexcept;
	void SetToolbarState(ToolbarState value) noexcept;

	bool AnyVisibleWindow() const noexcept {
		return _isToolbarVisible || _isProfilerVisible;
	}

	// The effects have to be timed
	bool IsProfilerVisible() const noexcept {
		return _isProfilerVisible;
	}

	// Whether the overlay would look different now than when it was last
	// laid out, with the pointer at `mousePos` and this frame rate
	bool NeedRedraw(uint32_t fps, std::optional<Point> mousePos) const noexcept;

	// Lays the overlay out for the next frame and collects what the user
	// asked for. Does nothing if no window of the overlay is visible.
	void Update(const Input& input, Requests& requests) noexcept;

	// The pointer is over a window of the overlay, or holds one of its
	// controls: clicks have to reach the overlay and not the source
	bool WantsMouse() const noexcept {
		return _wantsMouse;
	}

	// Records the commands that draw what Update laid out. `offset` is where
	// the scaled picture is on the target. The target is in
	// VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL and is left in that layout.
	void Draw(VkCommandBuffer cmd, VkImage image, VkImageView view, Size targetSize, Point offset) noexcept;

	// The colour of every row of the profiler's timeline as an index into
	// its colour table: one per effect if there are several, then one per
	// pass for effects with several. Neighbours differ. Port of
	// OverlayHelper::GenerateTimelineColors; public for the tests.
	static SmallVector<uint32_t> GenerateTimelineColors(const std::vector<const EffectDesc*>& effectDescs) noexcept;

	// How visible the toolbar is, from 0 to 1, with the pointer this far
	// from it in pixels before DPI scaling. Port of the end of
	// OverlayDrawer::_CalcToolbarAlpha.
	static float CalcToolbarAlphaForDistance(float distance) noexcept;

private:
	struct EffectDrawInfo {
		const EffectDesc* desc = nullptr;
		std::span<const float> passTimings;
		float totalTime = 0.0f;
	};

	bool _BuildFonts() noexcept;

	void _NewFrame(const Input& input, float fittsLawAdjustment) noexcept;

	bool _DrawToolbar(const Input& input, Requests& requests, int& itemId) noexcept;

	bool _DrawProfiler(const Input& input, int& itemId) noexcept;

	bool _DrawTimingItem(int& itemId, const char* text, const uint32_t* color, float time, bool isExpanded = false) const noexcept;

	int _DrawEffectTimings(
		int& itemId,
		const EffectDrawInfo& drawInfo,
		bool showPasses,
		std::span<const uint32_t> colors,
		bool singleEffect
	) const noexcept;

	void _DrawTimelineItem(
		int& itemId,
		uint32_t color,
		std::string_view name,
		float time,
		float effectsTotalTime,
		bool selected
	) noexcept;

	void _Tooltip(const char* content, const char* description = nullptr, float maxWidth = -1.0f) noexcept;

	// Left, top, right, bottom of the window with this id, if it exists
	std::optional<std::array<float, 4>> _GetWindowRect(const char* id) const noexcept;

	const char* _GetHoveredWindowId() const noexcept;

	float _CalcToolbarAlpha(std::optional<Point> mousePos) const noexcept;

	void _ClearStatesIfNoVisibleWindow() noexcept;

	VulkanDevice* _device = nullptr;
	Config _config;
	// The back end keeps a pointer to it
	VkFormat _targetFormat = VK_FORMAT_UNDEFINED;
	OverlayOptions* _overlayOptions = nullptr;
	std::vector<const EffectDesc*> _effectDescs;
	std::string _gpuName;
	bool _isBackendInitialized = false;
	bool _hasContext = false;

	ImFont* _fontUI = nullptr;
	// Like _fontUI, but all digits have the same width
	ImFont* _fontMonoNumbers = nullptr;
	// Null without the icon font
	ImFont* _fontIcons = nullptr;

	// Position and size of the windows whose place is remembered, as they
	// were last put
	std::vector<std::pair<std::string, std::array<float, 4>>> _windowRects;
	Size _displaySize;

	SmallVector<uint32_t> _timelineColors;
	// Per pass: sum and number of the timings since the last update of the
	// display, and what is displayed
	SmallVector<std::pair<float, uint32_t>> _effectTimingsStatistics;
	SmallVector<float> _lastestAvgEffectTimings;
	std::chrono::steady_clock::time_point _lastUpdateTime;

	float _dpiScale = 1.0f;
	float _lastToolbarAlpha = 0.0f;
	uint32_t _lastFPS = 0;
	int _selectedTimelineIdx = -1;

	bool _isToolbarVisible = false;
	bool _isToolbarPinned = false;
	bool _isToolbarItemActive = false;
	bool _isProfilerVisible = false;
	bool _showPasses = false;
	bool _isFirstFrame = true;
	bool _wantsMouse = false;
	bool _hasDrawData = false;
};

}
