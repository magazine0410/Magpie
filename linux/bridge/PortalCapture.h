// Screen cast through the desktop portal (org.freedesktop.portal.ScreenCast).
// It works on every desktop that has a portal, and the user chooses the
// source in a dialog of the desktop. A restore token lets a later session
// take the same source without the dialog, where the portal supports that.
#pragma once
#include <memory>
#include <string>
#include "Bridge.h"

namespace Magpie {

struct PortalCapture {
	// Values of the portal's interface
	enum SourceType : uint32_t {
		Monitor = 1,
		Window = 2
	};
	enum CursorMode : uint32_t {
		Hidden = 1,
		Embedded = 2,
		Metadata = 4
	};

	struct Info {
		uint32_t version = 0;
		// Bit sets of SourceType and CursorMode
		uint32_t sourceTypes = 0;
		uint32_t cursorModes = 0;
	};

	struct Request {
		// What the dialog offers; a bit set of SourceType
		uint32_t sourceTypes = Window | Monitor;
		// From an earlier session; empty to show the dialog in any case
		std::string restoreToken;
	};

	struct Result {
		std::unique_ptr<CaptureSession> session;
		// For the next session; empty if the portal gave none
		std::string restoreToken;
		// What the user chose, and its size as the portal reports it (zero if
		// it does not)
		uint32_t sourceType = 0;
		Size size;
		// The cursor is part of the picture, because the portal cannot
		// deliver it as metadata
		bool isCursorEmbedded = false;
	};

	// Returns false if no portal with the screen cast interface is running
	static bool Probe(Info& info) noexcept;

	// Blocks until the user has chosen a source or cancelled. Without a
	// session in the result the reason has been logged.
	static Result Start(const Request& request) noexcept;
};

}
