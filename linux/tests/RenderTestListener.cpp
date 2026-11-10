// Shared by the test programs that use Vulkan devices.
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <spdlog/sinks/stdout_sinks.h>
#include "Logger.h"
#include "RenderTestHelpers.h"

using namespace Magpie;
using namespace RenderTest;

// The devices are destroyed at the end of the run and not with the other
// static objects: by then the validation layers have destroyed their own, and
// calling into them crashes.
class DeviceCleanup : public Catch::EventListenerBase {
public:
	using Catch::EventListenerBase::EventListenerBase;

	// Shows what the validation layers and the renderer report
	void testRunStarting(const Catch::TestRunInfo&) override {
		Logger::Get().InitializeWithSink(spdlog::level::warn, std::make_shared<spdlog::sinks::stderr_sink_mt>());
	}

	void testRunEnded(const Catch::TestRunStats&) override {
		TestDevices().clear();
	}
};
CATCH_REGISTER_LISTENER(DeviceCleanup)
