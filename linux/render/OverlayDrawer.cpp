#include "OverlayDrawer.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>
#include <fmt/format.h>
#include <fontconfig/fontconfig.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>
#include "Logger.h"
#include "VulkanDevice.h"

using namespace std::chrono;

namespace Magpie {

static constexpr float CORNER_ROUNDING = 6;

static constexpr const char* TOOLBAR_WINDOW_ID = "toolbar";
static constexpr const char* PROFILER_WINDOW_ID = "profiler";

// The English texts of the resource keys that the Windows overlay uses
namespace Text {
static constexpr const char* PROFILER = "Profiler";
static constexpr const char* PROFILER_CAPTURE_METHOD = "Capture method";
static constexpr const char* PROFILER_DYNAMIC_DETECTION = "Dynamic detection";
static constexpr const char* PROFILER_FRAME_RATE = "Frame rate";
static constexpr const char* PROFILER_TIMINGS = "Timings";
static constexpr const char* PROFILER_SWITCH_TO_EFFECTS = "Switch to effects";
static constexpr const char* PROFILER_SWITCH_TO_PASSES = "Switch to passes";
static constexpr const char* PROFILER_TOTAL = "Total";
static constexpr const char* DEVELOPER_OPTIONS = "Developer options";
static constexpr const char* TOOLBAR_PIN = "Pin Toolbar";
static constexpr const char* TOOLBAR_PROFILER = "Profiler Window";
static constexpr const char* TOOLBAR_TAKE_SCREENSHOT = "Take Screenshot";
static constexpr const char* TOOLBAR_TAKE_SCREENSHOT_DESCRIPTION = "Right-click to export intermediate result";
static constexpr const char* TOOLBAR_TAKE_SCREENSHOT_POPUP_TITLE = "Export intermediate result";
static constexpr const char* TOOLBAR_MINIMIZE = "Minimize";
static constexpr const char* TOOLBAR_SWITCH_TO_FULLSCREEN = "Switch to Fullscreen Scaling";
static constexpr const char* TOOLBAR_SWITCH_TO_WINDOWED = "Switch to Windowed Scaling";
static constexpr const char* TOOLBAR_CLOSE = "Stop Scaling";
static constexpr const char* TOOLBAR_CLOSE_DESCRIPTION = "Right-click to close toolbar";
}

// Code points in the icon font, and the text of a button without that font
struct Icon {
	ImWchar codePoint;
	const char* fallback;
};

namespace Icons {
static constexpr Icon CAMERA{ 0xE3C2, "Shot" };
static constexpr Icon DISMISS{ 0xE6D3, "X" };
static constexpr Icon FULL_SCREEN{ 0xE951, "Full" };
static constexpr Icon PIN{ 0xEE75, "Pin" };
static constexpr Icon PULSE{ 0xEEF7, "Prof" };
static constexpr Icon SUBTRACT{ 0xF1A3, "_" };
static constexpr Icon WINDOW{ 0xF5B7, "Win" };

// Pairs of first and last, ascending, with a zero at the end
static constexpr ImWchar RANGES[]{
	CAMERA.codePoint, CAMERA.codePoint,
	DISMISS.codePoint, DISMISS.codePoint,
	FULL_SCREEN.codePoint, FULL_SCREEN.codePoint,
	PIN.codePoint, PIN.codePoint,
	PULSE.codePoint, PULSE.codePoint,
	SUBTRACT.codePoint, SUBTRACT.codePoint,
	WINDOW.codePoint, WINDOW.codePoint,
	0
};
}

// Basic Latin, Latin-1 Supplement and Latin Extended-A
static constexpr ImWchar UI_RANGES[]{ 0x20, 0x17F, 0 };
static constexpr ImWchar NUMBER_RANGES[]{ '0', '9', 0 };
static constexpr ImWchar NOT_NUMBER_RANGES[]{ 0x20, '0' - 1, '9' + 1, 0x17F, 0 };

static constexpr ImU32 TIMELINE_COLORS[]{
	IM_COL32(229, 57, 53, 255),
	IM_COL32(156, 39, 176, 255),
	IM_COL32(63, 81, 181, 255),
	IM_COL32(30, 136, 229, 255),
	IM_COL32(0, 137, 123, 255),
	IM_COL32(121, 85, 72, 255),
	IM_COL32(117, 117, 117, 255)
};

static const char* GetWindowIDFromName(const char* name) noexcept {
	const size_t idPos = std::string_view(name).find("##");
	return idPos == std::string_view::npos ? name : name + idPos + 2;
}

// On Windows the names of effects have backslashes between directories
static std::string_view GetEffectDisplayName(const EffectDesc& effectDesc) noexcept {
	const size_t delimPos = effectDesc.name.find_last_of("\\/");
	return delimPos == std::string::npos ?
		std::string_view(effectDesc.name) : std::string_view(effectDesc.name).substr(delimPos + 1);
}

// The file and the index in it of the font that the system gives for a name
static std::pair<std::string, int> FindSystemFont(const char* family) noexcept {
	std::pair<std::string, int> result;

	FcConfig* config = FcInitLoadConfigAndFonts();
	FcPattern* pattern = FcNameParse((const FcChar8*)family);
	if (config && pattern) {
		FcConfigSubstitute(config, pattern, FcMatchPattern);
		FcDefaultSubstitute(pattern);

		FcResult matchResult;
		if (FcPattern* match = FcFontMatch(config, pattern, &matchResult)) {
			FcChar8* file = nullptr;
			if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
				result.first = (const char*)file;
				FcPatternGetInteger(match, FC_INDEX, 0, &result.second);
			}
			FcPatternDestroy(match);
		}
	}
	if (pattern) {
		FcPatternDestroy(pattern);
	}
	if (config) {
		FcConfigDestroy(config);
	}
	return result;
}

OverlayDrawer::~OverlayDrawer() {
	if (_isBackendInitialized) {
		ImGui_ImplVulkan_Shutdown();
	}
	if (_hasContext) {
		ImGui::DestroyContext();
	}
}

bool OverlayDrawer::Initialize(
	VulkanDevice& device,
	VkFormat targetFormat,
	const Config& config,
	OverlayOptions& overlayOptions,
	const std::vector<const EffectDesc*>& effectDescs
) noexcept {
	_device = &device;
	_config = config;
	_dpiScale = config.dpiScale > 0.0f ? config.dpiScale : 1.0f;
	_overlayOptions = &overlayOptions;
	_targetFormat = targetFormat;

	// The profiler starts on the right, in the middle
	if (!overlayOptions.windows.contains(PROFILER_WINDOW_ID)) {
		overlayOptions.windows.emplace(PROFILER_WINDOW_ID, OverlayWindowOption{
			.hArea = 2,
			.vArea = 1,
			.hPos = 60.0f,
			.vPos = 0.5f
		});
	}

	if (!IMGUI_CHECKVERSION()) {
		Logger::Get().Error("The headers and the library of Dear ImGui are of different versions");
		return false;
	}

	ImGui::CreateContext();
	_hasContext = true;

	ImGuiIO& io = ImGui::GetIO();
	io.BackendPlatformName = "Magpie";
	io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
	// No ini file
	io.IniFilename = nullptr;
	io.ConfigDebugHighlightIdConflicts = false;

	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.Instance = device.Instance();
	initInfo.PhysicalDevice = device.PhysicalDevice();
	initInfo.Device = device.Device();
	initInfo.QueueFamily = device.QueueFamily();
	initInfo.Queue = device.Queue();
	// The back end makes a pool of its own for the font texture
	initInfo.DescriptorPoolSize = 8;
	initInfo.MinImageCount = 2;
	initInfo.ImageCount = 2;
	initInfo.UseDynamicRendering = true;
	initInfo.PipelineRenderingCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &_targetFormat
	};
	if (!ImGui_ImplVulkan_Init(&initInfo)) {
		Logger::Get().Error("ImGui_ImplVulkan_Init failed");
		return false;
	}
	_isBackendInitialized = true;

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.PopupRounding = style.WindowRounding = CORNER_ROUNDING;
	// Rendering is on demand, so tool tips appear without a delay
	style.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_DelayNone;
	style.FrameBorderSize = 1;
	style.FrameRounding = 2;
	style.WindowMinSize = ImVec2(10, 10);
	style.ScaleAllSizes(_dpiScale);

	if (!_BuildFonts()) {
		Logger::Get().Error("The fonts of the overlay could not be built");
		return false;
	}
	io.FontDefault = _fontUI;

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(device.PhysicalDevice(), &properties);
	_gpuName = properties.deviceName;

	UpdateAfterActiveEffectsChanged(effectDescs);
	return true;
}

bool OverlayDrawer::_BuildFonts() noexcept {
	ImFontAtlas& fontAtlas = *ImGui::GetIO().Fonts;
	fontAtlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight | ImFontAtlasFlags_NoMouseCursors;

	// What the system uses for text without serifs, the counterpart of
	// Segoe UI on Windows
	const auto [uiFontPath, uiFontIndex] = FindSystemFont("sans-serif");
	if (uiFontPath.empty()) {
		Logger::Get().Error("fontconfig found no font");
		return false;
	}
	Logger::Get().Info("The overlay uses the font " + uiFontPath);

	const float fontSize = 18 * _dpiScale;

	const auto addUIFonts = [&](float digitAdvance) {
		ImFontConfig config;
		config.FontNo = uiFontIndex;
		_fontUI = fontAtlas.AddFontFromFileTTF(uiFontPath.c_str(), fontSize, &config, UI_RANGES);
		if (!_fontUI) {
			return false;
		}

		// The same font with digits of one width, so that numbers that
		// change do not move what is next to them
		config.GlyphMinAdvanceX = config.GlyphMaxAdvanceX = digitAdvance;
		_fontMonoNumbers = fontAtlas.AddFontFromFileTTF(uiFontPath.c_str(), fontSize, &config, NUMBER_RANGES);
		config.MergeMode = true;
		config.GlyphMinAdvanceX = 0;
		config.GlyphMaxAdvanceX = FLT_MAX;
		return _fontMonoNumbers &&
			fontAtlas.AddFontFromFileTTF(uiFontPath.c_str(), fontSize, &config, NOT_NUMBER_RANGES);
	};

	// The Windows version knows the width of the digits of its font. Here
	// the font is whatever the system has, so its widest digit is measured
	// with a first build.
	if (!addUIFonts(0.0f) || !fontAtlas.Build()) {
		return false;
	}
	float digitAdvance = 0.0f;
	for (ImWchar c = '0'; c <= '9'; ++c) {
		digitAdvance = std::max(digitAdvance, _fontUI->GetCharAdvance(c));
	}

	fontAtlas.Clear();
	if (!addUIFonts(digitAdvance)) {
		return false;
	}

	_fontIcons = nullptr;
	std::error_code ec;
	if (!_config.iconFontPath.empty() && std::filesystem::exists(_config.iconFontPath, ec)) {
		ImFontConfig config;
		_fontIcons = fontAtlas.AddFontFromFileTTF(
			_config.iconFontPath.c_str(), 18 * _dpiScale, &config, Icons::RANGES);
	}
	if (!_fontIcons) {
		Logger::Get().Warn("The icon font of the toolbar was not found; its buttons carry text");
	}

	return fontAtlas.Build();
}

void OverlayDrawer::UpdateAfterActiveEffectsChanged(const std::vector<const EffectDesc*>& effectDescs) noexcept {
	_effectDescs = effectDescs;
	_timelineColors = GenerateTimelineColors(effectDescs);

	uint32_t passCount = 0;
	for (const EffectDesc* desc : effectDescs) {
		passCount += (uint32_t)desc->passes.size();
	}

	// The old timings are void
	_effectTimingsStatistics.clear();
	_effectTimingsStatistics.resize(passCount);
	_lastestAvgEffectTimings.clear();
	_lastestAvgEffectTimings.resize(passCount);
	_lastUpdateTime = {};
}

ToolbarState OverlayDrawer::GetToolbarState() const noexcept {
	if (!_isToolbarVisible) {
		return ToolbarState::Off;
	}
	return _isToolbarPinned ? ToolbarState::AlwaysShow : ToolbarState::AutoHide;
}

void OverlayDrawer::SetToolbarState(ToolbarState value) noexcept {
	if (GetToolbarState() == value) {
		return;
	}

	if (value == ToolbarState::Off) {
		_isToolbarVisible = false;
		_ClearStatesIfNoVisibleWindow();
	} else {
		_isToolbarVisible = true;
		_isToolbarPinned = value == ToolbarState::AlwaysShow;
	}
}

bool OverlayDrawer::NeedRedraw(uint32_t fps, std::optional<Point> mousePos) const noexcept {
	if (!AnyVisibleWindow()) {
		// Once more, without the overlay
		return _hasDrawData;
	}

	if (_CalcToolbarAlpha(mousePos) != _lastToolbarAlpha) {
		return true;
	}

	return _lastFPS != fps && (_lastToolbarAlpha > 1e-5f || _isProfilerVisible);
}

void OverlayDrawer::Update(const Input& input, Requests& requests) noexcept {
	// Without a visible window ImGui is not run at all
	if (!AnyVisibleWindow()) {
		_hasDrawData = false;
		_wantsMouse = false;
		return;
	}

	_lastFPS = input.fps;

	ImGuiIO& io = ImGui::GetIO();
	if (input.canClick) {
		for (const auto& [button, isDown] : input.buttonEvents) {
			io.AddMouseButtonEvent(button, isDown);
		}
		if (input.wheel != 0.0f) {
			io.AddMouseWheelEvent(0.0f, input.wheel);
		}
	} else {
		io.AddMouseButtonEvent(0, false);
		io.AddMouseButtonEvent(1, false);
	}

	// ImGui lays out at least twice, or the layout is often wrong. It takes
	// one change of a button per frame, so there is a frame for each.
	int count = std::max(2, (int)input.buttonEvents.size() + 1);
	if (_isFirstFrame) {
		// When it has just been shown it takes one more
		_isFirstFrame = false;
		++count;
	}

	const bool wasToolbarVisible = _isToolbarVisible;
	ImGui_ImplVulkan_NewFrame();

	// Intermediate states are not to be shown, so several frames are laid
	// out, but ten at most
	for (int i = 0; i < 10; ++i) {
		// By Fitts's law: with the pointer on the toolbar its logical
		// position is a little lower, so that the buttons can be hit at the
		// very top edge
		float fittsLawAdjustment = 0;
		const char* hoveredWindowId = _GetHoveredWindowId();
		if (hoveredWindowId && hoveredWindowId == std::string_view(TOOLBAR_WINDOW_ID)) {
			fittsLawAdjustment = 4 * _dpiScale;
		}

		_NewFrame(input, fittsLawAdjustment);

		bool needRedraw = false;
		// Against clashes of ids
		int itemId = 0;

		if (wasToolbarVisible && _DrawToolbar(input, requests, itemId)) {
			needRedraw = true;
		}
		if (_isProfilerVisible && _DrawProfiler(input, itemId)) {
			needRedraw = true;
		}
		if (needRedraw) {
			++count;
		}

		ImGui::EndFrame();

		if (--count == 0) {
			break;
		}
	}

	ImGui::Render();
	_hasDrawData = true;
	_wantsMouse = io.WantCaptureMouse;

	_ClearStatesIfNoVisibleWindow();
}

// Port of ImGuiImpl::NewFrame
void OverlayDrawer::_NewFrame(const Input& input, float fittsLawAdjustment) noexcept {
	ImGuiIO& io = ImGui::GetIO();

	if (input.displaySize != _displaySize) {
		_displaySize = input.displaySize;
		io.DisplaySize = ImVec2((float)input.displaySize.width, (float)input.displaySize.height);
		// When the picture changes its size the windows of the overlay are
		// put anew
		_windowRects.clear();
	}

	if (input.mousePos) {
		float y = (float)input.mousePos->y;
		// The logical position is lower, so that the buttons of the toolbar
		// can be hit at the top edge
		if (y >= 0 && y < fittsLawAdjustment) {
			y = fittsLawAdjustment;
		}
		io.AddMousePosEvent((float)input.mousePos->x, y);
	} else {
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	}

	// No keyboard input
	if (io.WantCaptureKeyboard) {
		io.AddKeyEvent(ImGuiKey_Enter, true);
		io.AddKeyEvent(ImGuiKey_Enter, false);
	}

	ImGui::NewFrame();

	std::map<std::string, OverlayWindowOption>& windowOptions = _overlayOptions->windows;
	const auto clampToDisplay = [&](ImVec2& pos, const ImVec2& size) {
		pos.x = io.DisplaySize.x > size.x ? std::clamp(pos.x, 0.0f, io.DisplaySize.x - size.x) : 0.0f;
		pos.y = io.DisplaySize.y > size.y ? std::clamp(pos.y, 0.0f, io.DisplaySize.y - size.y) : 0.0f;
	};

	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
		if (window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoMove)) {
			continue;
		}
		// Not Debug##Default and not windows that are still being set up
		if (window->IsFallbackWindow || window->Appearing) {
			continue;
		}

		ImVec2 pos = window->Pos;
		// The window stays inside the picture
		clampToDisplay(pos, window->Size);

		const char* windowId = GetWindowIDFromName(window->Name);
		const auto optionIt = windowOptions.find(windowId);
		if (optionIt == windowOptions.end()) {
			ImGui::SetWindowPos(window, pos);
			continue;
		}

		OverlayWindowOption& option = optionIt->second;
		const std::array<float, 4> rect{ pos.x, pos.y, window->Size.x, window->Size.y };
		const auto rectIt = std::ranges::find(_windowRects, windowId,
			&std::pair<std::string, std::array<float, 4>>::first);
		if (rectIt == _windowRects.end()) {
			// Shown for the first time, or the picture changed its size: the
			// window is put by its rule
			if (option.hArea == 0) {
				pos.x = option.hPos * _dpiScale;
			} else if (option.hArea == 1) {
				pos.x = io.DisplaySize.x * option.hPos - window->Size.x / 2;
			} else {
				pos.x = io.DisplaySize.x - option.hPos * _dpiScale - window->Size.x;
			}

			if (option.vArea == 0) {
				pos.y = option.vPos * _dpiScale;
			} else if (option.vArea == 1) {
				pos.y = io.DisplaySize.y * option.vPos - window->Size.y / 2;
			} else {
				pos.y = io.DisplaySize.y - option.vPos * _dpiScale - window->Size.y;
			}

			clampToDisplay(pos, window->Size);
		} else if (rectIt->second != rect) {
			// The user moved or resized the window: decide anew which edges
			// it keeps its distance to, by the ratio of the margins on both
			// sides. The thresholds depend on the size of the window
			// relative to the picture.
			const float thresholdX = std::max(window->Size.x / io.DisplaySize.x, 0.2f);
			const float thresholdY = std::max(window->Size.y / io.DisplaySize.y, 0.2f);

			float ratio = pos.x / (io.DisplaySize.x - pos.x - window->Size.x);
			if (ratio < thresholdX) {
				option.hArea = 0;
				option.hPos = pos.x / _dpiScale;
			} else if (ratio <= 1 / thresholdX) {
				option.hArea = 1;
				option.hPos = (pos.x + window->Size.x / 2) / io.DisplaySize.x;
			} else {
				option.hArea = 2;
				option.hPos = (io.DisplaySize.x - pos.x - window->Size.x) / _dpiScale;
			}

			ratio = pos.y / (io.DisplaySize.y - pos.y - window->Size.y);
			if (ratio < thresholdY) {
				option.vArea = 0;
				option.vPos = pos.y / _dpiScale;
			} else if (ratio <= 1 / thresholdY) {
				option.vArea = 1;
				option.vPos = (pos.y + window->Size.y / 2) / io.DisplaySize.y;
			} else {
				option.vArea = 2;
				option.vPos = (io.DisplaySize.y - pos.y - window->Size.y) / _dpiScale;
			}
		}

		ImGui::SetWindowPos(window, pos);

		// Where the window is now
		const std::array<float, 4> newRect{ window->Pos.x, window->Pos.y, window->Size.x, window->Size.y };
		if (rectIt == _windowRects.end()) {
			_windowRects.emplace_back(windowId, newRect);
		} else {
			rectIt->second = newRect;
		}
	}
}

void OverlayDrawer::Draw(VkCommandBuffer cmd, VkImage image, VkImageView view, Size targetSize, Point offset) noexcept {
	if (!_hasDrawData) {
		return;
	}
	ImDrawData* drawData = ImGui::GetDrawData();
	if (!drawData || drawData->CmdListsCount == 0) {
		return;
	}

	static constexpr VkImageSubresourceRange COLOR_RANGE{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = COLOR_RANGE
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);

	const VkRenderingAttachmentInfo attachment{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE
	};
	const VkRenderingInfo renderingInfo{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { { 0, 0 }, { (uint32_t)targetSize.width, (uint32_t)targetSize.height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &attachment
	};
	vkCmdBeginRendering(cmd, &renderingInfo);

	// The back end takes the display area of the draw data to be the whole
	// target. The overlay was laid out for the picture, which is at
	// `offset` of this target; shifting the origin puts it there.
	const ImVec2 displayPos = drawData->DisplayPos;
	const ImVec2 displaySize = drawData->DisplaySize;
	drawData->DisplayPos = ImVec2(-(float)offset.x, -(float)offset.y);
	drawData->DisplaySize = ImVec2((float)targetSize.width, (float)targetSize.height);
	ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
	drawData->DisplayPos = displayPos;
	drawData->DisplaySize = displaySize;

	vkCmdEndRendering(cmd);

	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Port of ImGuiImpl::Tooltip
void OverlayDrawer::_Tooltip(const char* content, const char* description, float maxWidth) noexcept {
	static constexpr float DESCRIPTION_SCALE = 0.9f;

	const ImVec2 padding = ImGui::GetStyle().WindowPadding;
	const ImVec2 contentSize = ImGui::CalcTextSize(content, nullptr, false, maxWidth - 2 * padding.x);
	ImVec2 descriptionSize{};
	if (description) {
		const float oldFontScale = ImGui::GetIO().FontGlobalScale;
		ImGui::GetIO().FontGlobalScale *= DESCRIPTION_SCALE;
		ImGui::PushFont(ImGui::GetFont());
		descriptionSize = ImGui::CalcTextSize(description, nullptr, false, maxWidth - 2 * padding.x);
		ImGui::GetIO().FontGlobalScale = oldFontScale;
		ImGui::PopFont();
	}

	// A little higher, or the lower border is narrower than the upper one
	const ImVec2 windowSize(
		std::max(contentSize.x, descriptionSize.x) + 2 * padding.x,
		contentSize.y + descriptionSize.y + 2 * padding.y + 1.5f * _dpiScale
	);
	ImGui::SetNextWindowSize(windowSize);

	ImVec2 windowPos = ImGui::GetMousePos();
	windowPos.x += 16.0f * _dpiScale * ImGui::GetStyle().MouseCursorScale;
	windowPos.y += 8.0f * _dpiScale * ImGui::GetStyle().MouseCursorScale;

	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	windowPos.x = std::clamp(windowPos.x, 0.0f, std::max(displaySize.x - windowSize.x, 0.0f));
	windowPos.y = std::clamp(windowPos.y, 0.0f, std::max(displaySize.y - windowSize.y, 0.0f));
	ImGui::SetNextWindowPos(windowPos);

	ImGui::SetNextWindowBgAlpha(ImGui::GetStyle().Colors[ImGuiCol_PopupBg].w);
	ImGui::Begin("tooltip", nullptr,
		ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoFocusOnAppearing);

	ImGui::PushTextWrapPos(maxWidth - padding.x);
	ImGui::TextUnformatted(content);
	if (description) {
		ImGui::PushStyleColor(ImGuiCol_Text, { 1.0f, 1.0f, 1.0f, 0.8f });
		const float oldFontScale = ImGui::GetIO().FontGlobalScale;
		ImGui::GetIO().FontGlobalScale *= DESCRIPTION_SCALE;
		ImGui::PushFont(ImGui::GetFont());
		ImGui::TextUnformatted(description);
		ImGui::GetIO().FontGlobalScale = oldFontScale;
		ImGui::PopFont();
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();

	ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
	ImGui::End();
}

std::optional<std::array<float, 4>> OverlayDrawer::_GetWindowRect(const char* id) const noexcept {
	const std::string suffix = std::string("##") + id;
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
		if (std::string_view(window->Name).ends_with(suffix)) {
			return std::array<float, 4>{
				window->Pos.x, window->Pos.y, window->Pos.x + window->Size.x, window->Pos.y + window->Size.y
			};
		}
	}
	return std::nullopt;
}

const char* OverlayDrawer::_GetHoveredWindowId() const noexcept {
	const ImGuiWindow* hoveredWindow = ImGui::GetCurrentContext()->HoveredWindow;
	return hoveredWindow ? GetWindowIDFromName(hoveredWindow->Name) : nullptr;
}

float OverlayDrawer::CalcToolbarAlphaForDistance(float distance) noexcept {
	return (40.0f - std::clamp(distance - 10.0f, 0.0f, 40.0f)) / 40.0f;
}

float OverlayDrawer::_CalcToolbarAlpha(std::optional<Point> mousePos) const noexcept {
	// Not hidden while a button of it holds the pointer
	if (_isToolbarPinned || _isToolbarItemActive) {
		return 1.0f;
	}

	std::optional<std::array<float, 4>> windowRect = _GetWindowRect(TOOLBAR_WINDOW_ID);
	if (!windowRect || !mousePos) {
		return 0.0f;
	}
	auto& [left, top, right, bottom] = *windowRect;
	// A part at the top is outside the picture, to cut the round corners off
	top = 0.0f;

	const float cursorX = (float)mousePos->x;
	const float cursorY = (float)mousePos->y;

	// The shortest distance to an edge or corner
	const float dx = cursorX < left ? left - cursorX : (cursorX > right ? cursorX - right : 0.0f);
	const float dy = cursorY < top ? top - cursorY : (cursorY > bottom ? cursorY - bottom : 0.0f);
	return CalcToolbarAlphaForDistance(std::hypot(dx, dy) / _dpiScale);
}

// Port of ImGuiImpl::ClearStates
void OverlayDrawer::_ClearStatesIfNoVisibleWindow() noexcept {
	if (AnyVisibleWindow()) {
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	io.AddMouseButtonEvent(0, false);
	io.AddMouseButtonEvent(1, false);

	// Brings the state up to date
	ImGui::NewFrame();
	ImGui::EndFrame();
	if (io.WantCaptureMouse) {
		// After a drag it takes two frames to reset WantCaptureMouse
		ImGui::NewFrame();
		ImGui::EndFrame();
	}

	_wantsMouse = false;
	_isToolbarItemActive = false;
	_isFirstFrame = true;
}

bool OverlayDrawer::_DrawToolbar(const Input& input, Requests& requests, int& itemId) noexcept {
	bool needRedraw = false;

	const float windowWidth = 360 * _dpiScale;
	ImGui::SetNextWindowSize({ windowWidth, (CORNER_ROUNDING + 31) * _dpiScale });
	ImGui::SetNextWindowPos(
		ImVec2((ImGui::GetIO().DisplaySize.x - windowWidth) / 2, -CORNER_ROUNDING * _dpiScale));

	_lastToolbarAlpha = _CalcToolbarAlpha(input.mousePos);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, _lastToolbarAlpha);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImU32)ImColor(15, 15, 15, 180));
	const ImVec2 originalWindowPadding = ImGui::GetStyle().WindowPadding;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 6 * _dpiScale, 0.0f });

	_isToolbarItemActive = false;

	if (ImGui::Begin((std::string("##") + TOOLBAR_WINDOW_ID).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::SetCursorPosY((CORNER_ROUNDING + 3) * _dpiScale);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, originalWindowPadding);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 4 * _dpiScale, 4 * _dpiScale });
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * _dpiScale);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 4 * _dpiScale, 0.0f });
		// Disabling only blocks interaction; it should not look different
		ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0.118f, 0.533f, 0.894f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0.118f, 0.533f, 0.894f, 0.8f });

		// Without the icon font the buttons carry text and all have the
		// width of the longest one
		const float textButtonWidth = ImGui::CalcTextSize("Shot").x + 8 * _dpiScale;

		// The label of a button: its icon, or text without the icon font
		const auto iconButton = [&](const Icon& icon) {
			if (!_fontIcons) {
				return ImGui::Button(icon.fallback, ImVec2(textButtonWidth, 0));
			}

			char label[8]{};
			ImTextCharToUtf8(label, icon.codePoint);
			ImGui::PushFont(_fontIcons);
			const bool clicked = ImGui::Button(label);
			ImGui::PopFont();
			return clicked;
		};

		const auto drawToggleButton = [&](bool& value, const Icon& icon, const char* tooltip) {
			const bool stylePushed = value;
			if (stylePushed) {
				ImGui::PushStyleColor(ImGuiCol_Button, { 0.118f, 0.533f, 0.894f, 0.8f });
			}

			if (iconButton(icon)) {
				value = !value;
				needRedraw = true;
			}
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				_isToolbarItemActive = true;
			}
			if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
				_Tooltip(tooltip);
			}

			if (stylePushed) {
				ImGui::PopStyleColor();
			}
		};

		const auto drawButton = [&](const Icon& icon, const char* tooltip, const char* description = nullptr) {
			const bool clicked = iconButton(icon);
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				_isToolbarItemActive = true;
			}
			if (ImGui::IsItemHovered()) {
				_Tooltip(tooltip, description);
			}
			return clicked;
		};

		// With the name of the shortcut, if it has one
		const auto withShortcut = [](const char* text, const std::string& shortcut) {
			return shortcut.empty() ? std::string(text) : fmt::format("{} ({})", text, shortcut);
		};

		// No interaction while the pointer is not on the picture
		ImGui::BeginDisabled(!input.mousePos);

		drawToggleButton(_isToolbarPinned, Icons::PIN, Text::TOOLBAR_PIN);
		ImGui::SameLine();
		drawToggleButton(_isProfilerVisible, Icons::PULSE, Text::TOOLBAR_PROFILER);
		ImGui::SameLine();

		if (drawButton(Icons::CAMERA,
			withShortcut(Text::TOOLBAR_TAKE_SCREENSHOT, _overlayOptions->takeScreenshotShortcut).c_str(),
			Text::TOOLBAR_TAKE_SCREENSHOT_DESCRIPTION)) {
			requests.takeScreenshot = true;
			requests.screenshotEffect.reset();
		}
		// The context menu of the screenshot button: the output of one of
		// the effects. The Windows version also offers the outputs of the
		// passes in developer mode; those are not available here.
		if (ImGui::BeginPopupContextItem()) {
			_isToolbarItemActive = true;

			ImGui::SeparatorText(Text::TOOLBAR_TAKE_SCREENSHOT_POPUP_TITLE);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f * _dpiScale);
			ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 6.0f * _dpiScale);

			for (uint32_t i = 0; i < (uint32_t)_effectDescs.size(); ++i) {
				ImGui::PushID(itemId++);
				if (ImGui::MenuItem(std::string(GetEffectDisplayName(*_effectDescs[i])).c_str())) {
					requests.takeScreenshot = true;
					requests.screenshotEffect = i;
				}
				ImGui::PopID();
			}

			ImGui::PopStyleVar();
			ImGui::EndPopup();
		}

		// The frame rate in the middle
		ImGui::SameLine();
		const std::string fpsText = fmt::format("{} FPS", input.fps);
		ImGui::SetCursorPosX((ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(fpsText.c_str()).x) / 2);
		ImGui::SetCursorPosY((CORNER_ROUNDING + 1) * _dpiScale);
		ImGui::PushFont(_fontMonoNumbers);
		ImGui::TextUnformatted(fpsText.c_str());
		ImGui::PopFont();

		ImGui::SameLine();
		ImGui::SetCursorPosY((CORNER_ROUNDING + 3) * _dpiScale);

		// The buttons on the right are placed by their width with the
		// spacing, which is 28 for an icon
		const float buttonWidth = _fontIcons ? 28 * _dpiScale : textButtonWidth + 4 * _dpiScale;
		const int buttonCount = _config.canMinimizeSource ? 3 : 2;
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttonCount * buttonWidth + 4 * _dpiScale);

		if (_config.canMinimizeSource) {
			if (drawButton(Icons::SUBTRACT, Text::TOOLBAR_MINIMIZE)) {
				requests.minimizeSource = true;
			}
			ImGui::SameLine();
		}

		if (drawButton(_config.isWindowedMode ? Icons::FULL_SCREEN : Icons::WINDOW,
			withShortcut(
				_config.isWindowedMode ? Text::TOOLBAR_SWITCH_TO_FULLSCREEN : Text::TOOLBAR_SWITCH_TO_WINDOWED,
				_config.isWindowedMode ? _overlayOptions->scaleShortcut : _overlayOptions->windowedModeScaleShortcut
			).c_str())) {
			requests.switchMode = true;
		}
		ImGui::SameLine();

		// As the close button of the main window on Windows (#C42B1C)
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0.769f, 0.169f, 0.11f, 1.0f });
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0.769f, 0.169f, 0.11f, 0.8f });

		if (drawButton(Icons::DISMISS,
			withShortcut(Text::TOOLBAR_CLOSE,
				_config.isWindowedMode ? _overlayOptions->windowedModeScaleShortcut : _overlayOptions->scaleShortcut).c_str(),
			Text::TOOLBAR_CLOSE_DESCRIPTION)) {
			requests.stopScaling = true;
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			// Takes effect after this frame
			_isToolbarVisible = false;
		}

		ImGui::EndDisabled();

		ImGui::PopStyleColor(5);
		ImGui::PopStyleVar(6);
	}
	ImGui::End();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);

	return needRedraw;
}

bool OverlayDrawer::_DrawTimingItem(
	int& itemId,
	const char* text,
	const uint32_t* color,
	float time,
	bool isExpanded
) const noexcept {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	const std::string timeStr = fmt::format("{:.3f} ms", time);
	const float timeWidth = _fontMonoNumbers->CalcTextSizeA(
		ImGui::GetFontSize(), FLT_MAX, 0.0f, timeStr.c_str()).x;

	// The Windows version draws the character U+25A0 in the colour. A font
	// of the system need not have it, so a square is drawn here.
	const float fontHeight = ImGui::GetFont()->FontSize;
	const float indicatorSize = std::round(fontHeight * 0.6f);

	static constexpr float spacingBeforeText = 3;
	static constexpr float spacingAfterText = 8;
	const float descWrapPos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - timeWidth - spacingAfterText;
	const float descHeight = ImGui::CalcTextSize(
		text, nullptr, false,
		descWrapPos - ImGui::GetCursorPosX() - (color ? indicatorSize + spacingBeforeText : 0)).y;

	ImGui::PushID(itemId++);

	bool isHovered = false;
	if (color) {
		ImGui::Selectable("", false, 0, ImVec2(0, descHeight));
		if (ImGui::IsItemHovered()) {
			isHovered = true;
		}
		ImGui::SameLine(0, 0);

		// In the middle of the lines of the description
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const ImVec2 min(cursor.x, cursor.y + (descHeight - indicatorSize) / 2);
		ImGui::GetWindowDrawList()->AddRectFilled(
			min, ImVec2(min.x + indicatorSize, min.y + indicatorSize), *color);
		ImGui::Dummy(ImVec2(indicatorSize, descHeight));
		ImGui::SameLine(0, spacingBeforeText);
	}

	ImGui::PushTextWrapPos(descWrapPos);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::SameLine(0, 0);

	// With a description of several lines the time is in the middle
	if (color && descHeight >= fontHeight * 2) {
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (descHeight - fontHeight) / 2);
	}

	if (isExpanded) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.5f));
	}

	ImGui::PushFont(_fontMonoNumbers);
	ImGui::SetCursorPosX(descWrapPos + spacingAfterText);
	ImGui::TextUnformatted(timeStr.c_str());
	ImGui::PopFont();

	if (isExpanded) {
		ImGui::PopStyleColor();
	}

	ImGui::PopID();
	return isHovered;
}

// Returns the number of the item under the pointer, or -1
int OverlayDrawer::_DrawEffectTimings(
	int& itemId,
	const EffectDrawInfo& drawInfo,
	bool showPasses,
	std::span<const uint32_t> colors,
	bool singleEffect
) const noexcept {
	int result = -1;

	showPasses &= drawInfo.passTimings.size() > 1;

	std::string effectName(GetEffectDisplayName(*drawInfo.desc));
	if (_config.isDeveloperMode && (drawInfo.desc->flags & EffectFlags::FP16)) {
		// With the developer options on, whether the effect uses FP16
		effectName += " (FP16)";
	}

	if (_DrawTimingItem(
		itemId,
		effectName.c_str(),
		(!singleEffect && !showPasses) ? &colors[0] : nullptr,
		drawInfo.totalTime,
		showPasses
	)) {
		result = 0;
	}

	if (showPasses) {
		for (size_t j = 0; j < drawInfo.passTimings.size(); ++j) {
			ImGui::Indent(16);

			if (_DrawTimingItem(itemId, drawInfo.desc->passes[j].desc.c_str(), &colors[j], drawInfo.passTimings[j])) {
				result = (int)j;
			}

			ImGui::Unindent(16);
		}
	}

	return result;
}

void OverlayDrawer::_DrawTimelineItem(
	int& itemId,
	uint32_t color,
	std::string_view name,
	float time,
	float effectsTotalTime,
	bool selected
) noexcept {
	ImGui::PushID(itemId++);

	ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, color);

	ImGui::PushStyleColor(ImGuiCol_HeaderActive, color);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color);
	ImGui::PushStyleColor(ImGuiCol_Header, color);
	ImGui::Selectable("", selected);
	ImGui::PopStyleColor(3);

	if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
		const std::string content = fmt::format("{}\n{:.3f} ms\n{}%", name, time,
			std::lround(time / effectsTotalTime * 100));
		ImGui::PushFont(_fontMonoNumbers);
		_Tooltip(content.c_str(), nullptr, 500 * _dpiScale);
		ImGui::PopFont();
	}

	// The text if there is room for it
	std::string text;
	if (selected) {
		text = fmt::format("{}%", std::lround(time / effectsTotalTime * 100));
	} else {
		text.assign(name);
	}

	const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
	const float itemWidth = ImGui::GetItemRectSize().x;
	const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
	if (itemWidth - (selected ? 0 : itemSpacing) > textWidth + 4 * _dpiScale) {
		ImGui::SameLine(0, 0);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (itemWidth - textWidth - itemSpacing) / 2);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 0.5f * _dpiScale);
		ImGui::TextUnformatted(text.c_str());
	}

	ImGui::PopID();
}

bool OverlayDrawer::_DrawProfiler(const Input& input, int& itemId) noexcept {
	const uint32_t passCount = (uint32_t)_effectTimingsStatistics.size();
	const std::span<const float> effectTimings = input.effectTimings;

	bool needRedraw = false;

	// Empty: no new frame was scaled
	if (effectTimings.size() >= passCount && passCount > 0) {
		const steady_clock::time_point now = steady_clock::now();
		if (_lastUpdateTime == steady_clock::time_point{}) {
			// The first frame
			_lastUpdateTime = now;

			for (uint32_t i = 0; i < passCount; ++i) {
				_lastestAvgEffectTimings[i] = effectTimings[i];
			}
		} else {
			if (now - _lastUpdateTime > 500ms) {
				// Not more often than every 500 ms
				_lastUpdateTime = now;

				for (uint32_t i = 0; i < passCount; ++i) {
					auto& [total, count] = _effectTimingsStatistics[i];
					if (count > 0) {
						_lastestAvgEffectTimings[i] = total / (float)count;
					}

					count = 0;
					total = 0;
				}
			}

			for (uint32_t i = 0; i < passCount; ++i) {
				auto& [total, count] = _effectTimingsStatistics[i];
				// An effect that was skipped has a time of zero and does not count
				if (effectTimings[i] > 1e-3) {
					++count;
					total += effectTimings[i];
				}
			}
		}
	}

	{
		const float windowWidth = 310 * _dpiScale;
		ImGui::SetNextWindowSizeConstraints(ImVec2(windowWidth, 0.0f), ImVec2(windowWidth, 500 * _dpiScale));
	}

	const std::string profilerStr = fmt::format("{}##{}", Text::PROFILER, PROFILER_WINDOW_ID);
	if (!ImGui::Begin(profilerStr.c_str(), &_isProfilerVisible, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return needRedraw;
	}

	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted(("GPU: " + _gpuName).c_str());
	ImGui::TextUnformatted(fmt::format("{}: {}", Text::PROFILER_CAPTURE_METHOD, _config.captureMethod).c_str());
	if (_config.showDynamicDetectionStatistics) {
		const std::pair<uint32_t, uint32_t> statistics = input.dynamicDetectionStatistics;
		ImGui::TextUnformatted(fmt::format("{}: ", Text::PROFILER_DYNAMIC_DETECTION).c_str());
		ImGui::SameLine(0, 0);
		ImGui::PushFont(_fontMonoNumbers);
		ImGui::TextUnformatted(fmt::format("{}/{} ({:.1f}%)", statistics.first, statistics.second,
			statistics.second == 0 ? 0.0f : (float)statistics.first * 100.0f / (float)statistics.second).c_str());
		ImGui::PopFont();
	}
	ImGui::TextUnformatted(fmt::format("{}: {} FPS", Text::PROFILER_FRAME_RATE, input.fps).c_str());
	ImGui::PopTextWrapPos();

	const uint32_t nEffect = (uint32_t)_effectDescs.size();

	SmallVector<EffectDrawInfo, 4> effectDrawInfos(_effectDescs.size());
	{
		uint32_t idx = 0;
		for (uint32_t i = 0; i < nEffect; ++i) {
			EffectDrawInfo& drawInfo = effectDrawInfos[i];
			drawInfo.desc = _effectDescs[i];

			const uint32_t nPass = (uint32_t)drawInfo.desc->passes.size();
			drawInfo.passTimings = { _lastestAvgEffectTimings.begin() + idx, nPass };
			idx += nPass;

			for (const float t : drawInfo.passTimings) {
				drawInfo.totalTime += t;
			}
		}
	}

	bool& showPasses = _showPasses;
	if (_config.isDeveloperMode) {
		ImGui::Spacing();
		if (ImGui::CollapsingHeader(Text::DEVELOPER_OPTIONS, ImGuiTreeNodeFlags_DefaultOpen)) {
			// A button to switch if some effect has several passes
			const bool showSwitchButton = std::ranges::any_of(effectDrawInfos, [](const EffectDrawInfo& drawInfo) {
				return drawInfo.passTimings.size() > 1;
			});

			if (showSwitchButton) {
				if (ImGui::Button(showPasses ? Text::PROFILER_SWITCH_TO_EFFECTS : Text::PROFILER_SWITCH_TO_PASSES)) {
					showPasses = !showPasses;
					// Once more for the change of layout that a scroll bar causes
					needRedraw = true;
				}
			} else {
				showPasses = false;
			}
		}
	} else {
		showPasses = false;
	}

	ImGui::Spacing();
	// How long the effects take
	if (ImGui::CollapsingHeader(Text::PROFILER_TIMINGS, ImGuiTreeNodeFlags_DefaultOpen)) {
		float effectsTotalTime = 0.0f;
		for (const EffectDrawInfo& drawInfo : effectDrawInfos) {
			effectsTotalTime += drawInfo.totalTime;
		}

		SmallVector<uint32_t, 4> colors;
		colors.reserve(_timelineColors.size());
		if (nEffect == 1) {
			colors.resize(_timelineColors.size());
			for (size_t i = 0; i < _timelineColors.size(); ++i) {
				colors[i] = TIMELINE_COLORS[_timelineColors[i]];
			}
		} else if (showPasses) {
			uint32_t i = 0;
			for (const EffectDrawInfo& drawInfo : effectDrawInfos) {
				if (drawInfo.passTimings.size() == 1) {
					colors.push_back(TIMELINE_COLORS[_timelineColors[i]]);
					++i;
					continue;
				}

				++i;
				for (uint32_t j = 0; j < drawInfo.passTimings.size(); ++j) {
					colors.push_back(TIMELINE_COLORS[_timelineColors[i]]);
					++i;
				}
			}
		} else {
			size_t i = 0;
			for (const EffectDrawInfo& drawInfo : effectDrawInfos) {
				colors.push_back(TIMELINE_COLORS[_timelineColors[i]]);

				++i;
				if (drawInfo.passTimings.size() > 1) {
					i += drawInfo.passTimings.size();
				}
			}
		}

		int& selectedIdx = _selectedTimelineIdx;

		if (nEffect > 1 || showPasses) {
			ImGui::Spacing();

			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 5));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

			static constexpr ImGuiTableColumnFlags COLUMN_FLAGS = ImGuiTableColumnFlags_WidthStretch |
				ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder;

			if (effectsTotalTime > 0) {
				if (showPasses) {
					if (ImGui::BeginTable("timeline", (int)passCount)) {
						for (uint32_t i = 0; i < passCount; ++i) {
							if (_lastestAvgEffectTimings[i] < 1e-3f) {
								continue;
							}

							ImGui::TableSetupColumn(std::to_string(i).c_str(), COLUMN_FLAGS,
								_lastestAvgEffectTimings[i] / effectsTotalTime);
						}

						ImGui::TableNextRow();

						uint32_t i = 0;
						for (const EffectDrawInfo& drawInfo : effectDrawInfos) {
							for (uint32_t j = 0, end = (uint32_t)drawInfo.passTimings.size(); j < end; ++j) {
								if (drawInfo.passTimings[j] < 1e-5f) {
									continue;
								}

								ImGui::TableNextColumn();

								std::string name;
								if (drawInfo.passTimings.size() == 1) {
									name = std::string(GetEffectDisplayName(*drawInfo.desc));
								} else if (nEffect == 1) {
									name = drawInfo.desc->passes[j].desc;
								} else {
									name = fmt::format("{}/{}",
										GetEffectDisplayName(*drawInfo.desc), drawInfo.desc->passes[j].desc);
								}

								_DrawTimelineItem(itemId, colors[i], name, drawInfo.passTimings[j],
									effectsTotalTime, selectedIdx == (int)i);

								++i;
							}
						}

						ImGui::EndTable();
					}
				} else {
					if (ImGui::BeginTable("timeline", (int)nEffect)) {
						for (uint32_t i = 0; i < nEffect; ++i) {
							if (effectDrawInfos[i].totalTime < 1e-5f) {
								continue;
							}

							ImGui::TableSetupColumn(std::to_string(i).c_str(), COLUMN_FLAGS,
								effectDrawInfos[i].totalTime / effectsTotalTime);
						}

						ImGui::TableNextRow();

						for (uint32_t i = 0; i < nEffect; ++i) {
							const EffectDrawInfo& drawInfo = effectDrawInfos[i];
							if (drawInfo.totalTime < 1e-5f) {
								continue;
							}

							ImGui::TableNextColumn();
							_DrawTimelineItem(itemId, colors[i], GetEffectDisplayName(*drawInfo.desc),
								drawInfo.totalTime, effectsTotalTime, selectedIdx == (int)i);
						}

						ImGui::EndTable();
					}
				}
			} else {
				// A placeholder until the first timings are there
				if (ImGui::BeginTable("timeline", 1)) {
					ImGui::TableSetupColumn("0", COLUMN_FLAGS);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();

					const ImU32 color = ImColor();
					ImGui::PushStyleColor(ImGuiCol_HeaderActive, color);
					ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color);
					ImGui::Selectable("");
					ImGui::PopStyleColor(2);

					ImGui::EndTable();
				}
			}

			ImGui::PopStyleVar(4);

			ImGui::Spacing();
		}

		selectedIdx = -1;

		static constexpr ImGuiTableColumnFlags COLUMN_FLAGS = ImGuiTableColumnFlags_WidthStretch |
			ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder;

		if (ImGui::BeginTable("timings", 1, ImGuiTableFlags_PadOuterX)) {
			ImGui::TableSetupColumn(nullptr, COLUMN_FLAGS);

			if (nEffect == 1) {
				const int hovered = _DrawEffectTimings(itemId, effectDrawInfos[0], showPasses, colors, true);
				if (hovered >= 0) {
					selectedIdx = hovered;
				}
			} else {
				size_t idx = 0;
				for (const EffectDrawInfo& drawInfo : effectDrawInfos) {
					const size_t idxBegin = idx;

					std::span<const uint32_t> colorSpan;
					if (!showPasses || drawInfo.passTimings.size() == 1) {
						colorSpan = std::span<const uint32_t>(colors.data() + idx, 1);
						++idx;
					} else {
						colorSpan = std::span<const uint32_t>(colors.data() + idx, drawInfo.passTimings.size());
						idx += drawInfo.passTimings.size();
					}

					const int hovered = _DrawEffectTimings(itemId, drawInfo, showPasses, colorSpan, false);
					if (hovered >= 0) {
						selectedIdx = (int)idxBegin + hovered;
					}
				}
			}

			ImGui::EndTable();
		}

		if (nEffect > 1) {
			ImGui::Separator();

			if (ImGui::BeginTable("total", 1, ImGuiTableFlags_PadOuterX)) {
				ImGui::TableSetupColumn(nullptr, COLUMN_FLAGS);
				_DrawTimingItem(itemId, Text::PROFILER_TOTAL, nullptr, effectsTotalTime);
				ImGui::EndTable();
			}
		}
	}

	ImGui::End();
	return needRedraw;
}

// The seed depends on the names of the effects only, so that the colours of
// a scaling mode stay the same
static uint32_t GetSeed(const std::vector<const EffectDesc*>& effectDescs) noexcept {
	uint32_t result = 0;
	for (const EffectDesc* effectDesc : effectDescs) {
		result ^= (uint32_t)std::hash<std::string>()(effectDesc->name);
	}
	return result;
}

SmallVector<uint32_t> OverlayDrawer::GenerateTimelineColors(const std::vector<const EffectDesc*>& effectDescs) noexcept {
	const uint32_t nEffect = (uint32_t)effectDescs.size();
	uint32_t totalColors = nEffect > 1 ? nEffect : 0;
	for (uint32_t i = 0; i < nEffect; ++i) {
		const uint32_t nPass = (uint32_t)effectDescs[i]->passes.size();
		if (nPass > 1) {
			totalColors += nPass;
		}
	}

	if (totalColors == 0) {
		return {};
	}

	constexpr uint32_t nColors = (uint32_t)std::size(TIMELINE_COLORS);

	std::default_random_engine randomEngine(GetSeed(effectDescs));
	SmallVector<uint32_t> result;

	if (totalColors <= nColors) {
		result.resize(nColors);
		for (uint32_t i = 0; i < nColors; ++i) {
			result[i] = i;
		}
		std::shuffle(result.begin(), result.end(), randomEngine);

		result.resize(totalColors);
	} else {
		// Neighbouring passes differ, and neighbouring effects differ
		result.resize(totalColors);
		std::uniform_int_distribution<uint32_t> uniformDst(0, nColors - 1);

		if (nEffect <= nColors) {
			if (nEffect > 1) {
				// No colour twice among the effects
				std::array<uint32_t, nColors> effectColors{};
				for (uint32_t i = 0; i < nColors; ++i) {
					effectColors[i] = i;
				}
				std::shuffle(effectColors.begin(), effectColors.end(), randomEngine);

				uint32_t i = 0;
				for (uint32_t j = 0; j < nEffect; ++j) {
					result[i] = effectColors[j];
					++i;

					const uint32_t nPass = (uint32_t)effectDescs[j]->passes.size();
					if (nPass > 1) {
						i += nPass;
					}
				}
			}
		} else {
			// Only different from the effect before
			uint32_t prevColor = std::numeric_limits<uint32_t>::max();
			uint32_t i = 0;
			for (uint32_t j = 0; j < nEffect; ++j) {
				uint32_t c = uniformDst(randomEngine);
				while (c == prevColor) {
					c = uniformDst(randomEngine);
				}

				result[i] = c;
				prevColor = c;
				++i;

				const uint32_t nPass = (uint32_t)effectDescs[j]->passes.size();
				if (nPass > 1) {
					i += nPass;
				}
			}
		}

		// The colours of the passes
		size_t idx = 0;
		for (uint32_t i = 0; i < nEffect; ++i) {
			const uint32_t nPass = (uint32_t)effectDescs[i]->passes.size();

			if (nEffect > 1) {
				++idx;

				if (nPass == 1) {
					continue;
				}
			}

			for (uint32_t j = 0; j < nPass; ++j) {
				uint32_t c = uniformDst(randomEngine);

				if (i > 0 || j > 0) {
					const uint32_t prevColor = (i > 0 && j == 0) ? result[idx - 2] : result[idx - 1];

					if (j + 1 == nPass && i + 1 != nEffect && effectDescs[(size_t)i + 1]->passes.size() == 1) {
						// The last pass of this effect, and the next effect has one pass
						const uint32_t nextColor = result[idx + 1];
						while (c == prevColor || c == nextColor) {
							c = uniformDst(randomEngine);
						}
					} else {
						while (c == prevColor) {
							c = uniformDst(randomEngine);
						}
					}
				}

				result[idx] = c;
				++idx;
			}
		}
	}

	return result;
}

}
