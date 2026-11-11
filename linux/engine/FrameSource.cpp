#include "FrameSource.h"
#include "Bridge.h"
#include "Logger.h"
#include "PipeWireFrameSource.h"

namespace Magpie {

std::unique_ptr<FrameSource> FrameSource::Create(
	VulkanDevice& device,
	const CaptureSession& capture,
	const Options& options
) noexcept {
	switch (capture.GetKind()) {
	case CaptureSession::Kind::PipeWire:
	{
		auto source = std::make_unique<PipeWireFrameSource>();
		if (!source->Initialize(device, {
			.pipeWireFd = capture.PipeWireFd(),
			.nodeId = capture.NodeId(),
			.fixedSize = options.fixedSize,
			.isDmabufAllowed = options.isDmabufAllowed,
			.isDamageUsed = options.isDamageUsed,
			.cropping = options.cropping,
			.duplicateFrameDetectionMode = options.duplicateFrameDetectionMode,
			.isStatisticsForDynamicDetectionEnabled = options.isStatisticsForDynamicDetectionEnabled
		})) {
			return nullptr;
		}
		return source;
	}
	default:
		Logger::Get().Error("There is no frame source for this kind of screen cast");
		return nullptr;
	}
}

}
