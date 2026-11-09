// Test T9: cursor control for Xwayland clients through the X server.
//
// Under Wayland a client cannot hide or read the system cursor. For source
// windows that are X11 clients (Wine games), the X server offers both:
// XFixesHideCursor and XFixesGetCursorImage. This tool checks what they do
// under Xwayland.
//
// Usage: x11_cursor [--hide SECONDS] [--seconds TOTAL]
// Prints the pointer position and the current cursor image four times a
// second. With --hide, the cursor is hidden after one second for SECONDS.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

static double Mono(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char* argv[]) {
	int hideSeconds = 0;
	int totalSeconds = 6;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--hide") == 0) {
			hideSeconds = atoi(argv[i + 1]);
		} else if (strcmp(argv[i], "--seconds") == 0) {
			totalSeconds = atoi(argv[i + 1]);
		}
	}

	Display* display = XOpenDisplay(NULL);
	if (!display) {
		fprintf(stderr, "cannot open the X display\n");
		return 1;
	}
	int eventBase, errorBase;
	if (!XFixesQueryExtension(display, &eventBase, &errorBase)) {
		printf("RESULT: no XFIXES extension\n");
		return 1;
	}
	int major = 5, minor = 0;
	XFixesQueryVersion(display, &major, &minor);
	printf("XFIXES %d.%d\n", major, minor);

	const Window root = DefaultRootWindow(display);
	const double start = Mono();
	int hidden = 0;

	while (Mono() - start < totalSeconds) {
		const double t = Mono() - start;

		if (hideSeconds > 0 && !hidden && t >= 1.0) {
			XFixesHideCursor(display, root);
			XFlush(display);
			hidden = 1;
			printf("mono=%.2f XFixesHideCursor\n", Mono());
		} else if (hidden == 1 && t >= 1.0 + hideSeconds) {
			XFixesShowCursor(display, root);
			XFlush(display);
			hidden = 2;
			printf("mono=%.2f XFixesShowCursor\n", Mono());
		}

		Window rootReturn, child;
		int rootX, rootY, winX, winY;
		unsigned int mask;
		const Bool onScreen = XQueryPointer(display, root, &rootReturn, &child, &rootX, &rootY, &winX, &winY, &mask);

		XFixesCursorImage* image = XFixesGetCursorImage(display);
		if (image) {
			printf("mono=%.2f pointer=%d,%d on_screen=%d child=0x%lx cursor=%dx%d hot=%d,%d serial=%lu name=%s\n",
				Mono(), rootX, rootY, onScreen, child, image->width, image->height,
				image->xhot, image->yhot, image->cursor_serial, image->name ? image->name : "-");
			XFree(image);
		} else {
			printf("mono=%.2f pointer=%d,%d on_screen=%d child=0x%lx cursor=none\n",
				Mono(), rootX, rootY, onScreen, child);
		}
		fflush(stdout);
		usleep(250000);
	}

	// The hide is also undone by the server when this client disconnects.
	if (hidden == 1) {
		XFixesShowCursor(display, root);
	}
	XCloseDisplay(display);
	return 0;
}
